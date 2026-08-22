#include "industrial_iot_gateway/config/runtime_config.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <utility>

#include <yaml-cpp/yaml.h>

namespace industrial_iot_gateway::config {
namespace {

[[nodiscard]] protocol::FunctionCode parse_function(const std::string &text) {
  if (text == "0x03") {
    return protocol::FunctionCode::read_holding_registers;
  }
  if (text == "0x04") {
    return protocol::FunctionCode::read_input_registers;
  }
  throw std::runtime_error("unsupported poll function");
}

[[nodiscard]] RegisterDataType parse_data_type(const std::string &text) {
  if (text == "uint16") {
    return RegisterDataType::uint16;
  }
  if (text == "int16") {
    return RegisterDataType::int16;
  }
  if (text == "uint32") {
    return RegisterDataType::uint32;
  }
  if (text == "int32") {
    return RegisterDataType::int32;
  }
  if (text == "float32") {
    return RegisterDataType::float32;
  }
  throw std::runtime_error("unsupported register data type");
}

[[nodiscard]] std::uint16_t expected_word_count(RegisterDataType type) noexcept {
  return type == RegisterDataType::uint16 || type == RegisterDataType::int16 ? 1U : 2U;
}

[[nodiscard]] types::RegisterValue parse_raw_value(const YAML::Node &node, RegisterDataType type) {
  switch (type) {
  case RegisterDataType::uint16: {
    const auto value = node.as<std::uint64_t>();
    if (value > std::numeric_limits<std::uint16_t>::max()) {
      throw std::runtime_error("invalid uint16 raw sentinel");
    }
    return value;
  }
  case RegisterDataType::int16: {
    const auto value = node.as<std::int64_t>();
    if (value < std::numeric_limits<std::int16_t>::min() ||
        value > std::numeric_limits<std::int16_t>::max()) {
      throw std::runtime_error("invalid int16 raw sentinel");
    }
    return value;
  }
  case RegisterDataType::uint32: {
    const auto value = node.as<std::uint64_t>();
    if (value > std::numeric_limits<std::uint32_t>::max()) {
      throw std::runtime_error("invalid uint32 raw sentinel");
    }
    return value;
  }
  case RegisterDataType::int32: {
    const auto value = node.as<std::int64_t>();
    if (value < std::numeric_limits<std::int32_t>::min() ||
        value > std::numeric_limits<std::int32_t>::max()) {
      throw std::runtime_error("invalid int32 raw sentinel");
    }
    return value;
  }
  case RegisterDataType::float32: {
    const auto value = node.as<double>();
    if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max()) {
      throw std::runtime_error("invalid float32 raw sentinel");
    }
    return value;
  }
  }
  throw std::runtime_error("unsupported raw sentinel type");
}

[[nodiscard]] RuntimeConfigLoadResult failure(RuntimeConfigErrorCategory error,
                                              std::string detail) {
  return {nullptr, error, std::move(detail)};
}

} // namespace

const RuntimeRegisterDefinition *
RuntimeConfiguration::find_register(const std::uint32_t poll_job_id) const noexcept {
  for (const auto &definition : registers) {
    if (definition.poll_job_id == poll_job_id) {
      return &definition;
    }
  }
  return nullptr;
}

RuntimeConfigLoadResult::operator bool() const noexcept {
  return configuration != nullptr && error == RuntimeConfigErrorCategory::none;
}

DecodeResult::operator bool() const noexcept {
  return value.has_value() && error == DecodeErrorCategory::none;
}

