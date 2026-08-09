#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "industrial_iot_gateway/config/runtime_config.hpp"

namespace industrial_iot_gateway::config {
namespace {

TEST(RuntimeConfigTest, LoadsFrozenThreeSlaveSixteenRegisterPlan) {
  const auto loaded = load_runtime_configuration(GATEWAY_REGISTER_MAP_PATH);

  ASSERT_TRUE(loaded) << loaded.detail;
  ASSERT_NE(loaded.configuration, nullptr);
  EXPECT_EQ(loaded.configuration->device_count, 3U);
  EXPECT_EQ(loaded.configuration->registers.size(), 16U);
  EXPECT_EQ(loaded.configuration->poll_jobs.size(), 16U);
  EXPECT_EQ(loaded.configuration->freshness.size(), 16U);

  for (std::size_t index = 0U; index < loaded.configuration->registers.size(); ++index) {
    const auto expected_id = static_cast<std::uint32_t>(index + 1U);
    EXPECT_EQ(loaded.configuration->registers[index].poll_job_id, expected_id);
    EXPECT_EQ(loaded.configuration->poll_jobs[index].poll_job_id, expected_id);
    EXPECT_EQ(loaded.configuration->freshness[index].poll_job_id, expected_id);
  }
}

TEST(RuntimeConfigTest, PreservesFrozenRegisterSemantics) {
  const auto loaded = load_runtime_configuration(GATEWAY_REGISTER_MAP_PATH);
  ASSERT_TRUE(loaded) << loaded.detail;

  const auto *temperature = loaded.configuration->find_register(1U);
  ASSERT_NE(temperature, nullptr);
  EXPECT_EQ(temperature->slave_id, 1U);
  EXPECT_EQ(temperature->function, protocol::FunctionCode::read_input_registers);
  EXPECT_EQ(temperature->address, 0U);
  EXPECT_EQ(temperature->register_count, 1U);
  EXPECT_EQ(temperature->data_type, RegisterDataType::int16);
  EXPECT_DOUBLE_EQ(temperature->scale, 0.1);
  EXPECT_EQ(temperature->topic, "industrial_iot_gateway/devices/environment_sensor/registers/"
                                "ambient_temperature_c");

  const auto *pressure = loaded.configuration->find_register(4U);
  ASSERT_NE(pressure, nullptr);
  EXPECT_EQ(pressure->data_type, RegisterDataType::float32);
  EXPECT_EQ(pressure->register_count, 2U);
}

TEST(RuntimeConfigTest, DecodesSignedScaledFloatAndUint32Values) {
  RuntimeRegisterDefinition signed_value{};
  signed_value.data_type = RegisterDataType::int16;
  signed_value.register_count = 1U;
  signed_value.scale = 0.1;
  const auto decoded_signed = decode_engineering_value(signed_value, {0xFF9CU});
  ASSERT_TRUE(decoded_signed);
  ASSERT_TRUE(decoded_signed.value.has_value());
  EXPECT_DOUBLE_EQ(decoded_signed.value.value_or(0.0), -10.0);

  RuntimeRegisterDefinition float_value{};
  float_value.data_type = RegisterDataType::float32;
  float_value.register_count = 2U;
  const auto decoded_float = decode_engineering_value(float_value, {0x42C8U, 0x0000U});
  ASSERT_TRUE(decoded_float);
  ASSERT_TRUE(decoded_float.value.has_value());
  EXPECT_FLOAT_EQ(static_cast<float>(decoded_float.value.value_or(0.0)), 100.0F);

  RuntimeRegisterDefinition uint32_value{};
  uint32_value.data_type = RegisterDataType::uint32;
  uint32_value.register_count = 2U;
  const auto decoded_uint32 = decode_engineering_value(uint32_value, {0x0001U, 0x0002U});
  ASSERT_TRUE(decoded_uint32);
  ASSERT_TRUE(decoded_uint32.value.has_value());
  EXPECT_DOUBLE_EQ(decoded_uint32.value.value_or(0.0), 65538.0);
}

TEST(RuntimeConfigTest, RejectsMissingAndMalformedFiles) {
  const auto missing = load_runtime_configuration("/definitely/missing/register_map.yaml");
  EXPECT_FALSE(missing);
  EXPECT_EQ(missing.error, RuntimeConfigErrorCategory::file_not_found);

  const auto malformed = load_runtime_configuration(GATEWAY_INVALID_REGISTER_MAP_PATH);
  EXPECT_FALSE(malformed);
  EXPECT_NE(malformed.error, RuntimeConfigErrorCategory::none);
}

TEST(RuntimeConfigTest, RejectsWrongDecodeWordCount) {
  RuntimeRegisterDefinition definition{};
  definition.data_type = RegisterDataType::float32;
  definition.register_count = 2U;

  const auto decoded = decode_engineering_value(definition, {0x42C8U});
  EXPECT_FALSE(decoded);
  EXPECT_EQ(decoded.error, DecodeErrorCategory::wrong_register_count);
}

} // namespace
} // namespace industrial_iot_gateway::config
