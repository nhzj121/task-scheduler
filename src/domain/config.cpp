#include "ts/domain/config.h"

#include <algorithm>
#include <array>
#include <limits>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

namespace ts::domain {
namespace {

using Json = nlohmann::json;
using namespace std::string_view_literals;

common::Result<void> require_object(const Json& value, std::string_view path) {
    if (!value.is_object()) {
        return common::Result<void>::failure(std::string(path) + " must be an object");
    }
    return common::Result<void>::success();
}

template <std::size_t Size>
common::Result<void> reject_unknown(const Json& object,
                                    const std::array<std::string_view, Size>& allowed,
                                    std::string_view path) {
    for (auto iterator = object.cbegin(); iterator != object.cend(); ++iterator) {
        const std::string_view key(iterator.key());
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
            return common::Result<void>::failure(std::string(path) +
                                                 " contains unknown field '" + iterator.key() +
                                                 "'");
        }
    }
    return common::Result<void>::success();
}

common::Result<std::uint64_t> parse_unsigned(const Json& value, std::uint64_t minimum,
                                             std::uint64_t maximum,
                                             std::string_view path) {
    std::uint64_t parsed = 0U;
    if (value.is_number_unsigned()) {
        parsed = value.get<std::uint64_t>();
    } else if (value.is_number_integer()) {
        const auto signed_value = value.get<std::int64_t>();
        if (signed_value < 0) {
            return common::Result<std::uint64_t>::failure(std::string(path) +
                                                          " must not be negative");
        }
        parsed = static_cast<std::uint64_t>(signed_value);
    } else {
        return common::Result<std::uint64_t>::failure(std::string(path) +
                                                      " must be an integer");
    }

    if (parsed < minimum || parsed > maximum) {
        return common::Result<std::uint64_t>::failure(std::string(path) +
                                                      " is outside its allowed range");
    }
    return common::Result<std::uint64_t>::success(parsed);
}

common::Result<std::string> parse_nonempty_string(const Json& value, std::string_view path) {
    if (!value.is_string()) {
        return common::Result<std::string>::failure(std::string(path) + " must be a string");
    }
    std::string parsed = value.get<std::string>();
    if (parsed.empty() || parsed.find('\0') != std::string::npos) {
        return common::Result<std::string>::failure(
            std::string(path) + " must be nonempty and contain no NUL");
    }
    return common::Result<std::string>::success(std::move(parsed));
}

common::Result<EndpointConfig> parse_endpoint(const Json& value, EndpointConfig defaults,
                                              std::string_view path) {
    if (const auto result = require_object(value, path); !result) {
        return common::Result<EndpointConfig>::failure(result.error().message);
    }
    constexpr std::array allowed{"address"sv, "port"sv};
    if (const auto result = reject_unknown(value, allowed, path); !result) {
        return common::Result<EndpointConfig>::failure(result.error().message);
    }
    if (value.contains("address")) {
        auto address = parse_nonempty_string(value.at("address"),
                                             std::string(path) + ".address");
        if (!address) {
            return common::Result<EndpointConfig>::failure(address.error().message);
        }
        defaults.address = std::move(address).value();
    }
    if (value.contains("port")) {
        auto port = parse_unsigned(value.at("port"), 1U, 65'535U,
                                   std::string(path) + ".port");
        if (!port) {
            return common::Result<EndpointConfig>::failure(port.error().message);
        }
        defaults.port = static_cast<std::uint16_t>(port.value());
    }
    return common::Result<EndpointConfig>::success(std::move(defaults));
}

common::Result<LoggingConfig> parse_logging(const Json& value, LoggingConfig defaults,
                                            std::string_view path) {
    if (const auto result = require_object(value, path); !result) {
        return common::Result<LoggingConfig>::failure(result.error().message);
    }
    constexpr std::array allowed{"level"sv, "destination"sv};
    if (const auto result = reject_unknown(value, allowed, path); !result) {
        return common::Result<LoggingConfig>::failure(result.error().message);
    }
    if (value.contains("level")) {
        auto level = parse_nonempty_string(value.at("level"), std::string(path) + ".level");
        if (!level) {
            return common::Result<LoggingConfig>::failure(level.error().message);
        }
        constexpr std::array levels{"trace"sv, "debug"sv, "info"sv, "warn"sv,
                                    "error"sv, "critical"sv, "off"sv};
        if (std::find(levels.begin(), levels.end(), level.value()) == levels.end()) {
            return common::Result<LoggingConfig>::failure(std::string(path) +
                                                          ".level is unsupported");
        }
        defaults.level = std::move(level).value();
    }
    if (value.contains("destination")) {
        auto destination = parse_nonempty_string(value.at("destination"),
                                                 std::string(path) + ".destination");
        if (!destination) {
            return common::Result<LoggingConfig>::failure(destination.error().message);
        }
        defaults.destination = std::move(destination).value();
    }
    return common::Result<LoggingConfig>::success(std::move(defaults));
}

common::Result<std::size_t> parse_size(const Json& value, std::size_t minimum,
                                       std::string_view path) {
    auto parsed = parse_unsigned(value, static_cast<std::uint64_t>(minimum),
                                 static_cast<std::uint64_t>(
                                     std::numeric_limits<std::size_t>::max()),
                                 path);
    if (!parsed) {
        return common::Result<std::size_t>::failure(parsed.error().message);
    }
    return common::Result<std::size_t>::success(static_cast<std::size_t>(parsed.value()));
}

common::Result<void> parse_heartbeat(const Json& value, HeartbeatConfig& config) {
    constexpr std::string_view path = "heartbeat";
    if (const auto result = require_object(value, path); !result) {
        return result;
    }
    constexpr std::array allowed{"interval_ms"sv, "offline_timeout_ms"sv};
    if (const auto result = reject_unknown(value, allowed, path); !result) {
        return result;
    }
    if (value.contains("interval_ms")) {
        auto parsed = parse_unsigned(value.at("interval_ms"), 1U,
                                     std::numeric_limits<std::uint64_t>::max(),
                                     "heartbeat.interval_ms");
        if (!parsed) {
            return common::Result<void>::failure(parsed.error().message);
        }
        config.interval_ms = parsed.value();
    }
    if (value.contains("offline_timeout_ms")) {
        auto parsed = parse_unsigned(value.at("offline_timeout_ms"), 1U,
                                     std::numeric_limits<std::uint64_t>::max(),
                                     "heartbeat.offline_timeout_ms");
        if (!parsed) {
            return common::Result<void>::failure(parsed.error().message);
        }
        config.offline_timeout_ms = parsed.value();
    }
    if (config.offline_timeout_ms <= config.interval_ms) {
        return common::Result<void>::failure(
            "heartbeat.offline_timeout_ms must be greater than heartbeat.interval_ms");
    }
    return common::Result<void>::success();
}

common::Result<void> parse_dispatch(const Json& value, DispatchConfig& config) {
    constexpr std::string_view path = "dispatch";
    if (const auto result = require_object(value, path); !result) {
        return result;
    }
    constexpr std::array allowed{"ack_timeout_ms"sv, "start_timeout_ms"sv};
    if (const auto result = reject_unknown(value, allowed, path); !result) {
        return result;
    }
    if (value.contains("ack_timeout_ms")) {
        auto parsed = parse_unsigned(value.at("ack_timeout_ms"), 1U,
                                     std::numeric_limits<std::uint64_t>::max(),
                                     "dispatch.ack_timeout_ms");
        if (!parsed) {
            return common::Result<void>::failure(parsed.error().message);
        }
        config.ack_timeout_ms = parsed.value();
    }
    if (value.contains("start_timeout_ms")) {
        auto parsed = parse_unsigned(value.at("start_timeout_ms"), 1U,
                                     std::numeric_limits<std::uint64_t>::max(),
                                     "dispatch.start_timeout_ms");
        if (!parsed) {
            return common::Result<void>::failure(parsed.error().message);
        }
        config.start_timeout_ms = parsed.value();
    }
    return common::Result<void>::success();
}

common::Result<void> parse_protocol(const Json& value, ProtocolConfig& config) {
    constexpr std::string_view path = "protocol";
    if (const auto result = require_object(value, path); !result) {
        return result;
    }
    constexpr std::array allowed{"max_payload_bytes"sv};
    if (const auto result = reject_unknown(value, allowed, path); !result) {
        return result;
    }
    if (value.contains("max_payload_bytes")) {
        auto parsed = parse_unsigned(value.at("max_payload_bytes"), 1U,
                                     std::numeric_limits<std::uint32_t>::max(),
                                     "protocol.max_payload_bytes");
        if (!parsed) {
            return common::Result<void>::failure(parsed.error().message);
        }
        config.max_payload_bytes = static_cast<std::uint32_t>(parsed.value());
    }
    if (config.max_payload_bytes != ProtocolConfig::kV1MaximumPayloadBytes) {
        return common::Result<void>::failure(
            "protocol.max_payload_bytes must equal the frozen v1 limit of 4194304");
    }
    return common::Result<void>::success();
}

common::Result<void> parse_queues(const Json& value, QueueConfig& config) {
    constexpr std::string_view path = "queues";
    if (const auto result = require_object(value, path); !result) {
        return result;
    }
    constexpr std::array allowed{
        "persistence_commands"sv, "log_bytes"sv, "log_commands"sv,
        "log_control_reserve"sv, "initial_session_log_window_bytes"sv,
        "initial_session_log_window_chunks"sv};
    if (const auto result = reject_unknown(value, allowed, path); !result) {
        return result;
    }

    struct Field {
        std::string_view name;
        std::size_t QueueConfig::*member;
        std::size_t minimum;
    };
    constexpr std::array fields{
        Field{"persistence_commands", &QueueConfig::persistence_commands, 1U},
        Field{"log_bytes", &QueueConfig::log_bytes, 1U},
        Field{"log_commands", &QueueConfig::log_commands, 1U},
        Field{"log_control_reserve", &QueueConfig::log_control_reserve, 1U},
        Field{"initial_session_log_window_bytes",
              &QueueConfig::initial_session_log_window_bytes, 1U},
        Field{"initial_session_log_window_chunks",
              &QueueConfig::initial_session_log_window_chunks, 1U},
    };
    for (const auto& field : fields) {
        if (!value.contains(std::string(field.name))) {
            continue;
        }
        auto parsed = parse_size(value.at(std::string(field.name)), field.minimum,
                                 "queues." + std::string(field.name));
        if (!parsed) {
            return common::Result<void>::failure(parsed.error().message);
        }
        config.*(field.member) = parsed.value();
    }

    if (config.log_control_reserve >= config.log_commands) {
        return common::Result<void>::failure(
            "queues.log_control_reserve must be smaller than queues.log_commands");
    }
    if (config.initial_session_log_window_bytes > config.log_bytes) {
        return common::Result<void>::failure(
            "initial log byte window exceeds queues.log_bytes");
    }
    const std::size_t grantable_commands = config.log_commands - config.log_control_reserve;
    if (config.initial_session_log_window_chunks > grantable_commands) {
        return common::Result<void>::failure(
            "initial log chunk window exceeds grantable log command capacity");
    }
    return common::Result<void>::success();
}

common::Result<ReconnectConfig> parse_reconnect(const Json& value, ReconnectConfig defaults) {
    constexpr std::string_view path = "reconnect";
    if (const auto result = require_object(value, path); !result) {
        return common::Result<ReconnectConfig>::failure(result.error().message);
    }
    constexpr std::array allowed{"initial_backoff_ms"sv, "max_backoff_ms"sv};
    if (const auto result = reject_unknown(value, allowed, path); !result) {
        return common::Result<ReconnectConfig>::failure(result.error().message);
    }
    if (value.contains("initial_backoff_ms")) {
        auto parsed = parse_unsigned(value.at("initial_backoff_ms"), 1U,
                                     std::numeric_limits<std::uint64_t>::max(),
                                     "reconnect.initial_backoff_ms");
        if (!parsed) {
            return common::Result<ReconnectConfig>::failure(parsed.error().message);
        }
        defaults.initial_backoff_ms = parsed.value();
    }
    if (value.contains("max_backoff_ms")) {
        auto parsed = parse_unsigned(value.at("max_backoff_ms"), 1U,
                                     std::numeric_limits<std::uint64_t>::max(),
                                     "reconnect.max_backoff_ms");
        if (!parsed) {
            return common::Result<ReconnectConfig>::failure(parsed.error().message);
        }
        defaults.max_backoff_ms = parsed.value();
    }
    if (defaults.initial_backoff_ms > defaults.max_backoff_ms) {
        return common::Result<ReconnectConfig>::failure(
            "reconnect.initial_backoff_ms must not exceed reconnect.max_backoff_ms");
    }
    return common::Result<ReconnectConfig>::success(defaults);
}

}  // namespace

