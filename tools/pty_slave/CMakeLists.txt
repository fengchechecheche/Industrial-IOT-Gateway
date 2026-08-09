add_library(pty_slave_support STATIC pty_slave_support.cpp)
target_include_directories(pty_slave_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(
  pty_slave_support
  PUBLIC
    industrial_iot_gateway::gateway_core
    gateway_project_options
    yaml-cpp::yaml-cpp
    "${GATEWAY_UTIL_LIBRARY}"
)
gateway_enable_clang_tidy(pty_slave_support)

add_executable(pty_slave main.cpp)
target_link_libraries(pty_slave PRIVATE pty_slave_support gateway_project_options)
set_target_properties(pty_slave PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
gateway_enable_clang_tidy(pty_slave)
