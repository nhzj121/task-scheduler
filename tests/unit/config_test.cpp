#include "ts/domain/config.h"

#include <cstdint>
#include <limits>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace ts::domain {
namespace {

using Json = nlohmann::json;

TEST(Config, ControllerEmptyObjectUsesSafeFrozenDefaults) {
    const auto parsed = ControllerConfig::parse(Json::object());
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed.value().client_listen.address, "127.0.0.1");
    EXPECT_EQ(parsed.value().client_listen.port, 7420U);
    EXPECT_EQ(parsed.value().worker_listen.address, "127.0.0.1");
    EXPECT_EQ(parsed.value().worker_listen.port, 7421U);
    EXPECT_EQ(parsed.value().protocol.max_payload_bytes,
              ProtocolConfig::kV1MaximumPayloadBytes);
    EXPECT_GT(parsed.value().heartbeat.offline_timeout_ms,
              parsed.value().heartbeat.interval_ms);
}

TEST(Config, ParsesDocumentedControllerShapeWithoutCoercion) {
    const Json json = Json::parse(R"json({
      "client_listen":{"address":"127.0.0.1","port":7420},
      "worker_listen":{"address":"10.0.0.5","port":7421},
      "data_dir":"/srv/task-scheduler",
      "heartbeat":{"interval_ms":3000,"offline_timeout_ms":10000},
      "dispatch":{"ack_timeout_ms":5000,"start_timeout_ms":10000},
      "termination_grace_ms":2000,
      "protocol":{"max_payload_bytes":4194304},
      "queues":{"persistence_commands":4096,"log_bytes":67108864,
        "log_commands":4096,"log_control_reserve":128,
        "initial_session_log_window_bytes":1048576,
        "initial_session_log_window_chunks":64},
      "logging":{"level":"info","destination":"journald"}
    })json");
    const auto parsed = ControllerConfig::parse(json);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed.value().worker_listen.address, "10.0.0.5");
    EXPECT_EQ(parsed.value().data_dir, "/srv/task-scheduler");
    EXPECT_EQ(parsed.value().queues.log_control_reserve, 128U);
}

TEST(Config, RejectsUnknownFieldsAndWrongTypesAtEveryLevel) {
    EXPECT_FALSE(ControllerConfig::parse(Json{{"unknown", 1}}));
    EXPECT_FALSE(ControllerConfig::parse(Json{{"heartbeat", Json{{"extra", 1}}}}));
    EXPECT_FALSE(ControllerConfig::parse(Json{{"client_listen", Json{{"port", "7420"}}}}));
    EXPECT_FALSE(ControllerConfig::parse(Json{{"heartbeat", true}}));
    EXPECT_FALSE(ControllerConfig::parse(Json{{"termination_grace_ms", 1.5}}));
}

TEST(Config, RejectsInvalidControllerRangesAndRelationships) {
    EXPECT_FALSE(ControllerConfig::parse(
        Json{{"client_listen", Json{{"address", ""}}}}));
    EXPECT_FALSE(ControllerConfig::parse(Json{{"client_listen", Json{{"port", 0}}}}));
    EXPECT_FALSE(ControllerConfig::parse(Json{{"client_listen", Json{{"port", 65'536}}}}));
    EXPECT_FALSE(ControllerConfig::parse(
        Json{{"heartbeat", Json{{"interval_ms", 3'000}, {"offline_timeout_ms", 3'000}}}}));
    EXPECT_FALSE(ControllerConfig::parse(
        Json{{"protocol", Json{{"max_payload_bytes", 4'194'303}}}}));
    EXPECT_FALSE(ControllerConfig::parse(Json{{"data_dir", "relative/path"}}));
}

TEST(Config, RejectsQueueValuesThatBreakCreditReservations) {
    EXPECT_FALSE(ControllerConfig::parse(
        Json{{"queues", Json{{"log_commands", 8}, {"log_control_reserve", 8}}}}));
    EXPECT_FALSE(ControllerConfig::parse(
        Json{{"queues", Json{{"log_bytes", 100},
                              {"initial_session_log_window_bytes", 101}}}}));
    EXPECT_FALSE(ControllerConfig::parse(
        Json{{"queues", Json{{"log_commands", 10}, {"log_control_reserve", 4},
                              {"initial_session_log_window_chunks", 7}}}}));
}

TEST(Config, ParsesWorkerConfigAndPreservesZeroMemoryCapacity) {
    const Json json = Json::parse(R"json({
      "worker_id":"worker-a",
      "controller":{"address":"127.0.0.1","port":7421},
      "capacity":{"cpu_slots":8,"memory_mb":0},
      "max_concurrent_tasks":8,
      "reconnect":{"initial_backoff_ms":250,"max_backoff_ms":30000},
      "logging":{"level":"debug","destination":"journald"}
    })json");
    const auto parsed = WorkerConfig::parse(json);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed.value().worker_id.value(), "worker-a");
    EXPECT_EQ(parsed.value().capacity.cpu_slots(), 8U);
    EXPECT_EQ(parsed.value().capacity.memory_mb(), 0U);
    ASSERT_TRUE(parsed.value().max_concurrent_tasks.has_value());
    EXPECT_EQ(*parsed.value().max_concurrent_tasks, 8U);
}

TEST(Config, WorkerRequiresIdentityAndCapacityAndValidReconnectBounds) {
    EXPECT_FALSE(WorkerConfig::parse(Json::object()));
    EXPECT_FALSE(WorkerConfig::parse(
        Json{{"worker_id", "worker-a"}, {"capacity", Json{{"cpu_slots", 0},
                                                                  {"memory_mb", 1}}}}));
    EXPECT_FALSE(WorkerConfig::parse(
        Json{{"worker_id", "worker-a"}, {"capacity", Json{{"cpu_slots", 1},
                                                                  {"memory_mb", 1}}},
              {"max_concurrent_tasks", 0}}));
    EXPECT_FALSE(WorkerConfig::parse(
        Json{{"worker_id", "worker-a"}, {"capacity", Json{{"cpu_slots", 1},
                                                                  {"memory_mb", 1}}},
              {"reconnect", Json{{"initial_backoff_ms", 2'000},
                                  {"max_backoff_ms", 1'000}}}}));
}

}  // namespace
}  // namespace ts::domain
