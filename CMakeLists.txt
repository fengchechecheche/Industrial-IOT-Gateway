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

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(CompilerWarnings)
include(Sanitizers)
include(StaticAnalysis)

if(GATEWAY_BUILD_PTY_SLAVE)
  message(FATAL_ERROR "pty_slave is planned for P3-S2-T05 and is not implemented in P3-S2-T01")
endif()

if(GATEWAY_ENABLE_MQTT)
  message(FATAL_ERROR "MQTT is planned for P3-S4 and is not implemented in P3-S2-T01")
endif()

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
)
add_library(industrial_iot_gateway::gateway_core ALIAS gateway_core)
target_include_directories(
  gateway_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(gateway_core PRIVATE gateway_project_options)
gateway_enable_clang_tidy(gateway_core)

add_executable(gateway_app src/app/main.cpp)
target_link_libraries(gateway_app PRIVATE industrial_iot_gateway::gateway_core gateway_project_options)
gateway_enable_clang_tidy(gateway_app)

if(GATEWAY_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
