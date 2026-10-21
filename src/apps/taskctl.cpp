#include "ts/client/client.h"
#include "config_file.h"

#include <iostream>

namespace {
void help() {
    std::cout << "taskctl [--host ADDRESS] [--port PORT] [--json] COMMAND\n"
        "  submit [--cpu N] [--memory MB] [--timeout SEC] [--cwd DIR] [--env KEY=VALUE] [--] EXEC [ARGS...]\n"
        "  get|status TASK_ID\n  list\n  workers\n"
        "M7: logs/cancel/retry are unsupported. JSON list output is one response object per page (NDJSON).\n";
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
        } else if constexpr (std::is_same_v<T, protocol::ErrorResponse>) std::cerr << "server error: " << value.message << '\n';
        else throw std::runtime_error("unexpected taskctl response");
    }, response);
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
        std::optional<ts::protocol::Message> request;
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
        } else if (command == "get" || command == "status" || command == "logs" || command == "cancel" || command == "retry") {
            if (argc - i != 1) throw std::invalid_argument("command requires one Task UUID");
            const auto id = ts::domain::TaskId::parse(argv[i++]); if (!id) throw std::invalid_argument(id.error().message);
            if (command == "get" || command == "status") request = ts::protocol::GetTask{id.value()};
            else if (command == "cancel") request = ts::protocol::CancelTask{id.value()};
            else if (command == "retry") request = ts::protocol::RetryTask{id.value()};
            else request = ts::protocol::GetLogs{id.value(), {}, ts::protocol::Stream::Stdout, 0, 262144, {}};
        } else if (command == "workers") request = ts::protocol::ListWorkers{};
        else if (command != "list") throw std::invalid_argument("unknown command");
        if (i != argc) throw std::invalid_argument("unexpected argument");
        auto connected = ts::client::Client::connect(endpoint); if (!connected) throw std::runtime_error(connected.error().message);
        if (command == "list") {
            auto listed = connected.value()->list([&](const auto& page) { display(page, json); });
            if (!listed) throw std::runtime_error(listed.error().message);
        } else {
            auto response = connected.value()->request(std::move(*request)); if (!response) throw std::runtime_error(response.error().message);
            display(response.value(), json); if (std::holds_alternative<ts::protocol::ErrorResponse>(response.value())) return 2;
        }
        return 0;
    } catch (const std::exception& e) { std::cerr << "taskctl: " << e.what() << '\n'; return 1; }
}