ControllerConfig ControllerConfig::defaults() {
    return ControllerConfig{
        EndpointConfig{"127.0.0.1", 7420U},
        EndpointConfig{"127.0.0.1", 7421U},
        "/var/lib/task-scheduler/controller",
        HeartbeatConfig{3'000U, 10'000U},
        DispatchConfig{5'000U, 10'000U},
        2'000U,
        ProtocolConfig{ProtocolConfig::kV1MaximumPayloadBytes},
        QueueConfig{4'096U, 67'108'864U, 4'096U, 128U, 1'048'576U, 64U},
        LoggingConfig{"info", "journald"},
    };
}

common::Result<ControllerConfig> ControllerConfig::parse(const Json& json) {
    if (const auto result = require_object(json, "controller config"); !result) {
        return common::Result<ControllerConfig>::failure(result.error().message);
    }
    constexpr std::array allowed{
        "client_listen"sv, "worker_listen"sv, "data_dir"sv, "heartbeat"sv,
        "dispatch"sv, "termination_grace_ms"sv, "protocol"sv, "queues"sv, "logging"sv};
    if (const auto result = reject_unknown(json, allowed, "controller config"); !result) {
        return common::Result<ControllerConfig>::failure(result.error().message);
    }

    ControllerConfig config = defaults();
    if (json.contains("client_listen")) {
        auto parsed = parse_endpoint(json.at("client_listen"), config.client_listen,
                                     "client_listen");
        if (!parsed) {
            return common::Result<ControllerConfig>::failure(parsed.error().message);
        }
        config.client_listen = std::move(parsed).value();
    }
    if (json.contains("worker_listen")) {
        auto parsed = parse_endpoint(json.at("worker_listen"), config.worker_listen,
                                     "worker_listen");
        if (!parsed) {
            return common::Result<ControllerConfig>::failure(parsed.error().message);
        }
        config.worker_listen = std::move(parsed).value();
    }
    if (json.contains("data_dir")) {
        auto parsed = parse_nonempty_string(json.at("data_dir"), "data_dir");
        if (!parsed) {
            return common::Result<ControllerConfig>::failure(parsed.error().message);
        }
        if (parsed.value().front() != '/') {
            return common::Result<ControllerConfig>::failure(
                "data_dir must be an absolute Linux path");
        }
        config.data_dir = std::move(parsed).value();
    }
    if (json.contains("heartbeat")) {
        if (const auto result = parse_heartbeat(json.at("heartbeat"), config.heartbeat); !result) {
            return common::Result<ControllerConfig>::failure(result.error().message);
        }
    }
    if (json.contains("dispatch")) {
        if (const auto result = parse_dispatch(json.at("dispatch"), config.dispatch); !result) {
            return common::Result<ControllerConfig>::failure(result.error().message);
        }
    }
    if (json.contains("termination_grace_ms")) {
        auto parsed = parse_unsigned(json.at("termination_grace_ms"), 1U,
                                     std::numeric_limits<std::uint64_t>::max(),
                                     "termination_grace_ms");
        if (!parsed) {
            return common::Result<ControllerConfig>::failure(parsed.error().message);
        }
        config.termination_grace_ms = parsed.value();
    }
    if (json.contains("protocol")) {
        if (const auto result = parse_protocol(json.at("protocol"), config.protocol); !result) {
            return common::Result<ControllerConfig>::failure(result.error().message);
        }
    }
    if (json.contains("queues")) {
        if (const auto result = parse_queues(json.at("queues"), config.queues); !result) {
            return common::Result<ControllerConfig>::failure(result.error().message);
        }
    }
    if (json.contains("logging")) {
        auto parsed = parse_logging(json.at("logging"), config.logging, "logging");
        if (!parsed) {
            return common::Result<ControllerConfig>::failure(parsed.error().message);
        }
        config.logging = std::move(parsed).value();
    }
    return common::Result<ControllerConfig>::success(std::move(config));
}