RuntimeConfigLoadResult load_runtime_configuration(const std::string &register_map_path) noexcept {
  std::ifstream input(register_map_path);
  if (!input.good()) {
    return failure(RuntimeConfigErrorCategory::file_not_found,
                   "register map does not exist: " + register_map_path);
  }
  input.close();

  try {
    const YAML::Node root = YAML::LoadFile(register_map_path);
    if (!root["schema_version"] || root["schema_version"].as<std::string>() != "1.0.0" ||
        !root["devices"] || !root["devices"].IsSequence()) {
      return failure(RuntimeConfigErrorCategory::invalid_schema,
                     "register map schema or devices section is invalid");
    }

    auto configuration = std::make_unique<RuntimeConfiguration>();
    if (const auto protocol_contract = root["protocol_contract"]) {
      if (!protocol_contract.IsMap()) {
        return failure(RuntimeConfigErrorCategory::invalid_schema,
                       "protocol_contract must be a mapping");
      }
      if (const auto interval = protocol_contract["minimum_request_interval_ms"]) {
        const auto interval_ms = interval.as<std::uint32_t>();
        constexpr std::uint32_t maximum_interval_ms = 10000U;
        if (interval_ms > maximum_interval_ms) {
          return failure(RuntimeConfigErrorCategory::invalid_schema,
                         "minimum_request_interval_ms exceeds 10000");
        }
        configuration->minimum_request_interval = std::chrono::milliseconds(interval_ms);
      }
    }
    std::array<bool, 248U> seen_slaves{};
    std::uint32_t next_poll_job_id = 1U;
    for (const auto &device : root["devices"]) {
      if (!device["enabled"] || !device["enabled"].as<bool>()) {
        continue;
      }
      const auto slave_value = device["slave_id"].as<unsigned int>();
      if (slave_value == 0U || slave_value > 247U || seen_slaves[slave_value]) {
        return failure(RuntimeConfigErrorCategory::invalid_device,
                       "enabled device has invalid or duplicate slave_id");
      }
      seen_slaves[slave_value] = true;
      ++configuration->device_count;
      const auto slave_id = static_cast<std::uint8_t>(slave_value);
      const auto device_name = device["name"].as<std::string>();
      if (!device["registers"] || !device["registers"].IsSequence()) {
        return failure(RuntimeConfigErrorCategory::invalid_device,
                       "enabled device has no register sequence");
      }

      for (const auto &entry : device["registers"]) {
        const auto poll = entry["poll"];
        if (!poll || !poll["enabled"] || !poll["enabled"].as<bool>()) {
          continue;
        }
        RuntimeRegisterDefinition definition{};
        definition.poll_job_id = next_poll_job_id++;
        definition.slave_id = slave_id;
        definition.device_name = device_name;
        definition.register_name = entry["name"].as<std::string>();
        definition.function = parse_function(entry["function"].as<std::string>());
        definition.address = entry["address"].as<std::uint16_t>();
        definition.register_count = entry["register_count"].as<std::uint16_t>();
        definition.data_type = parse_data_type(entry["data_type"].as<std::string>());
        definition.scale = entry["scale"].as<double>();
        definition.offset = entry["offset"].as<double>();
        definition.unit = entry["unit"].as<std::string>();
        definition.topic = entry["mqtt"]["topic"].as<std::string>();
        const auto period = std::chrono::milliseconds(poll["poll_period_ms"].as<std::uint32_t>());
        const auto freshness = std::chrono::milliseconds(poll["freshness_ms"].as<std::uint32_t>());
        definition.freshness = freshness;
        if (const auto invalid_values = entry["invalid_raw_values"]) {
          if (!invalid_values.IsSequence()) {
            return failure(RuntimeConfigErrorCategory::invalid_register,
                           "invalid_raw_values must be a sequence: " + definition.register_name);
          }
          for (const auto &invalid_value : invalid_values) {
            definition.invalid_raw_values.push_back(
                parse_raw_value(invalid_value, definition.data_type));
          }
        }
        if (definition.register_count != expected_word_count(definition.data_type) ||
            definition.scale == 0.0 || period.count() <= 0 || freshness < period ||
            definition.topic.empty()) {
          return failure(RuntimeConfigErrorCategory::invalid_register,
                         "register contract is invalid: " + definition.register_name);
        }

        scheduler::PollJob job{};
        job.poll_job_id = definition.poll_job_id;
        job.request = {definition.slave_id, definition.function, definition.address,
                       definition.register_count};
        job.poll_period = period;
        job.priority = scheduler::PollPriority::normal;
        configuration->registers.push_back(definition);
        configuration->poll_jobs.push_back(job);
        configuration->freshness.push_back(
            {definition.poll_job_id, definition.slave_id, period, freshness});
      }
    }
    if (configuration->device_count == 0U || configuration->registers.empty()) {
      return failure(RuntimeConfigErrorCategory::invalid_schema,
                     "register map contains no enabled poll plan");
    }
    return {std::move(configuration), RuntimeConfigErrorCategory::none, {}};
  } catch (const YAML::Exception &error) {
    return failure(RuntimeConfigErrorCategory::parse_error, error.what());
  } catch (const std::exception &error) {
    return failure(RuntimeConfigErrorCategory::invalid_register, error.what());
  } catch (...) {
    return failure(RuntimeConfigErrorCategory::parse_error, "unknown register map error");
  }
}

DecodeResult decode_engineering_value(const RuntimeRegisterDefinition &definition,
                                      const std::vector<std::uint16_t> &words) noexcept {
  if (words.size() != definition.register_count ||
      definition.register_count != expected_word_count(definition.data_type)) {
    return {std::nullopt, DecodeErrorCategory::wrong_register_count, std::nullopt};
  }

  types::RegisterValue raw_value{std::uint64_t{0U}};
  switch (definition.data_type) {
  case RegisterDataType::uint16:
    raw_value = static_cast<std::uint64_t>(words[0]);
    break;
  case RegisterDataType::int16:
    raw_value = static_cast<std::int64_t>(static_cast<std::int16_t>(words[0]));
    break;
  case RegisterDataType::uint32: {
    const auto bits = (static_cast<std::uint32_t>(words[0]) << 16U) | words[1];
    raw_value = static_cast<std::uint64_t>(bits);
    break;
  }
  case RegisterDataType::int32: {
    const auto bits = (static_cast<std::uint32_t>(words[0]) << 16U) | words[1];
    raw_value = static_cast<std::int64_t>(static_cast<std::int32_t>(bits));
    break;
  }
  case RegisterDataType::float32: {
    const auto bits = (static_cast<std::uint32_t>(words[0]) << 16U) | words[1];
    float decoded{};
    static_assert(sizeof(decoded) == sizeof(bits));
    std::memcpy(&decoded, &bits, sizeof(decoded));
    if (!std::isfinite(decoded)) {
      return {std::nullopt, DecodeErrorCategory::non_finite_value, std::nullopt};
    }
    raw_value = static_cast<double>(decoded);
    break;
  }
  }
  if (std::find(definition.invalid_raw_values.begin(), definition.invalid_raw_values.end(),
                raw_value) != definition.invalid_raw_values.end()) {
    return {std::nullopt, DecodeErrorCategory::invalid_raw_value, raw_value};
  }
  const double numeric_raw =
      std::visit([](const auto value) noexcept { return static_cast<double>(value); }, raw_value);
  const double engineering_value = numeric_raw * definition.scale + definition.offset;
  if (!std::isfinite(engineering_value)) {
    return {std::nullopt, DecodeErrorCategory::non_finite_value, raw_value};
  }
  return {engineering_value, DecodeErrorCategory::none, raw_value};
}

} // namespace industrial_iot_gateway::config
