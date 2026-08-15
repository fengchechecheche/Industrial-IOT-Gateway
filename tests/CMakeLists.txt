find_package(GTest CONFIG REQUIRED)
find_package(Threads REQUIRED)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_test(
  NAME release_contract_unit
  COMMAND "${Python3_EXECUTABLE}" -m unittest tests/release/test_release_contract.py
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(release_contract_unit PROPERTIES LABELS "unit;release" TIMEOUT 15)
add_test(
  NAME demo_cli_unit
  COMMAND "${Python3_EXECUTABLE}" -m unittest tests/release/test_demo.py
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(demo_cli_unit PROPERTIES LABELS "unit;release;demo" TIMEOUT 20)
add_test(
  NAME release_manifest_unit
  COMMAND "${Python3_EXECUTABLE}" -m unittest tests/release/test_release_manifest.py
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(release_manifest_unit PROPERTIES LABELS "unit;release" TIMEOUT 20)
add_test(
  NAME release_runner_unit
  COMMAND "${Python3_EXECUTABLE}" -m unittest tests/release/test_release_runner.py
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(release_runner_unit PROPERTIES LABELS "unit;release" TIMEOUT 20)
add_test(
  NAME release_systemd_runner_unit
  COMMAND "${Python3_EXECUTABLE}" -m unittest tests/release/test_systemd_runner.py
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(
  release_systemd_runner_unit PROPERTIES LABELS "unit;release;system" TIMEOUT 20
)
add_test(
  NAME release_systemd_scenarios_unit
  COMMAND "${Python3_EXECUTABLE}" -m unittest tests/release/test_systemd_scenarios.py
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(
  release_systemd_scenarios_unit PROPERTIES LABELS "unit;release;system" TIMEOUT 20
)
add_test(
  NAME release_native_systemd_runner_unit
  COMMAND "${Python3_EXECUTABLE}" -m unittest tests/release/test_native_systemd_runner.py
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(
  release_native_systemd_runner_unit PROPERTIES LABELS "unit;release;system" TIMEOUT 20
)
add_test(
  NAME release_native_bundle_runner_unit
  COMMAND "${Python3_EXECUTABLE}" -m unittest tests/release/test_native_bundle_runner.py
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(
  release_native_bundle_runner_unit PROPERTIES LABELS "unit;release;bundle" TIMEOUT 30
)

add_test(
  NAME soak_profile_contract
  COMMAND "${Python3_EXECUTABLE}" -m unittest tests/soak/test_soak_profile.py
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(soak_profile_contract PROPERTIES LABELS "unit;soak" TIMEOUT 15)
add_test(
  NAME soak_runner_unit
  COMMAND "${Python3_EXECUTABLE}" -m unittest tests/soak/test_soak_runner.py
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(soak_runner_unit PROPERTIES LABELS "unit;soak" TIMEOUT 15)
add_test(
  NAME soak_summary_unit
  COMMAND "${Python3_EXECUTABLE}" -m unittest tests/soak/test_soak_summary.py
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
set_tests_properties(soak_summary_unit PROPERTIES LABELS "unit;soak" TIMEOUT 15)

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
  unit/publish_sink_test.cpp
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

add_executable(
  gateway_runtime_unit_test
  unit/runtime_config_test.cpp
  unit/pipeline_wait_test.cpp
  unit/gateway_runtime_test.cpp
)
target_link_libraries(
  gateway_runtime_unit_test
  PRIVATE
    industrial_iot_gateway::gateway_core
    gateway_project_options
    GTest::gtest_main
    Threads::Threads
)
target_compile_definitions(
  gateway_runtime_unit_test
  PRIVATE
    GATEWAY_REGISTER_MAP_PATH="${CMAKE_SOURCE_DIR}/config/examples/register_map.yaml"
    GATEWAY_INVALID_REGISTER_MAP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/data/invalid_register_map.yaml"
)
gateway_enable_clang_tidy(gateway_runtime_unit_test)

add_executable(gateway_fault_contract_test fault/fault_contract_test.cpp)
target_link_libraries(
  gateway_fault_contract_test
  PRIVATE
    industrial_iot_gateway::gateway_core
    gateway_project_options
    GTest::gtest_main
    Threads::Threads
)
target_compile_definitions(
  gateway_fault_contract_test
  PRIVATE GATEWAY_REGISTER_MAP_PATH="${CMAKE_SOURCE_DIR}/config/examples/register_map.yaml"
)
gateway_enable_clang_tidy(gateway_fault_contract_test)

add_executable(
  gateway_mqtt_policy_test
  unit/mqtt_reconnect_policy_test.cpp
  ${CMAKE_SOURCE_DIR}/src/mqtt/reconnect_policy.cpp
)
target_link_libraries(
  gateway_mqtt_policy_test
  PRIVATE
    gateway_project_options
    GTest::gtest_main
)
target_include_directories(gateway_mqtt_policy_test PRIVATE "${CMAKE_SOURCE_DIR}/include")
gateway_enable_clang_tidy(gateway_mqtt_policy_test)

if(GATEWAY_ENABLE_MQTT)
  find_program(GATEWAY_MOSQUITTO_EXECUTABLE NAMES mosquitto REQUIRED)
  find_program(GATEWAY_MOSQUITTO_SUB_EXECUTABLE NAMES mosquitto_sub REQUIRED)
  add_executable(
    gateway_mqtt_unit_test
    unit/mqtt_payload_serializer_test.cpp
    unit/mqtt_publish_sink_test.cpp
  )
  target_link_libraries(
    gateway_mqtt_unit_test
    PRIVATE
      industrial_iot_gateway::gateway_mqtt
      gateway_project_options
      gateway_paho_mqtt
      GTest::gtest_main
      nlohmann_json::nlohmann_json
  )
  gateway_enable_clang_tidy(gateway_mqtt_unit_test)

  add_executable(
    gateway_mqtt_broker_integration_test
    integration/mqtt_broker_integration_test.cpp
  )
  target_link_libraries(
    gateway_mqtt_broker_integration_test
    PRIVATE
      industrial_iot_gateway::gateway_mqtt
      gateway_project_options
      gateway_paho_mqtt
      GTest::gtest_main
      Threads::Threads
  )
  target_compile_definitions(
    gateway_mqtt_broker_integration_test
    PRIVATE
      GATEWAY_MOSQUITTO_EXECUTABLE="${GATEWAY_MOSQUITTO_EXECUTABLE}"
      GATEWAY_MOSQUITTO_SUB_EXECUTABLE="${GATEWAY_MOSQUITTO_SUB_EXECUTABLE}"
  )
  gateway_enable_clang_tidy(gateway_mqtt_broker_integration_test)
endif()
if(GATEWAY_ENABLE_MQTT AND GATEWAY_BUILD_PTY_SLAVE)
  add_test(
    NAME soak_smoke_integration
    COMMAND
      "${Python3_EXECUTABLE}" tests/soak/run_smoke_ctest.py
      --driver $<TARGET_FILE:gateway_soak_driver>
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  )
  set_tests_properties(
    soak_smoke_integration
    PROPERTIES LABELS "integration;soak" RUN_SERIAL TRUE TIMEOUT 130
  )
  if(GATEWAY_TSAN_TEST_EXECUTOR)
    list(JOIN GATEWAY_TSAN_TEST_EXECUTOR " " gateway_soak_tsan_executor)
    set_tests_properties(
      soak_smoke_integration
      PROPERTIES ENVIRONMENT "GATEWAY_SOAK_DRIVER_EXECUTOR=${gateway_soak_tsan_executor}"
    )
    unset(gateway_soak_tsan_executor)
  endif()

  add_executable(
    gateway_mqtt_pty_runtime_integration_test
    integration/mqtt_pty_runtime_integration_test.cpp
  )
  target_link_libraries(
    gateway_mqtt_pty_runtime_integration_test
    PRIVATE
      industrial_iot_gateway::gateway_mqtt
      gateway_project_options
      pty_slave_support
      industrial_iot_gateway::gateway_simulation
      GTest::gtest_main
      Threads::Threads
      "${GATEWAY_UTIL_LIBRARY}"
  )
  target_compile_definitions(
    gateway_mqtt_pty_runtime_integration_test
    PRIVATE
      GATEWAY_MOSQUITTO_EXECUTABLE="${GATEWAY_MOSQUITTO_EXECUTABLE}"
      GATEWAY_REGISTER_MAP_PATH="${CMAKE_SOURCE_DIR}/config/examples/register_map.yaml"
      GATEWAY_PTY_SCENARIO_PATH="${CMAKE_SOURCE_DIR}/config/examples/pty_slave_scenarios.yaml"
  )
  gateway_enable_clang_tidy(gateway_mqtt_pty_runtime_integration_test)

  add_executable(
    gateway_mqtt_slow_ack_integration_test
    integration/mqtt_slow_ack_integration_test.cpp
  )
  target_link_libraries(
    gateway_mqtt_slow_ack_integration_test
    PRIVATE
      industrial_iot_gateway::gateway_mqtt
      gateway_project_options
      pty_slave_support
      industrial_iot_gateway::gateway_simulation
      GTest::gtest_main
      Threads::Threads
      "${GATEWAY_UTIL_LIBRARY}"
  )
  target_compile_definitions(
    gateway_mqtt_slow_ack_integration_test
    PRIVATE
      GATEWAY_MOSQUITTO_EXECUTABLE="${GATEWAY_MOSQUITTO_EXECUTABLE}"
      GATEWAY_REGISTER_MAP_PATH="${CMAKE_SOURCE_DIR}/config/examples/register_map.yaml"
      GATEWAY_PTY_SCENARIO_PATH="${CMAKE_SOURCE_DIR}/config/examples/pty_slave_scenarios.yaml"
  )
  gateway_enable_clang_tidy(gateway_mqtt_slow_ack_integration_test)
endif()
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

  add_executable(
    gateway_runtime_pty_test
    integration/gateway_runtime_pty_test.cpp
  )
  target_link_libraries(
    gateway_runtime_pty_test
    PRIVATE
      industrial_iot_gateway::gateway_core
      gateway_project_options
      pty_slave_support
      industrial_iot_gateway::gateway_simulation
      GTest::gtest_main
      Threads::Threads
      "${GATEWAY_UTIL_LIBRARY}"
  )
  target_compile_definitions(
    gateway_runtime_pty_test
    PRIVATE
      GATEWAY_REGISTER_MAP_PATH="${CMAKE_SOURCE_DIR}/config/examples/register_map.yaml"
      GATEWAY_PTY_SCENARIO_PATH="${CMAKE_SOURCE_DIR}/config/examples/pty_slave_scenarios.yaml"
  )
  gateway_enable_clang_tidy(gateway_runtime_pty_test)

  add_executable(
    gateway_app_process_test
    integration/gateway_app_process_test.cpp
  )
  target_link_libraries(
    gateway_app_process_test
    PRIVATE
      industrial_iot_gateway::gateway_core
      gateway_project_options
      pty_slave_support
      industrial_iot_gateway::gateway_simulation
      GTest::gtest_main
      Threads::Threads
      "${GATEWAY_UTIL_LIBRARY}"
  )
  target_compile_definitions(
    gateway_app_process_test
    PRIVATE
      GATEWAY_APP_EXECUTABLE="$<TARGET_FILE:gateway_app>"
      GATEWAY_REGISTER_MAP_PATH="${CMAKE_SOURCE_DIR}/config/examples/register_map.yaml"
      GATEWAY_PTY_SCENARIO_PATH="${CMAKE_SOURCE_DIR}/config/examples/pty_slave_scenarios.yaml"
  )
  add_dependencies(gateway_app_process_test gateway_app)
  gateway_enable_clang_tidy(gateway_app_process_test)
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
  gateway_runtime_unit_test
  gateway_fault_contract_test
  gateway_mqtt_policy_test
)
if(GATEWAY_BUILD_PTY_SLAVE)
  list(
    APPEND
    gateway_gtest_targets
    gateway_pty_slave_config_test
    gateway_pty_slave_integration_test
    gateway_serial_port_pty_test
    gateway_runtime_pty_test
    gateway_app_process_test
  )
endif()
if(GATEWAY_ENABLE_MQTT)
  list(
    APPEND
    gateway_gtest_targets
    gateway_mqtt_unit_test
    gateway_mqtt_broker_integration_test
  )
endif()
if(GATEWAY_ENABLE_MQTT AND GATEWAY_BUILD_PTY_SLAVE)
  list(
    APPEND
    gateway_gtest_targets
    gateway_mqtt_pty_runtime_integration_test
    gateway_mqtt_slow_ack_integration_test
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
gtest_discover_tests(
  gateway_runtime_unit_test
  PROPERTIES
    LABELS unit
    TIMEOUT 10
  DISCOVERY_TIMEOUT 10
)

gtest_discover_tests(
  gateway_fault_contract_test
  PROPERTIES
    LABELS "unit;fault"
    TIMEOUT 10
  DISCOVERY_TIMEOUT 10
)

add_test(
  NAME fault_runner_unit
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/fault/test_run_fault_matrix.py"
)
set_tests_properties(
  fault_runner_unit
  PROPERTIES
    LABELS "unit;fault"
    TIMEOUT 15
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

gtest_discover_tests(
  gateway_mqtt_policy_test
  PROPERTIES
    LABELS unit
    TIMEOUT 10
  DISCOVERY_TIMEOUT 10
)
add_test(NAME mqtt_policy_suite COMMAND gateway_mqtt_policy_test)
set_tests_properties(mqtt_policy_suite PROPERTIES LABELS mqtt TIMEOUT 10)

if(GATEWAY_ENABLE_MQTT)
  gtest_discover_tests(
    gateway_mqtt_unit_test
    PROPERTIES
      LABELS mqtt
      TIMEOUT 10
    DISCOVERY_TIMEOUT 10
  )
  gtest_discover_tests(
    gateway_mqtt_broker_integration_test
    PROPERTIES
      LABELS mqtt
      TIMEOUT 30
    DISCOVERY_TIMEOUT 10
  )
endif()
if(GATEWAY_ENABLE_MQTT AND GATEWAY_BUILD_PTY_SLAVE)
  gtest_discover_tests(
    gateway_mqtt_pty_runtime_integration_test
    PROPERTIES
      LABELS mqtt
      TIMEOUT 30
    DISCOVERY_TIMEOUT 10
  )
  gtest_discover_tests(
    gateway_mqtt_slow_ack_integration_test
    PROPERTIES
      LABELS "mqtt;integration;fault"
      TIMEOUT 40
    DISCOVERY_TIMEOUT 10
  )
endif()


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
  gtest_discover_tests(
    gateway_runtime_pty_test
    PROPERTIES
      LABELS "integration;pty;runtime"
      TIMEOUT 60
    DISCOVERY_TIMEOUT 10
  )
  gtest_discover_tests(
    gateway_app_process_test
    PROPERTIES
      LABELS "integration;pty;runtime;shutdown"
      TIMEOUT 15
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
  add_test(NAME gateway_runtime_pty_suite COMMAND gateway_runtime_pty_test)
  set_tests_properties(
    gateway_runtime_pty_suite
    PROPERTIES
      LABELS pty
      TIMEOUT 60
  )
  add_test(NAME gateway_app_process_suite COMMAND gateway_app_process_test)
  set_tests_properties(
    gateway_app_process_suite
    PROPERTIES
      LABELS pty
      TIMEOUT 15
  )
endif()
