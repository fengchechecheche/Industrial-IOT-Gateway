#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
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

TEST(RuntimeConfigTest, LoadsStm32Address4ReadOnlyHardwareProfile) {
  using namespace std::chrono_literals;
  struct ExpectedRegister {
    std::string_view name;
    std::uint16_t address;
    RegisterDataType data_type;
    std::uint16_t register_count;
    double scale;
    std::chrono::milliseconds poll_period;
    std::chrono::milliseconds freshness;
  };
  constexpr std::array expected{
      ExpectedRegister{"device_signature", 0U, RegisterDataType::uint16, 1U, 1.0, 5000ms, 15000ms},
      ExpectedRegister{"register_image_generation", 8U, RegisterDataType::uint32, 2U, 1.0, 1000ms,
                       3000ms},
      ExpectedRegister{"source_value_present_mask", 12U, RegisterDataType::uint16, 1U, 1.0, 1000ms,
                       3000ms},
      ExpectedRegister{"bme280_temperature", 18U, RegisterDataType::int32, 2U, 0.01, 1000ms,
                       3000ms},
      ExpectedRegister{"bme280_pressure", 20U, RegisterDataType::uint32, 2U, 0.001, 1000ms, 3000ms},
      ExpectedRegister{"bme280_humidity", 22U, RegisterDataType::uint32, 2U, 0.001, 1000ms, 3000ms},
      ExpectedRegister{"veml7700_illuminance", 24U, RegisterDataType::uint32, 2U, 0.001, 1000ms,
                       3000ms},
      ExpectedRegister{"adxl345_acceleration_x", 26U, RegisterDataType::int32, 2U, 0.001, 1000ms,
                       3000ms},
      ExpectedRegister{"adxl345_acceleration_y", 28U, RegisterDataType::int32, 2U, 0.001, 1000ms,
                       3000ms},
      ExpectedRegister{"adxl345_acceleration_z", 30U, RegisterDataType::int32, 2U, 0.001, 1000ms,
                       3000ms},
      ExpectedRegister{"adxl345_resultant_rms", 50U, RegisterDataType::uint32, 2U, 0.001, 1000ms,
                       3000ms},
      ExpectedRegister{"bme280_sequence", 56U, RegisterDataType::uint32, 2U, 1.0, 2000ms, 6000ms},
      ExpectedRegister{"veml7700_sequence", 66U, RegisterDataType::uint32, 2U, 1.0, 2000ms, 6000ms},
      ExpectedRegister{"adxl345_sample_sequence", 76U, RegisterDataType::uint32, 2U, 1.0, 2000ms,
                       6000ms},
      ExpectedRegister{"adxl345_feature_sequence", 86U, RegisterDataType::uint32, 2U, 1.0, 2000ms,
                       6000ms},
      ExpectedRegister{"health_state", 117U, RegisterDataType::uint16, 1U, 1.0, 1000ms, 3000ms},
      ExpectedRegister{"rs485_error_count", 120U, RegisterDataType::uint32, 2U, 1.0, 2000ms,
                       6000ms},
  };

  const auto loaded = load_runtime_configuration(GATEWAY_STM32_ADDRESS4_MAP_PATH);
  ASSERT_TRUE(loaded) << loaded.detail;
  ASSERT_NE(loaded.configuration, nullptr);
  EXPECT_EQ(loaded.configuration->device_count, 1U);
  ASSERT_EQ(loaded.configuration->registers.size(), expected.size());
  ASSERT_EQ(loaded.configuration->poll_jobs.size(), expected.size());
  ASSERT_EQ(loaded.configuration->freshness.size(), expected.size());

  for (std::size_t index = 0U; index < expected.size(); ++index) {
    const auto &definition = loaded.configuration->registers[index];
    const auto &job = loaded.configuration->poll_jobs[index];
    const auto &freshness = loaded.configuration->freshness[index];
    const auto &contract = expected[index];

    EXPECT_EQ(definition.poll_job_id, index + 1U);
    EXPECT_EQ(definition.slave_id, 4U);
    EXPECT_EQ(definition.device_name, "stm32_condition_node");
    EXPECT_EQ(definition.register_name, contract.name);
    EXPECT_EQ(definition.function, protocol::FunctionCode::read_input_registers);
    EXPECT_EQ(definition.address, contract.address);
    EXPECT_EQ(definition.data_type, contract.data_type);
    EXPECT_EQ(definition.register_count, contract.register_count);
    EXPECT_DOUBLE_EQ(definition.scale, contract.scale);
    EXPECT_EQ(definition.freshness, contract.freshness);
    EXPECT_EQ(definition.topic, "industrial_iot_gateway/devices/stm32_condition_node/registers/" +
                                    std::string(contract.name));

    EXPECT_EQ(job.poll_job_id, index + 1U);
    EXPECT_EQ(job.request.slave_id, 4U);
    EXPECT_EQ(job.request.function, protocol::FunctionCode::read_input_registers);
    EXPECT_EQ(job.request.start_address, contract.address);
    EXPECT_EQ(job.request.quantity, contract.register_count);
    EXPECT_EQ(job.poll_period, contract.poll_period);

    EXPECT_EQ(freshness.poll_job_id, index + 1U);
    EXPECT_EQ(freshness.slave_id, 4U);
  }

  std::ifstream profile(GATEWAY_STM32_ADDRESS4_MAP_PATH);
  ASSERT_TRUE(profile.good());
  const std::string profile_text{std::istreambuf_iterator<char>{profile},
                                 std::istreambuf_iterator<char>{}};
  EXPECT_EQ(profile_text.find("write_function"), std::string::npos);
  EXPECT_EQ(profile_text.find("function: \"0x03\""), std::string::npos);
  EXPECT_EQ(profile_text.find("function: \"0x06\""), std::string::npos);
}

