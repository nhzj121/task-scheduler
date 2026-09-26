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
endif()