common::Result<WorkerConfig> WorkerConfig::parse(const Json& json) {
    if (const auto result = require_object(json, "worker config"); !result) {
        return common::Result<WorkerConfig>::failure(result.error().message);
    }
    constexpr std::array allowed{"worker_id"sv, "controller"sv, "capacity"sv,
                                 "max_concurrent_tasks"sv, "reconnect"sv, "logging"sv};
    if (const auto result = reject_unknown(json, allowed, "worker config"); !result) {
        return common::Result<WorkerConfig>::failure(result.error().message);
    }
    if (!json.contains("worker_id") || !json.contains("capacity")) {
        return common::Result<WorkerConfig>::failure(
            "worker config requires worker_id and capacity");
    }

    auto worker_id_text = parse_nonempty_string(json.at("worker_id"), "worker_id");
    if (!worker_id_text) {
        return common::Result<WorkerConfig>::failure(worker_id_text.error().message);
    }
    auto worker_id = WorkerId::parse(std::move(worker_id_text).value());
    if (!worker_id) {
        return common::Result<WorkerConfig>::failure(worker_id.error().message);
    }

    const Json& capacity_json = json.at("capacity");
    if (const auto result = require_object(capacity_json, "capacity"); !result) {
        return common::Result<WorkerConfig>::failure(result.error().message);
    }
    constexpr std::array capacity_allowed{"cpu_slots"sv, "memory_mb"sv};
    if (const auto result = reject_unknown(capacity_json, capacity_allowed, "capacity"); !result) {
        return common::Result<WorkerConfig>::failure(result.error().message);
    }
    if (!capacity_json.contains("cpu_slots") || !capacity_json.contains("memory_mb")) {
        return common::Result<WorkerConfig>::failure(
            "capacity requires cpu_slots and memory_mb");
    }
    auto cpu = parse_unsigned(capacity_json.at("cpu_slots"), 1U,
                              std::numeric_limits<std::uint32_t>::max(),
                              "capacity.cpu_slots");
    auto memory = parse_unsigned(capacity_json.at("memory_mb"), 0U,
                                 std::numeric_limits<std::uint64_t>::max(),
                                 "capacity.memory_mb");
    if (!cpu) {
        return common::Result<WorkerConfig>::failure(cpu.error().message);
    }
    if (!memory) {
        return common::Result<WorkerConfig>::failure(memory.error().message);
    }
    auto capacity = ResourceCapacity::create(static_cast<std::uint32_t>(cpu.value()),
                                             memory.value());
    if (!capacity) {
        return common::Result<WorkerConfig>::failure(capacity.error().message);
    }

    EndpointConfig controller{"127.0.0.1", 7421U};
    if (json.contains("controller")) {
        auto parsed = parse_endpoint(json.at("controller"), controller, "controller");
        if (!parsed) {
            return common::Result<WorkerConfig>::failure(parsed.error().message);
        }
        controller = std::move(parsed).value();
    }

    std::optional<std::uint32_t> max_concurrent_tasks;
    if (json.contains("max_concurrent_tasks") && !json.at("max_concurrent_tasks").is_null()) {
        auto parsed = parse_unsigned(json.at("max_concurrent_tasks"), 1U,
                                     std::numeric_limits<std::uint32_t>::max(),
                                     "max_concurrent_tasks");
        if (!parsed) {
            return common::Result<WorkerConfig>::failure(parsed.error().message);
        }
        max_concurrent_tasks = static_cast<std::uint32_t>(parsed.value());
    }

    ReconnectConfig reconnect{250U, 30'000U};
    if (json.contains("reconnect")) {
        auto parsed = parse_reconnect(json.at("reconnect"), reconnect);
        if (!parsed) {
            return common::Result<WorkerConfig>::failure(parsed.error().message);
        }
        reconnect = parsed.value();
    }

    LoggingConfig logging{"info", "journald"};
    if (json.contains("logging")) {
        auto parsed = parse_logging(json.at("logging"), logging, "logging");
        if (!parsed) {
            return common::Result<WorkerConfig>::failure(parsed.error().message);
        }
        logging = std::move(parsed).value();
    }

    return common::Result<WorkerConfig>::success(
        WorkerConfig{std::move(worker_id).value(), std::move(controller),
                     std::move(capacity).value(), max_concurrent_tasks, reconnect,
                     std::move(logging)});
}

}  // namespace ts::domain
