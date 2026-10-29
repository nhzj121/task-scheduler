cmake_minimum_required(VERSION 3.20)

project(task_scheduler VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

find_package(Threads REQUIRED)
find_package(nlohmann_json 3.10 REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(spdlog CONFIG REQUIRED)

function(ts_enable_project_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /WX /permissive-)
  else()
    target_compile_options(
      ${target}
      PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Werror
    )
  endif()
endfunction()

add_library(ts_common STATIC
  src/common/timestamp.cpp
  src/common/unique_fd.cpp
)
target_include_directories(ts_common PUBLIC include)
target_compile_features(ts_common PUBLIC cxx_std_17)
ts_enable_project_warnings(ts_common)

add_library(ts_domain STATIC
  src/domain/config.cpp
  src/domain/ids.cpp
  src/domain/resources.cpp
  src/domain/states.cpp
)
target_include_directories(ts_domain PUBLIC include)
target_compile_features(ts_domain PUBLIC cxx_std_17)
target_link_libraries(ts_domain PUBLIC ts_common nlohmann_json::nlohmann_json)
ts_enable_project_warnings(ts_domain)

add_library(ts_io STATIC
  src/io/event_fd.cpp
  src/io/event_loop.cpp
  src/io/signal_fd.cpp
  src/io/timer_fd.cpp
)
target_include_directories(ts_io PUBLIC include)
target_compile_features(ts_io PUBLIC cxx_std_17)
target_link_libraries(ts_io PUBLIC ts_common)
ts_enable_project_warnings(ts_io)

add_library(ts_runtime STATIC
  src/runtime/prepared_exec.cpp
  src/runtime/process_handle.cpp
  src/runtime/process_runtime.cpp
)
target_include_directories(ts_runtime PUBLIC include)
target_compile_features(ts_runtime PUBLIC cxx_std_17)
target_link_libraries(ts_runtime PUBLIC ts_common ts_io)
ts_enable_project_warnings(ts_runtime)

add_library(ts_protocol STATIC src/protocol/frame.cpp src/protocol/messages.cpp)
target_include_directories(ts_protocol PUBLIC include)
target_link_libraries(ts_protocol PUBLIC ts_common ts_domain nlohmann_json::nlohmann_json)
ts_enable_project_warnings(ts_protocol)

add_library(ts_net STATIC
  src/net/output_queue.cpp
  src/net/connection_manager.cpp
)
target_include_directories(ts_net PUBLIC include)
target_link_libraries(ts_net PUBLIC ts_common ts_io)
ts_enable_project_warnings(ts_net)

add_library(ts_storage STATIC
  src/storage/types.cpp
  src/storage/state_store.cpp
  src/storage/persistence_thread.cpp
)
target_include_directories(ts_storage PUBLIC include)
target_link_libraries(ts_storage PUBLIC ts_common ts_domain SQLite::SQLite3 nlohmann_json::nlohmann_json Threads::Threads)
ts_enable_project_warnings(ts_storage)

# Integration composition is separate: ts_storage never depends on ts_io.
add_library(ts_persistence STATIC src/persistence/channel.cpp)
target_include_directories(ts_persistence PUBLIC include)
target_link_libraries(ts_persistence PUBLIC ts_storage ts_io)
ts_enable_project_warnings(ts_persistence)

add_library(ts_scheduler STATIC src/scheduler/scheduler.cpp)
target_include_directories(ts_scheduler PUBLIC include)
target_link_libraries(ts_scheduler PUBLIC ts_domain)
ts_enable_project_warnings(ts_scheduler)

add_library(ts_controller_state STATIC src/controller/resource_ledger.cpp src/controller/worker_registry.cpp src/controller/task_manager.cpp)
target_include_directories(ts_controller_state PUBLIC include)
target_link_libraries(ts_controller_state PUBLIC ts_scheduler ts_storage)
ts_enable_project_warnings(ts_controller_state)

add_library(ts_controller STATIC src/controller/views.cpp src/controller/application.cpp src/controller/log_credit_ledger.cpp src/controller/log_coordinator.cpp)
add_library(ts_logs STATIC src/logs/writer.cpp)
target_include_directories(ts_logs PUBLIC include)
target_link_libraries(ts_logs PUBLIC ts_common ts_domain ts_io Threads::Threads)
ts_enable_project_warnings(ts_logs)
target_include_directories(ts_controller PUBLIC include)
target_link_libraries(ts_controller PUBLIC ts_controller_state ts_persistence ts_net ts_protocol ts_logs spdlog::spdlog)
ts_enable_project_warnings(ts_controller)
add_executable(controller src/apps/controller.cpp)
target_link_libraries(controller PRIVATE ts_controller)
ts_enable_project_warnings(controller)

add_library(ts_client STATIC src/client/client.cpp)
target_include_directories(ts_client PUBLIC include)
target_link_libraries(ts_client PUBLIC ts_net ts_protocol)
ts_enable_project_warnings(ts_client)
add_executable(taskctl src/apps/taskctl.cpp)
target_link_libraries(taskctl PRIVATE ts_client)
ts_enable_project_warnings(taskctl)

add_library(ts_worker STATIC src/worker/execution_manager.cpp src/worker/application.cpp)
target_include_directories(ts_worker PUBLIC include)
target_link_libraries(ts_worker PUBLIC ts_net ts_protocol ts_runtime spdlog::spdlog)
ts_enable_project_warnings(ts_worker)
add_executable(worker src/apps/worker.cpp)
target_link_libraries(worker PRIVATE ts_worker)
ts_enable_project_warnings(worker)

if(BUILD_TESTING)
  find_package(GTest 1.11 REQUIRED)
  include(GoogleTest)

  add_executable(ts_milestone8_tests tests/component/log_writer_test.cpp tests/unit/log_credit_test.cpp tests/component/log_coordinator_test.cpp)
  target_link_libraries(ts_milestone8_tests PRIVATE ts_logs ts_controller GTest::gtest_main)
  ts_enable_project_warnings(ts_milestone8_tests)
  gtest_discover_tests(ts_milestone8_tests PROPERTIES LABELS m8 TIMEOUT 20)

  add_executable(ts_milestone7_tests tests/component/m7_runtime_support_test.cpp tests/unit/m7_staging_test.cpp tests/legacy/provisional_staging.cpp)
  target_include_directories(ts_milestone7_tests PRIVATE tests/legacy/include)
  target_link_libraries(ts_milestone7_tests PRIVATE ts_runtime ts_controller ts_client GTest::gtest_main)
  ts_enable_project_warnings(ts_milestone7_tests)
  gtest_discover_tests(ts_milestone7_tests NO_PRETTY_VALUES PROPERTIES LABELS "m7;component" TIMEOUT 20)

  add_executable(ts_m7_fault_controller tests/helpers/m7_fault_controller.cpp)
  target_link_libraries(ts_m7_fault_controller PRIVATE ts_controller)
  ts_enable_project_warnings(ts_m7_fault_controller)
  find_program(TS_PYTHON_EXECUTABLE NAMES python3 REQUIRED)
  add_executable(ts_milestone9_tests tests/component/m9_execution_test.cpp tests/component/m9_state_test.cpp)
  target_link_libraries(ts_milestone9_tests PRIVATE ts_worker ts_controller_state GTest::gtest_main)
  target_compile_definitions(ts_milestone9_tests PRIVATE TS_RUNTIME_HELPER_PATH="$<TARGET_FILE:ts_runtime_test_helper>")
  add_dependencies(ts_milestone9_tests ts_runtime_test_helper)
  ts_enable_project_warnings(ts_milestone9_tests)
  gtest_discover_tests(ts_milestone9_tests PROPERTIES LABELS "m9;component" TIMEOUT 15)
  add_executable(ts_m9_fault_controller tests/helpers/m9_fault_controller.cpp)
  target_link_libraries(ts_m9_fault_controller PRIVATE ts_controller)
  ts_enable_project_warnings(ts_m9_fault_controller)
  foreach(m9_case IN ITEMS
      pending_cancel_commit_gate_idempotence_and_missing_conflicts
      active_cancel_commit_gate_receipt_not_terminal_and_duplicate
      cancel_unknown_failed_commit_never_escapes_on_wire
      no_dispatch_assigned_proven_empty_and_possible_bytes_cancel_intent
      retry_commit_preserves_history_logs_and_unique_attempt_number
      client_disconnect_and_reused_request_cannot_steal_cancel_retry_response
      bad_cancel_ack_correlations_and_duplicates_invalidate_not_terminalize
      real_cancel_timeout_and_noncooperative_group_cleanup
      cancel_during_held_final_barrier_keeps_natural_outcome_and_reservation
      controller_crash_running_cleanup_before_fresh_registration_and_recovery
      deadlines_and_lost_commit_hold_reservations_and_replacement_ack
      zero_credit_cancel_duplicate_timeout_first_and_natural_first_wire
      running_disconnect_churn_cleans_groups_before_register_and_bounds_fds
      retry_can_succeed_on_another_worker_preserving_exact_old_log_history
      disconnected_retry_and_failed_unknown_retry_commit_never_auto_schedule
      crash_before_and_after_cancel_intent_commit_recovers_lost_never_replays
      killed_worker_lost_has_no_automatic_retry_and_new_session_is_empty
      real_multiworker_projected_choice_and_pending_registration_trigger
      launch_setup_cancel_race_and_stale_worker_command_are_fenced
      partial_cancel_send_is_lost_not_cancelled_and_late_valid_ack_is_receipt
      stalled_log_path_cancel_control_two_workers_and_exact_cancel_tail
      protocol_invalidation_keeps_socket_until_lost_commit_and_fences_late_packets
      cancelled_log_namespace_and_file_sync_failures_never_claim_complete
      idle_reconnect_registration_rejection_backoff_resets_heartbeat_and_credit
      assignment_before_after_commit_crash_has_no_run_and_exact_recovery
      cancel_terminal_before_after_commit_crash_does_not_infer_from_synced_files)
    add_test(NAME M9Loopback.${m9_case}
      COMMAND ${TS_PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/m9_loopback.py
      --bin $<TARGET_FILE_DIR:controller> --case test_${m9_case})
    set_tests_properties(M9Loopback.${m9_case} PROPERTIES LABELS "m9;integration" TIMEOUT 90)
  endforeach()
  add_executable(ts_log_workload tests/helpers/log_workload.cpp)
  ts_enable_project_warnings(ts_log_workload)
  add_executable(ts_m8_fault_controller tests/helpers/m8_fault_controller.cpp)
  target_link_libraries(ts_m8_fault_controller PRIVATE ts_controller)
  ts_enable_project_warnings(ts_m8_fault_controller)
  add_executable(ts_m8_partial_worker tests/helpers/m8_partial_worker.cpp)
  target_link_libraries(ts_m8_partial_worker PRIVATE ts_worker)
  ts_enable_project_warnings(ts_m8_partial_worker)
  foreach(m8_case IN ITEMS empty_real_barrier_terminal_release_and_restart
      large_binary_multiple_replenishments_and_paged_cli_are_exact
      active_snapshot_is_fixed_while_later_append_completes
      every_file_directory_and_sql_gate_keeps_terminal_and_reservation_hidden
      sync_and_open_failures_preserve_observed_outcome_as_log_persistence_error
      write_error_before_finish_is_lost_not_a_fabricated_result
      completion_gate_prevents_replenishment_until_success_is_delivered
      crash_at_namespace_data_and_metadata_boundaries_never_infers_success
      partial_tcp_sends_keep_both_streams_before_finish
      stalled_writer_backpressures_multiple_workers_without_hiding_heartbeats
      rejected_run_and_lost_and_missing_history_never_fabricate_logs
      default_clamped_paging_and_binary_payload_stay_below_frame_limit
      disconnect_and_replacement_during_write_or_barrier_fence_late_success
      nonzero_and_signal_results_are_committed_only_with_complete_logs
      finish_count_mismatch_and_late_log_are_protocol_faults
      worker_pauses_independently_for_missing_bytes_or_slots_and_resumes_exactly
      delayed_write_failure_after_finish_retains_outcome_and_never_syncs_success
      completion_notification_failure_fail_stops_and_restart_is_conservative)
    add_test(NAME M8Loopback.${m8_case}
      COMMAND ${TS_PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/m8_loopback.py
      --bin $<TARGET_FILE_DIR:controller> --case test_${m8_case})
    set_tests_properties(M8Loopback.${m8_case} PROPERTIES LABELS "m8;integration" TIMEOUT 60)
  endforeach()
  foreach(m7_case IN ITEMS
      real_vertical_slice_two_workers_and_crash_recovery
      help_invalid_configuration_lock_and_clean_cycles
      fragmentation_roles_request_correlation_and_fd_churn
      durable_submit_gate_and_disconnected_client
      assignment_gate_reserves_before_run_and_reuses_capacity_after_loss
      essential_failure_and_unknown_commit_fail_stop_without_false_effects
      pagination_history_restore_and_truthful_unsupported
      registration_replacement_heartbeat_and_staging_exhaustion
      output_credit_backpressure_control_liveness_and_group_shutdown
      worker_wire_exec_failure_credit_and_finish_barrier
      negative_missing_ack_and_session_replacement
      startup_read_failure_never_opens_ready_listeners
      real_launch_failures_hold_starting_without_fabricated_running
      connection_output_pressure_is_bounded_and_other_clients_survive
      partial_run_transport_failure_keeps_uncertainty_and_commits_lost
      retained_metadata_and_session_tombstones_have_lifetime_admission_bounds)
    add_test(NAME M7Loopback.${m7_case}
      COMMAND ${TS_PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/m7_loopback.py
      --bin $<TARGET_FILE_DIR:controller> --case test_${m7_case})
    set_tests_properties(M7Loopback.${m7_case} PROPERTIES LABELS "m7;integration" TIMEOUT 45)
  endforeach()

  add_executable(ts_milestone6_tests tests/unit/scheduler_test.cpp tests/unit/resource_ledger_test.cpp tests/unit/worker_registry_test.cpp tests/component/controller_state_test.cpp)
  target_link_libraries(ts_milestone6_tests PRIVATE ts_controller_state GTest::gtest_main)
  ts_enable_project_warnings(ts_milestone6_tests)
  gtest_discover_tests(ts_milestone6_tests NO_PRETTY_VALUES PROPERTIES LABELS m6 TIMEOUT 20)

  add_executable(ts_storage_lock_helper tests/helpers/storage_lock_helper.cpp)
  target_link_libraries(ts_storage_lock_helper PRIVATE ts_persistence)
  ts_enable_project_warnings(ts_storage_lock_helper)
  add_executable(ts_milestone5_tests
    tests/component/storage_schema_test.cpp
    tests/component/storage_transaction_test.cpp
    tests/component/storage_failure_test.cpp
    tests/component/persistence_channel_test.cpp
  )
  target_link_libraries(ts_milestone5_tests PRIVATE ts_persistence GTest::gtest_main)
  target_compile_definitions(ts_milestone5_tests PRIVATE TS_STORAGE_LOCK_HELPER_PATH="$<TARGET_FILE:ts_storage_lock_helper>")
  add_dependencies(ts_milestone5_tests ts_storage_lock_helper)
  ts_enable_project_warnings(ts_milestone5_tests)
  gtest_discover_tests(ts_milestone5_tests NO_PRETTY_VALUES PROPERTIES LABELS m5 TIMEOUT 20 DISCOVERY_TIMEOUT 30)

  add_executable(ts_milestone3_tests
    tests/unit/frame_test.cpp
    tests/unit/messages_test.cpp
    tests/unit/output_queue_test.cpp
    tests/component/tcp_test.cpp
    tests/component/connection_manager_cleanup_test.cpp
  )
  target_link_libraries(ts_milestone3_tests
    PRIVATE ts_protocol ts_net GTest::gtest_main Threads::Threads)
  ts_enable_project_warnings(ts_milestone3_tests)
  gtest_discover_tests(ts_milestone3_tests NO_PRETTY_VALUES PROPERTIES LABELS m3 TIMEOUT 20)

  add_executable(ts_milestone1_tests
    tests/unit/bounded_queue_test.cpp
    tests/unit/config_test.cpp
    tests/unit/ids_test.cpp
    tests/unit/result_test.cpp
    tests/unit/resources_test.cpp
    tests/unit/states_test.cpp
    tests/unit/timestamp_test.cpp
  )
  target_link_libraries(
    ts_milestone1_tests
    PRIVATE
      ts_domain
      GTest::gtest_main
      Threads::Threads
  )
  ts_enable_project_warnings(ts_milestone1_tests)
  gtest_discover_tests(ts_milestone1_tests PROPERTIES LABELS unit)

  add_executable(ts_milestone2_unit_tests
    tests/unit/unique_fd_test.cpp
  )
  target_link_libraries(
    ts_milestone2_unit_tests
    PRIVATE
      ts_common
      GTest::gtest_main
  )
  ts_enable_project_warnings(ts_milestone2_unit_tests)
  gtest_discover_tests(ts_milestone2_unit_tests PROPERTIES LABELS unit)

  add_executable(ts_milestone2_tests
    tests/component/event_loop_test.cpp
    tests/component/io_stress_test.cpp
    tests/component/linux_fd_test.cpp
    tests/component/wakeup_channel_test.cpp
  )
  target_link_libraries(
    ts_milestone2_tests
    PRIVATE
      ts_io
      GTest::gtest_main
      Threads::Threads
  )
  ts_enable_project_warnings(ts_milestone2_tests)
  gtest_discover_tests(
    ts_milestone2_tests
    PROPERTIES LABELS component
    DISCOVERY_TIMEOUT 30
  )

  add_executable(ts_runtime_test_helper
    tests/helpers/runtime_helper.cpp
  )
  target_compile_features(ts_runtime_test_helper PRIVATE cxx_std_17)
  ts_enable_project_warnings(ts_runtime_test_helper)

  add_executable(ts_milestone4_unit_tests
    tests/unit/prepared_exec_test.cpp
    tests/unit/process_handle_test.cpp
  )
  target_link_libraries(
    ts_milestone4_unit_tests
    PRIVATE
      ts_runtime
      GTest::gtest_main
  )
  ts_enable_project_warnings(ts_milestone4_unit_tests)
  gtest_discover_tests(ts_milestone4_unit_tests PROPERTIES LABELS runtime)

  add_executable(ts_milestone4_tests
    tests/component/process_runtime_test.cpp
  )
  target_link_libraries(
    ts_milestone4_tests
    PRIVATE
      ts_runtime
      GTest::gtest_main
      Threads::Threads
  )
  target_compile_definitions(
    ts_milestone4_tests
    PRIVATE TS_RUNTIME_HELPER_PATH="$<TARGET_FILE:ts_runtime_test_helper>"
  )
  add_dependencies(ts_milestone4_tests ts_runtime_test_helper)
  ts_enable_project_warnings(ts_milestone4_tests)
  gtest_discover_tests(
    ts_milestone4_tests
    PROPERTIES LABELS runtime TIMEOUT 15
    DISCOVERY_TIMEOUT 30
  )

  add_executable(ts_runtime_closed_stdio_fixture
    tests/helpers/runtime_closed_stdio_fixture.cpp
  )
  target_link_libraries(ts_runtime_closed_stdio_fixture PRIVATE ts_runtime)
  target_compile_definitions(ts_runtime_closed_stdio_fixture
    PRIVATE TS_RUNTIME_HELPER_PATH="$<TARGET_FILE:ts_runtime_test_helper>"
  )
  add_dependencies(ts_runtime_closed_stdio_fixture ts_runtime_test_helper)
  ts_enable_project_warnings(ts_runtime_closed_stdio_fixture)
  add_test(NAME ProcessRuntime.ClosedStandardFds
           COMMAND ts_runtime_closed_stdio_fixture)
  set_tests_properties(ProcessRuntime.ClosedStandardFds PROPERTIES LABELS runtime TIMEOUT 20)
endif()
