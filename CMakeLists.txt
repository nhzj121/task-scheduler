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

if(BUILD_TESTING)
  find_package(GTest 1.11 REQUIRED)
  include(GoogleTest)

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
endif()
