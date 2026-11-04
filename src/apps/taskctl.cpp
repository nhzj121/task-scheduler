#include "ts/client/client.h"
#include "config_file.h"
#include "ts/common/diagnostic_field.h"

#include <iostream>
#include <algorithm>
#include <thread>

namespace {
void help() {
    std::cout << "taskctl [--host ADDRESS] [--port PORT] [--json] COMMAND\n"
        "  submit [--cpu N] [--memory MB] [--timeout SEC] [--cwd DIR] [--env KEY=VALUE] [--] EXEC [ARGS...]\n"
        "  get|status TASK_ID\n  list\n  workers\n"
        "  logs TASK_ID [--attempt N] [--stdout|--stderr|--both] [--limit-bytes N]\n"
        "  cancel TASK_ID\n  retry TASK_ID\n"
        "  wait TASK_ID [--timeout SEC]\n"
        "wait polls once per second; its timeout stops observation, not the task.\n"
        "submit/retry capacity rejection exits 3. wait: success 0, failed/cancelled 1, error 2, timeout 124.\n"
        "JSON list/logs output is one response object per page (NDJSON).\n";
}
void display(const ts::protocol::Message& response, bool json) {
    using namespace ts;
    if (json) {
        const auto encoded = protocol::encode_message(response, 1, protocol::Direction::ControllerToClient);
        if (!encoded) throw std::runtime_error(encoded.error().message);
        std::cout << encoded.value().payload << '\n'; return;
    }
    std::visit([](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, protocol::SubmitTaskResponse>) std::cout << value.task_id.value() << '\n';
        else if constexpr (std::is_same_v<T, protocol::GetTaskResponse>) {
            std::cout << value.task.task_id.value() << ' ' << domain::to_string(value.task.state) << '\n';
            for (const auto& a : value.attempts) std::cout << "  attempt " << a.attempt_no.value() << ' '
                << domain::to_string(a.state) << (a.finalization_pending ? " finalization_pending=true" : "") << '\n';
        } else if constexpr (std::is_same_v<T, protocol::ListTasksResponse>) {
            for (const auto& t : value.tasks) std::cout << t.task_id.value() << ' ' << domain::to_string(t.state)
                << " attempts=" << t.attempt_count << ' ' << t.executable << '\n';
        } else if constexpr (std::is_same_v<T, protocol::ListWorkersResponse>) {
            for (const auto& w : value.workers) std::cout << w.worker_id.value() << ' ' << domain::to_string(w.state)
                << " reserved_cpu=" << w.reserved.cpu_slots() << " attempts=" << w.running_attempts << '\n';
        } else if constexpr (std::is_same_v<T, protocol::CancelTaskResponse>) {
            const char* status = value.status == protocol::CancelStatus::Accepted ? "accepted" :
                value.status == protocol::CancelStatus::Completed ? "completed" : "already_cancelled";
            std::cout << value.task_id.value() << ' ' << status << ' ' << domain::to_string(value.task_state) << '\n';
        } else if constexpr (std::is_same_v<T, protocol::RetryTaskResponse>)
            std::cout << value.task_id.value() << ' ' << domain::to_string(value.state) << " next_attempt=" << value.next_attempt_no.value() << '\n';
        else if constexpr (std::is_same_v<T, protocol::ErrorResponse>) std::cerr << "server error: " << value.message << '\n';
        else throw std::runtime_error("unexpected taskctl response");
    }, response);
}
bool capacity_rejection(const ts::protocol::Message& response) {
    const auto* error = std::get_if<ts::protocol::ErrorResponse>(&response);
    if (!error || error->code != ts::protocol::ErrorCode::InvalidArgument || !error->details.is_object()) return false;
    const auto it = error->details.find("reason");
    return it != error->details.end() && it->is_string() &&
        (*it == "NO_KNOWN_WORKERS" || *it == "NO_KNOWN_WORKER_FITS");
}
int wait_cli(const ts::net::Endpoint& endpoint, bool json, int argc, char** argv, int i) {
    using namespace ts;
    using Clock = std::chrono::steady_clock;
    try {
        if (i == argc) throw std::invalid_argument("wait requires a Task UUID");
        const auto parsed = domain::TaskId::parse(argv[i++]);
        if (!parsed) throw std::invalid_argument(parsed.error().message);
        const auto task = parsed.value();
        std::optional<client::Deadline> deadline;
        if (i < argc) {
            if (std::string(argv[i++]) != "--timeout" || i == argc)
                throw std::invalid_argument("use wait TASK_ID [--timeout SEC]");
            const auto now = Clock::now();
            const auto maximum = std::chrono::duration_cast<std::chrono::seconds>(Clock::time_point::max() - now).count();
            const auto seconds = apps::unsigned_value(argv[i++], static_cast<std::uint64_t>(maximum));
            if (!seconds) throw std::invalid_argument("wait timeout must be positive");
            deadline = client::Deadline{now + std::chrono::seconds(seconds), false};
        }
        if (i != argc) throw std::invalid_argument("unexpected wait argument");
        auto* budget = deadline ? &*deadline : nullptr;
        const auto expired = [&] {
            if (!budget || Clock::now() < budget->at) return false;
            budget->expired = true; return true;
        };
        const auto timed_out = [] { std::cerr << "taskctl: wait deadline expired; task was not cancelled\n"; return 124; };
        auto connection = client::Client::connect(endpoint, std::chrono::seconds(5), budget);
        if (!connection) {
            if (budget && budget->expired) return timed_out();
            throw std::runtime_error(connection.error().message);
        }
        for (;;) {
            if (expired()) return timed_out();
            auto response = connection.value()->request(protocol::GetTask{task}, budget);
            if (!response) {
                if (budget && budget->expired) return timed_out();
                throw std::runtime_error(response.error().message);
            }
            if (std::holds_alternative<protocol::ErrorResponse>(response.value())) {
                display(response.value(), json); return 2;
            }
            const auto* view = std::get_if<protocol::GetTaskResponse>(&response.value());
            if (!view || !(view->task.task_id == task)) throw std::runtime_error("wait response Task ID mismatch");
            if (expired()) return timed_out();
            if (domain::is_terminal(view->task.state)) {
                const protocol::Attempt* latest = nullptr;
                if (view->task.latest_attempt_id) {
                    const auto found = std::find_if(view->attempts.begin(), view->attempts.end(),
                        [&](const auto& a) { return a.attempt_id == *view->task.latest_attempt_id; });
                    if (found == view->attempts.end()) throw std::runtime_error("wait latest recorded Attempt missing");
                    latest = &*found;
                }
                if (expired()) return timed_out();
                if (json) display(response.value(), true);
                else {
                    std::cout << "task_id=" << task.value() << " task_state=" << domain::to_string(view->task.state) << '\n';
                    if (latest) {
                        std::cout << "latest_recorded_attempt_id=" << latest->attempt_id.value()
                            << " attempt_no=" << latest->attempt_no.value() << " attempt_state=" << domain::to_string(latest->state);
                        if (latest->exit_code) std::cout << " exit_code=" << *latest->exit_code;
                        if (latest->term_signal) std::cout << " term_signal=" << *latest->term_signal;
                        if (latest->reason) std::cout << " reason=" << common::diagnostic_field(*latest->reason);
                        std::cout << " logs_complete=" << (latest->logs_complete ? "true" : "false") << '\n';
                    }
                }
                if (!std::cout) throw std::runtime_error("wait result output failed");
                return view->task.state == domain::TaskState::SUCCEEDED ? 0 : 1;
            }
            auto next = Clock::now() + std::chrono::seconds(1);
            if (budget) next = std::min(next, budget->at);
            std::this_thread::sleep_until(next);
        }
    } catch (const std::exception& e) { std::cerr << "taskctl: wait: " << e.what() << '\n'; return 2; }
}
}
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--help") { help(); return 0; }
        ts::net::Endpoint endpoint{"127.0.0.1", 7420}; bool json = false; int i = 1;
        auto value = [&]() -> std::string { if (++i >= argc) throw std::invalid_argument("missing option value"); return argv[i]; };
        for (; i < argc; ++i) {
            const std::string option = argv[i];
            if (option == "--json") json = true;
            else if (option == "--host") endpoint.address = value();
            else if (option == "--port") { const auto n = ts::apps::unsigned_value(value(), 65535); if (!n) throw std::invalid_argument("port zero"); endpoint.port = static_cast<std::uint16_t>(n); }
            else break;
        }
        if (i == argc) throw std::invalid_argument("missing command; use --help");
        const std::string command = argv[i++];
        if (command == "wait") return wait_cli(endpoint, json, argc, argv, i);
        std::optional<ts::protocol::Message> request;
        bool both = false;
        if (command == "submit") {
            std::uint32_t cpu = 1; std::uint64_t memory = 0; ts::protocol::Milliseconds timeout;
            std::optional<std::string> cwd; ts::protocol::Environment env;
            for (; i < argc; ++i) {
                const std::string option = argv[i];
                if (option == "--") { ++i; break; }
                if (option == "--cpu") cpu = static_cast<std::uint32_t>(ts::apps::unsigned_value(value(), UINT32_MAX));
                else if (option == "--memory") memory = ts::apps::unsigned_value(value(), INT64_MAX);
                else if (option == "--timeout") { const auto seconds = ts::apps::unsigned_value(value(), static_cast<std::uint64_t>(INT64_MAX) / 1000); if (!seconds) throw std::invalid_argument("timeout must be positive"); timeout = seconds * 1000; }
                else if (option == "--cwd") cwd = value();
                else if (option == "--env") { const auto e = value(); const auto at = e.find('='); if (at == std::string::npos || !at) throw std::invalid_argument("env requires KEY=VALUE"); env[e.substr(0, at)] = e.substr(at + 1); }
                else if (option.rfind("--", 0) == 0) throw std::invalid_argument("unknown submit option");
                else break;
            }
            if (i == argc) throw std::invalid_argument("missing executable");
            std::string executable = argv[i++]; std::vector<std::string> args;
            for (; i < argc; ++i) args.emplace_back(argv[i]);
            auto resources = ts::domain::ResourceRequest::create(cpu, memory); if (!resources) throw std::invalid_argument(resources.error().message);
            request = ts::protocol::SubmitTask{std::move(executable), std::move(args), std::move(env), std::move(cwd), resources.value(), timeout};
        } else if (command == "logs") {
            if (i == argc) throw std::invalid_argument("logs requires a Task UUID");
            const auto id = ts::domain::TaskId::parse(argv[i++]); if (!id) throw std::invalid_argument(id.error().message);
            ts::protocol::GetLogs log{id.value(), {}, ts::protocol::Stream::Stdout, 0, 262144, {}};
            bool stream_selected = false;
            for (; i < argc; ++i) {
                const std::string option = argv[i];
                if (option == "--attempt") {
                    const auto n = ts::domain::AttemptNumber::create(static_cast<std::uint32_t>(ts::apps::unsigned_value(value(), UINT32_MAX)));
                    if (!n) throw std::invalid_argument(n.error().message);
                    log.attempt_no = n.value();
                } else if (option == "--limit-bytes") {
                    log.limit_bytes = ts::apps::unsigned_value(value()); if (!log.limit_bytes) throw std::invalid_argument("log page size zero");
                } else if (option == "--stdout" || option == "--stderr" || option == "--both") {
                    if (stream_selected) throw std::invalid_argument("choose one stream option");
                    stream_selected = true; both = option == "--both";
                    log.stream = option == "--stderr" ? ts::protocol::Stream::Stderr : ts::protocol::Stream::Stdout;
                } else throw std::invalid_argument("unknown logs option");
            }
            request = log;
        } else if (command == "get" || command == "status" || command == "cancel" || command == "retry") {
            if (argc - i != 1) throw std::invalid_argument("command requires one Task UUID");
            const auto id = ts::domain::TaskId::parse(argv[i++]); if (!id) throw std::invalid_argument(id.error().message);
            if (command == "get" || command == "status") request = ts::protocol::GetTask{id.value()};
            else if (command == "cancel") request = ts::protocol::CancelTask{id.value()};
            else if (command == "retry") request = ts::protocol::RetryTask{id.value()};
        } else if (command == "workers") request = ts::protocol::ListWorkers{};
        else if (command != "list") throw std::invalid_argument("unknown command");
        if (i != argc) throw std::invalid_argument("unexpected argument");
        auto connected = ts::client::Client::connect(endpoint); if (!connected) throw std::runtime_error(connected.error().message);
        if (command == "logs") {
            auto log = std::get<ts::protocol::GetLogs>(*request);
            auto consume = [&](const ts::protocol::GetLogsResponse& page) {
                if (json) display(page, true);
                else {
                    const auto raw = ts::protocol::base64_decode(page.data_base64, static_cast<std::size_t>(ts::protocol::kMaxLogPage));
                    if (!raw) throw std::runtime_error(raw.error().message);
                    std::cout.write(raw.value().data(), static_cast<std::streamsize>(raw.value().size()));
                    if (!std::cout) throw std::runtime_error("binary log output write failed");
                }
            };
            auto fetched = connected.value()->logs(log, consume);
            if (!fetched) throw std::runtime_error(fetched.error().message);
            if (both) {
                log.stream = ts::protocol::Stream::Stderr;
                fetched = connected.value()->logs(log, consume);
                if (!fetched) throw std::runtime_error(fetched.error().message);
            }
        } else if (command == "list") {
            auto listed = connected.value()->list([&](const auto& page) { display(page, json); });
            if (!listed) throw std::runtime_error(listed.error().message);
        } else {
            auto response = connected.value()->request(std::move(*request)); if (!response) throw std::runtime_error(response.error().message);
            display(response.value(), json);
            if ((command == "submit" || command == "retry") && capacity_rejection(response.value())) return 3;
            if (std::holds_alternative<ts::protocol::ErrorResponse>(response.value())) return 2;
        }
        return 0;
    } catch (const std::exception& e) { std::cerr << "taskctl: " << e.what() << '\n'; return 1; }
}
