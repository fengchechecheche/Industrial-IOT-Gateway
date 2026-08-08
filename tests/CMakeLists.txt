find_package(GTest CONFIG REQUIRED)

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
