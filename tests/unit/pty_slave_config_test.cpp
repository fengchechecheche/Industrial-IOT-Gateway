#include "pty_slave_support.hpp"

#include <cstdint>
#include <string>
#include <variant>

#include <gtest/gtest.h>

namespace {

using industrial_iot_gateway::protocol::FunctionCode;
using industrial_iot_gateway::protocol::ReadResponse;
using industrial_iot_gateway::pty_slave::load_runtime_configuration;
using industrial_iot_gateway::pty_slave::RegisterOperationResult;

const std::string kRegisterMap = PTY_REGISTER_MAP_PATH;
const std::string kScenarioConfig = PTY_SCENARIO_CONFIG_PATH;

TEST(PtySlaveConfigTest, LoadsFrozenRegisterSpacesAndInitialValues) {
  auto loaded = load_runtime_configuration(kRegisterMap, kScenarioConfig, 1U);
  ASSERT_TRUE(loaded) << loaded.error;
  ASSERT_NE(loaded.configuration, nullptr);
  EXPECT_EQ(loaded.configuration->registers.slave_id(), 1U);

  ReadResponse input{};
  EXPECT_EQ(loaded.configuration->registers.read(FunctionCode::read_input_registers, 0U, 2U, input),
            RegisterOperationResult::success);
  ASSERT_EQ(input.value_count, 2U);
  EXPECT_EQ(input.values[0], 235U);
  EXPECT_EQ(input.values[1], 503U);

  ReadResponse holding{};
  EXPECT_EQ(
      loaded.configuration->registers.read(FunctionCode::read_holding_registers, 0U, 1U, holding),
      RegisterOperationResult::success);
  ASSERT_EQ(holding.value_count, 1U);
  EXPECT_EQ(holding.values[0], 1'000U);
}

TEST(PtySlaveConfigTest, EnforcesWritePermissionsAndLimits) {
  auto loaded = load_runtime_configuration(kRegisterMap, kScenarioConfig, 1U);
  ASSERT_TRUE(loaded) << loaded.error;

  EXPECT_EQ(loaded.configuration->registers.write(0U, 1'200U), RegisterOperationResult::success);
  EXPECT_EQ(loaded.configuration->registers.write(0U, 99U), RegisterOperationResult::illegal_value);
  EXPECT_EQ(loaded.configuration->registers.write(1U, 500U),
            RegisterOperationResult::illegal_address);

  ReadResponse holding{};
  ASSERT_EQ(
      loaded.configuration->registers.read(FunctionCode::read_holding_registers, 0U, 1U, holding),
      RegisterOperationResult::success);
  EXPECT_EQ(holding.values[0], 1'200U);
}

TEST(PtySlaveConfigTest, RejectsMissingFilesAndUnknownSlaves) {
  const auto missing =
      load_runtime_configuration("/missing/register_map.yaml", kScenarioConfig, 1U);
  EXPECT_FALSE(missing);
  EXPECT_FALSE(missing.error.empty());

  const auto unknown = load_runtime_configuration(kRegisterMap, kScenarioConfig, 247U);
  EXPECT_FALSE(unknown);
  EXPECT_FALSE(unknown.error.empty());
}

TEST(PtySlaveConfigTest, LoadsAllThreeFrozenSlaveDefinitions) {
  for (const std::uint8_t slave_id : {1U, 2U, 3U}) {
    auto loaded = load_runtime_configuration(kRegisterMap, kScenarioConfig, slave_id);
    ASSERT_TRUE(loaded) << "slave_id=" << static_cast<unsigned int>(slave_id) << ' '
                        << loaded.error;
    ASSERT_NE(loaded.configuration, nullptr);
    EXPECT_EQ(loaded.configuration->registers.slave_id(), slave_id);
  }
}

} // namespace
