find_package(GTest CONFIG REQUIRED)
find_package(Threads REQUIRED)

add_executable(gateway_smoke_test unit/build_info_test.cpp)
target_link_libraries(
  gateway_smoke_test
  PRIVATE
    industrial_iot_gateway::gateway_core
    gateway_project_options
    GTest::gtest_main
)
gateway_enable_clang_tidy(gateway_smoke_test)

add_executable(gateway_crc16_test unit/crc16_modbus_test.cpp)
target_link_libraries(
  gateway_crc16_test
  PRIVATE
    industrial_iot_gateway::gateway_core
    gateway_project_options
    GTest::gtest_main
)
target_compile_definitions(
  gateway_crc16_test
  PRIVATE GATEWAY_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
)
gateway_enable_clang_tidy(gateway_crc16_test)

add_executable(gateway_modbus_codec_test unit/modbus_codec_test.cpp)
target_link_libraries(
  gateway_modbus_codec_test
  PRIVATE
    industrial_iot_gateway::gateway_core
    gateway_project_options
    GTest::gtest_main
)
target_compile_definitions(
  gateway_modbus_codec_test
  PRIVATE GATEWAY_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
)
gateway_enable_clang_tidy(gateway_modbus_codec_test)

add_executable(gateway_rtu_stream_parser_test unit/rtu_stream_parser_test.cpp)
target_link_libraries(
  gateway_rtu_stream_parser_test
  PRIVATE
    industrial_iot_gateway::gateway_core
    gateway_project_options
    GTest::gtest_main
)
target_compile_definitions(
  gateway_rtu_stream_parser_test
  PRIVATE GATEWAY_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
)
gateway_enable_clang_tidy(gateway_rtu_stream_parser_test)

add_executable(gateway_serial_port_test unit/serial_port_test.cpp)
target_link_libraries(
  gateway_serial_port_test
  PRIVATE
    industrial_iot_gateway::gateway_core
    gateway_project_options
    GTest::gtest_main
)
target_include_directories(gateway_serial_port_test PRIVATE "${CMAKE_SOURCE_DIR}/src")
gateway_enable_clang_tidy(gateway_serial_port_test)

add_executable(gateway_poll_scheduler_test unit/poll_scheduler_test.cpp)
target_link_libraries(
  gateway_poll_scheduler_test
  PRIVATE
    industrial_iot_gateway::gateway_core
    gateway_project_options
    GTest::gtest_main
)
gateway_enable_clang_tidy(gateway_poll_scheduler_test)

add_executable(
  gateway_reliability_policy_test
  unit/reliability_policy_test.cpp
  unit/poll_scheduler_reliability_test.cpp
  unit/freshness_tracker_test.cpp
)
target_link_libraries(
  gateway_reliability_policy_test
  PRIVATE
    industrial_iot_gateway::gateway_core
    gateway_project_options
    GTest::gtest_main
)
gateway_enable_clang_tidy(gateway_reliability_policy_test)

add_executable(
  gateway_event_pipeline_test
  unit/bounded_queue_test.cpp
  unit/request_queue_test.cpp
  unit/event_pipeline_test.cpp
  unit/shutdown_coordinator_test.cpp
  unit/structured_log_test.cpp
)
target_link_libraries(
  gateway_event_pipeline_test
  PRIVATE
    industrial_iot_gateway::gateway_core
    gateway_project_options
    GTest::gtest_main
    Threads::Threads
)
gateway_enable_clang_tidy(gateway_event_pipeline_test)

add_executable(
  gateway_shutdown_signal_integration_test
  integration/shutdown_signal_integration_test.cpp
)
target_link_libraries(
  gateway_shutdown_signal_integration_test
  PRIVATE
    industrial_iot_gateway::gateway_core
    gateway_project_options
    GTest::gtest_main
    Threads::Threads
)
gateway_enable_clang_tidy(gateway_shutdown_signal_integration_test)
if(GATEWAY_BUILD_PTY_SLAVE)
  add_executable(gateway_pty_slave_config_test unit/pty_slave_config_test.cpp)
  target_link_libraries(
    gateway_pty_slave_config_test
    PRIVATE
      pty_slave_support
      GTest::gtest_main
  )
  target_compile_definitions(
    gateway_pty_slave_config_test
    PRIVATE
      PTY_REGISTER_MAP_PATH="${CMAKE_SOURCE_DIR}/config/examples/register_map.yaml"
      PTY_SCENARIO_CONFIG_PATH="${CMAKE_SOURCE_DIR}/config/examples/pty_slave_scenarios.yaml"
  )
  gateway_enable_clang_tidy(gateway_pty_slave_config_test)

  add_executable(gateway_pty_slave_integration_test integration/pty_slave_integration_test.cpp)
  target_link_libraries(
    gateway_pty_slave_integration_test
    PRIVATE
      industrial_iot_gateway::gateway_core
      gateway_project_options
      GTest::gtest_main
  )
  target_compile_definitions(
    gateway_pty_slave_integration_test
    PRIVATE
      PTY_SLAVE_EXECUTABLE="$<TARGET_FILE:pty_slave>"
      PTY_REGISTER_MAP_PATH="${CMAKE_SOURCE_DIR}/config/examples/register_map.yaml"
      PTY_SCENARIO_CONFIG_PATH="${CMAKE_SOURCE_DIR}/config/examples/pty_slave_scenarios.yaml"
  )
  add_dependencies(gateway_pty_slave_integration_test pty_slave)
  gateway_enable_clang_tidy(gateway_pty_slave_integration_test)

  add_executable(gateway_serial_port_pty_test integration/serial_port_pty_test.cpp)
  target_link_libraries(
    gateway_serial_port_pty_test
    PRIVATE
      industrial_iot_gateway::gateway_core
      gateway_project_options
      GTest::gtest_main
      "${GATEWAY_UTIL_LIBRARY}"
  )
  gateway_enable_clang_tidy(gateway_serial_port_pty_test)