TEST(RuntimeConfigTest, DecodesStm32Address4RepresentativeValues) {
  const auto loaded = load_runtime_configuration(GATEWAY_STM32_ADDRESS4_MAP_PATH);
  ASSERT_TRUE(loaded) << loaded.detail;

  const auto find_by_name = [&loaded](const std::string_view name) {
    for (const auto &definition : loaded.configuration->registers) {
      if (definition.register_name == name) {
        return &definition;
      }
    }
    return static_cast<const RuntimeRegisterDefinition *>(nullptr);
  };

  const auto *signature = find_by_name("device_signature");
  ASSERT_NE(signature, nullptr);
  const auto decoded_signature = decode_engineering_value(*signature, {0x5035U});
  ASSERT_TRUE(decoded_signature);
  EXPECT_DOUBLE_EQ(decoded_signature.value.value_or(0.0), 20533.0);

  const auto *temperature = find_by_name("bme280_temperature");
  ASSERT_NE(temperature, nullptr);
  const auto decoded_temperature = decode_engineering_value(*temperature, {0x0000U, 0x09C4U});
  ASSERT_TRUE(decoded_temperature);
  EXPECT_DOUBLE_EQ(decoded_temperature.value.value_or(0.0), 25.0);

  const auto *acceleration_x = find_by_name("adxl345_acceleration_x");
  ASSERT_NE(acceleration_x, nullptr);
  const auto decoded_acceleration = decode_engineering_value(*acceleration_x, {0xFFFFU, 0xFC18U});
  ASSERT_TRUE(decoded_acceleration);
  EXPECT_DOUBLE_EQ(decoded_acceleration.value.value_or(0.0), -1.0);

  const auto *illuminance = find_by_name("veml7700_illuminance");
  ASSERT_NE(illuminance, nullptr);
  const auto decoded_illuminance = decode_engineering_value(*illuminance, {0x0000U, 0x3039U});
  ASSERT_TRUE(decoded_illuminance);
  EXPECT_DOUBLE_EQ(decoded_illuminance.value.value_or(0.0), 12.345);
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

TEST(RuntimeConfigTest, PreservesRawValueAndRejectsConfiguredSentinel) {
  RuntimeRegisterDefinition definition{};
  definition.data_type = RegisterDataType::int16;
  definition.register_count = 1U;
  definition.scale = 0.1;
  definition.invalid_raw_values = {std::int64_t{-32768}};

  const auto valid = decode_engineering_value(definition, {0xFF9CU});
  ASSERT_TRUE(valid);
  ASSERT_TRUE(valid.raw_value.has_value());
  EXPECT_EQ(std::get<std::int64_t>(*valid.raw_value), -100);
  EXPECT_DOUBLE_EQ(valid.value.value_or(0.0), -10.0);

  const auto invalid = decode_engineering_value(definition, {0x8000U});
  EXPECT_FALSE(invalid);
  EXPECT_EQ(invalid.error, DecodeErrorCategory::invalid_raw_value);
  ASSERT_TRUE(invalid.raw_value.has_value());
  EXPECT_EQ(std::get<std::int64_t>(*invalid.raw_value), -32768);
}

} // namespace
} // namespace industrial_iot_gateway::config
