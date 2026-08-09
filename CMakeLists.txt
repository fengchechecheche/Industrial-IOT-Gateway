cmake_minimum_required(VERSION 3.22)

project(
  industrial_iot_gateway
  VERSION 0.1.0
  DESCRIPTION "Industrial IoT gateway"
  LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

option(GATEWAY_BUILD_TESTS "Build project tests" "${BUILD_TESTING}")
option(GATEWAY_BUILD_PTY_SLAVE "Build the PTY Modbus slave simulator" OFF)
option(GATEWAY_ENABLE_MQTT "Build MQTT integration" OFF)
option(GATEWAY_ENABLE_WARNINGS_AS_ERRORS "Treat project warnings as errors" OFF)
option(GATEWAY_ENABLE_CLANG_TIDY "Run clang-tidy while compiling project targets" OFF)
option(GATEWAY_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(GATEWAY_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(
  GATEWAY_ENABLE_WSL_TSAN_WORKAROUND
  "Run TSan tests through a process-local WSL2 address-layout workaround"
  ON
)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(CompilerWarnings)
include(Sanitizers)
include(StaticAnalysis)
include(TsanTestRuntime)

find_package(yaml-cpp 0.8 REQUIRED)

add_library(gateway_project_options INTERFACE)
target_compile_features(gateway_project_options INTERFACE cxx_std_17)
gateway_enable_warnings(gateway_project_options)
gateway_enable_sanitizers(gateway_project_options)

add_library(
  gateway_core STATIC
  src/gateway_core/build_info.cpp
  src/protocol/crc16_modbus.cpp
  src/protocol/modbus_codec.cpp
  src/protocol/rtu_stream_parser.cpp
  src/transport/serial_port.cpp
  src/scheduler/poll_scheduler.cpp
  src/scheduler/reliability_policy.cpp
  src/quality/freshness_tracker.cpp
  src/pipeline/request_queue.cpp
  src/pipeline/event_pipeline.cpp
  src/publish/jsonl_publish_sink.cpp
  src/lifecycle/shutdown_coordinator.cpp
  src/observability/structured_log.cpp
  src/config/runtime_config.cpp
  src/runtime/gateway_runtime.cpp
)
add_library(industrial_iot_gateway::gateway_core ALIAS gateway_core)
target_include_directories(
  gateway_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(gateway_core PRIVATE gateway_project_options yaml-cpp::yaml-cpp)
gateway_enable_clang_tidy(gateway_core)

if(GATEWAY_ENABLE_MQTT)
  include(MqttDependencies)

  add_library(
    gateway_mqtt STATIC
    src/mqtt/reconnect_policy.cpp
    src/mqtt/payload_serializer.cpp
    src/mqtt/mqtt_publish_sink.cpp
  )
  add_library(industrial_iot_gateway::gateway_mqtt ALIAS gateway_mqtt)
  target_include_directories(
    gateway_mqtt
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:include>
  )
  target_link_libraries(
    gateway_mqtt
    PUBLIC industrial_iot_gateway::gateway_core
    PRIVATE
      gateway_project_options
      gateway_paho_mqtt
      nlohmann_json::nlohmann_json
  )
  gateway_enable_clang_tidy(gateway_mqtt)
endif()

add_executable(gateway_app src/app/main.cpp)
target_link_libraries(gateway_app PRIVATE industrial_iot_gateway::gateway_core gateway_project_options)
if(GATEWAY_ENABLE_MQTT)
  target_link_libraries(gateway_app PRIVATE industrial_iot_gateway::gateway_mqtt)
  target_compile_definitions(gateway_app PRIVATE GATEWAY_HAS_MQTT=1)
endif()
gateway_enable_clang_tidy(gateway_app)

if(GATEWAY_BUILD_PTY_SLAVE)
  include(CheckIncludeFileCXX)
  check_include_file_cxx(pty.h GATEWAY_HAVE_PTY_H)
  if(NOT GATEWAY_HAVE_PTY_H)
    message(FATAL_ERROR "GATEWAY_BUILD_PTY_SLAVE requires <pty.h>")
  endif()
  find_library(GATEWAY_UTIL_LIBRARY util REQUIRED)
  add_subdirectory(tools/pty_slave)
endif()

if(GATEWAY_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