endif()

set(
  gateway_gtest_targets
  gateway_smoke_test
  gateway_crc16_test
  gateway_modbus_codec_test
  gateway_rtu_stream_parser_test
  gateway_serial_port_test
  gateway_poll_scheduler_test
  gateway_reliability_policy_test
  gateway_event_pipeline_test
  gateway_shutdown_signal_integration_test
)
if(GATEWAY_BUILD_PTY_SLAVE)
  list(
    APPEND
    gateway_gtest_targets
    gateway_pty_slave_config_test
    gateway_pty_slave_integration_test
    gateway_serial_port_pty_test
  )
endif()
foreach(gateway_gtest_target IN LISTS gateway_gtest_targets)
  gateway_configure_tsan_test_runtime("${gateway_gtest_target}")
endforeach()
unset(gateway_gtest_target)
unset(gateway_gtest_targets)

include(GoogleTest)
gtest_discover_tests(
  gateway_smoke_test
  PROPERTIES
    LABELS unit
    TIMEOUT 10
  DISCOVERY_TIMEOUT 10
)
gtest_discover_tests(
  gateway_crc16_test
  PROPERTIES
    LABELS unit
    TIMEOUT 10
  DISCOVERY_TIMEOUT 10
)
gtest_discover_tests(
  gateway_modbus_codec_test
  PROPERTIES
    LABELS unit
    TIMEOUT 10
  DISCOVERY_TIMEOUT 10
)
gtest_discover_tests(
  gateway_rtu_stream_parser_test
  PROPERTIES
    LABELS unit
    TIMEOUT 10
  DISCOVERY_TIMEOUT 10
)
gtest_discover_tests(
  gateway_serial_port_test
  PROPERTIES
    LABELS unit
    TIMEOUT 10
  DISCOVERY_TIMEOUT 10
)

gtest_discover_tests(
  gateway_poll_scheduler_test
  PROPERTIES
    LABELS unit
    TIMEOUT 10
  DISCOVERY_TIMEOUT 10
)

gtest_discover_tests(
  gateway_reliability_policy_test
  PROPERTIES
    LABELS unit
    TIMEOUT 10
  DISCOVERY_TIMEOUT 10
)

gtest_discover_tests(
  gateway_event_pipeline_test
  PROPERTIES
    LABELS unit
    TIMEOUT 10
  DISCOVERY_TIMEOUT 10
)
gtest_discover_tests(
  gateway_shutdown_signal_integration_test
  PROPERTIES
    LABELS "integration;shutdown"
    TIMEOUT 10
  DISCOVERY_TIMEOUT 10
)


if(GATEWAY_BUILD_PTY_SLAVE)
  gtest_discover_tests(
    gateway_pty_slave_config_test
    PROPERTIES
      LABELS unit
      TIMEOUT 10
    DISCOVERY_TIMEOUT 10
  )
  gtest_discover_tests(
    gateway_pty_slave_integration_test
    PROPERTIES
      LABELS integration
      TIMEOUT 10
    DISCOVERY_TIMEOUT 10
  )
  gtest_discover_tests(
    gateway_serial_port_pty_test
    PROPERTIES
      LABELS integration
      TIMEOUT 10
    DISCOVERY_TIMEOUT 10
  )
  add_test(NAME pty_integration_suite COMMAND gateway_pty_slave_integration_test)
  set_tests_properties(
    pty_integration_suite
    PROPERTIES
      LABELS pty
      TIMEOUT 10
  )
  add_test(NAME serial_port_pty_suite COMMAND gateway_serial_port_pty_test)
  set_tests_properties(
    serial_port_pty_suite
    PROPERTIES
      LABELS pty
      TIMEOUT 10
  )
endif()
