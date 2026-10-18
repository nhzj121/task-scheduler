#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string> 

#include <nlohmann/json_fwd.hpp>

#include "ts/common/result.h"
#include "ts/domain/ids.h"
#include "ts/domain/resources.h"

namespace ts::domain {

struct EndpointConfig {
    std::string address;
    std::uint16_t port;
};

struct LoggingConfig {
    std::string level;
    std::string destination;
};

struct HeartbeatConfig {
    std::uint64_t interval_ms;
    std::uint64_t offline_timeout_ms;
};

struct DispatchConfig {
    std::uint64_t ack_timeout_ms;
    std::uint64_t start_timeout_ms;
};

struct ProtocolConfig {
    static constexpr std::uint32_t kV1MaximumPayloadBytes = 4U * 1024U * 1024U;
    std::uint32_t max_payload_bytes;
};

struct QueueConfig {
    std::size_t persistence_commands;
    std::size_t log_bytes;
    std::size_t log_commands;
    std::size_t log_control_reserve;
    std::size_t initial_session_log_window_bytes;
    std::size_t initial_session_log_window_chunks;
};

struct ControllerConfig {
    EndpointConfig client_listen;
    EndpointConfig worker_listen;
    std::string data_dir;
    HeartbeatConfig heartbeat;
    DispatchConfig dispatch;
    std::uint64_t termination_grace_ms;
    ProtocolConfig protocol;
    QueueConfig queues;
    LoggingConfig logging;

    static ControllerConfig defaults();
    static common::Result<ControllerConfig> parse(const nlohmann::json& json);
};

struct ReconnectConfig {
    std::uint64_t initial_backoff_ms;
    std::uint64_t max_backoff_ms;
};

struct WorkerConfig {
    WorkerId worker_id;
    EndpointConfig controller;
    ResourceCapacity capacity;
    std::optional<std::uint32_t> max_concurrent_tasks;
    ReconnectConfig reconnect;
    LoggingConfig logging;

    static common::Result<WorkerConfig> parse(const nlohmann::json& json);
};

}  // namespace ts::domain
