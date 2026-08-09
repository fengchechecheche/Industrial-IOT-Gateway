#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "industrial_iot_gateway/quality/freshness_tracker.hpp"
#include "industrial_iot_gateway/scheduler/scheduled_request.hpp"

namespace industrial_iot_gateway::pipeline {

struct MeasurementRaw {
  std::uint64_t request_id{};
  std::uint8_t slave_id{};
  protocol::FunctionCode function{protocol::FunctionCode::read_holding_registers};
  std::uint16_t start_address{};
  std::vector<std::uint16_t> registers{};
  scheduler::SchedulerTimePoint received_at{};
};

struct RequestResultEvent {
  scheduler::AttemptResult result{};
  bool terminal{};
};

struct QualityTransitionEvent {
  quality::QualityTransition transition{};
};

using MeasurementMessage = std::variant<MeasurementRaw, RequestResultEvent, QualityTransitionEvent>;

struct TelemetryEvent {
  std::uint64_t event_id{};
  std::optional<std::uint64_t> request_id{};
  std::string topic{};
  quality::RegisterQuality quality{quality::RegisterQuality::no_valid_sample};
  std::string quality_reason{};
  bool value_is_retained{};
  std::optional<double> value{};
};

struct WriteAuditEvent {
  std::uint64_t event_id{};
  std::uint64_t request_id{};
  std::uint8_t slave_id{};
  std::uint16_t address{};
  scheduler::RequestResultCategory result{scheduler::RequestResultCategory::success};
};

using PublishMessage = std::variant<TelemetryEvent, QualityTransitionEvent, WriteAuditEvent>;

} // namespace industrial_iot_gateway::pipeline
