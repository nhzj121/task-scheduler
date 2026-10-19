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

if(BUILD_TESTING)
  find_package(GTest 1.11 REQUIRED)
  include(GoogleTest)

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
