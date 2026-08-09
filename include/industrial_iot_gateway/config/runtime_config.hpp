#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "industrial_iot_gateway/quality/freshness_tracker.hpp"
#include "industrial_iot_gateway/scheduler/scheduled_request.hpp"
#include "industrial_iot_gateway/types/register_value.hpp"

namespace industrial_iot_gateway::config {

enum class RegisterDataType {
  uint16,
  int16,
  uint32,
  int32,
  float32,
};

enum class RuntimeConfigErrorCategory {
  none,
  file_not_found,
  parse_error,
  invalid_schema,
  invalid_device,
  invalid_register,
};

struct RuntimeRegisterDefinition {
  std::uint32_t poll_job_id{};
  std::uint8_t slave_id{};
  std::string device_name{};
  std::string register_name{};
  protocol::FunctionCode function{protocol::FunctionCode::read_holding_registers};
  std::uint16_t address{};
  std::uint16_t register_count{};
  RegisterDataType data_type{RegisterDataType::uint16};
  double scale{1.0};
  double offset{};
  std::string unit{};
  std::string topic{};
  std::chrono::milliseconds freshness{};
  std::vector<types::RegisterValue> invalid_raw_values{};
};

struct RuntimeConfiguration {
  std::size_t device_count{};
  std::vector<RuntimeRegisterDefinition> registers{};
  std::vector<scheduler::PollJob> poll_jobs{};
  std::vector<quality::RegisterFreshnessConfig> freshness{};

  [[nodiscard]] const RuntimeRegisterDefinition *
  find_register(std::uint32_t poll_job_id) const noexcept;
};

struct RuntimeConfigLoadResult {
  std::unique_ptr<RuntimeConfiguration> configuration{};
  RuntimeConfigErrorCategory error{RuntimeConfigErrorCategory::none};
  std::string detail{};

  [[nodiscard]] explicit operator bool() const noexcept;
};

enum class DecodeErrorCategory {
  none,
  wrong_register_count,
  non_finite_value,
  invalid_raw_value,
};

struct DecodeResult {
  std::optional<double> value{};
  DecodeErrorCategory error{DecodeErrorCategory::none};
  std::optional<types::RegisterValue> raw_value{};

  [[nodiscard]] explicit operator bool() const noexcept;
};

[[nodiscard]] RuntimeConfigLoadResult
load_runtime_configuration(const std::string &register_map_path) noexcept;
[[nodiscard]] DecodeResult
decode_engineering_value(const RuntimeRegisterDefinition &definition,
                         const std::vector<std::uint16_t> &words) noexcept;

} // namespace industrial_iot_gateway::config
