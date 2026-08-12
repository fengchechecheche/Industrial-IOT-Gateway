#include <sstream>

#include <gtest/gtest.h>

#include "industrial_iot_gateway/runtime/gateway_runtime.hpp"

namespace industrial_iot_gateway::runtime {
namespace {

TEST(GatewayRuntimeValidationTest, RejectsMissingSerialDeviceBeforeStartingThreads) {
  GatewayRuntimeConfig config{};
  config.registers.device_count = 1U;
  config.registers.registers.push_back({});
  config.registers.poll_jobs.push_back({});
  config.registers.freshness.push_back({});
  std::ostringstream events;

  GatewayRuntime runtime(std::move(config), events);

  EXPECT_FALSE(runtime.valid());
  EXPECT_EQ(runtime.validation_error(), RuntimeErrorCategory::invalid_serial_configuration);
  EXPECT_FALSE(runtime.start());
  EXPECT_FALSE(runtime.statistics().running);
}

TEST(GatewayRuntimeValidationTest, RejectsEmptyRegisterPlanBeforeStartingThreads) {
  GatewayRuntimeConfig config{};
  config.serial.device_path = "/dev/null";
  std::ostringstream events;

  GatewayRuntime runtime(std::move(config), events);

  EXPECT_FALSE(runtime.valid());
  EXPECT_EQ(runtime.validation_error(), RuntimeErrorCategory::invalid_register_configuration);
  EXPECT_FALSE(runtime.start());
}

TEST(GatewayRuntimeValidationTest, RejectsInvalidTimingBeforeStartingThreads) {
  GatewayRuntimeConfig config{};
  config.serial.device_path = "/dev/null";
  config.registers.device_count = 1U;
  config.registers.registers.push_back({});
  config.registers.poll_jobs.push_back({});
  config.registers.freshness.push_back({});
  config.response_timeout = std::chrono::milliseconds(0);
  std::ostringstream events;

  GatewayRuntime runtime(std::move(config), events);

  EXPECT_FALSE(runtime.valid());
  EXPECT_EQ(runtime.validation_error(), RuntimeErrorCategory::invalid_timing);
  EXPECT_FALSE(runtime.start());
}

TEST(GatewayRuntimeValidationTest, RejectsZeroLateResponseGuardBeforeStartingThreads) {
  GatewayRuntimeConfig config{};
  config.serial.device_path = "/dev/null";
  config.registers.device_count = 1U;
  config.registers.registers.push_back({});
  config.registers.poll_jobs.push_back({});
  config.registers.freshness.push_back({});
  config.late_response_guard = std::chrono::milliseconds(0);
  std::ostringstream events;

  GatewayRuntime runtime(std::move(config), events);

  EXPECT_FALSE(runtime.valid());
  EXPECT_EQ(runtime.validation_error(), RuntimeErrorCategory::invalid_timing);
  EXPECT_FALSE(runtime.start());
}

} // namespace
} // namespace industrial_iot_gateway::runtime
