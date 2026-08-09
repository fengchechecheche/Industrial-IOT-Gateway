#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

#include "industrial_iot_gateway/protocol/modbus_codec.hpp"

namespace industrial_iot_gateway::scheduler {

using SchedulerClock = std::chrono::steady_clock;
using SchedulerTimePoint = SchedulerClock::time_point;

enum class PollPriority : std::uint8_t {
  low = 0U,
  normal = 1U,
  high = 2U,
};

enum class RequestKind {
  poll_read,
  explicit_read,
  explicit_write,
};

enum class RequestSource {
  poll_scheduler,
  local_test_runner,
  explicit_control_api,
};

struct PollJob {
  std::uint32_t poll_job_id{};
  protocol::ReadRequest request{};
  std::chrono::milliseconds poll_period{};
  PollPriority priority{PollPriority::normal};
};

struct ScheduledRequest {
  std::uint64_t request_id{};
  std::uint32_t poll_job_id{};
  RequestKind request_kind{RequestKind::poll_read};
  protocol::Request request{};
  SchedulerTimePoint due_at{};
  SchedulerTimePoint created_at{};
  SchedulerTimePoint enqueued_at{};
  std::uint32_t attempt{1U};
  RequestSource source{RequestSource::poll_scheduler};
};

struct RequestObservation {
  std::uint64_t request_id{};
  std::uint32_t poll_job_id{};
  bool succeeded{};
  std::optional<SchedulerTimePoint> sent_at{};
  std::optional<SchedulerTimePoint> first_byte_at{};
  SchedulerTimePoint completed_at{};
};

struct RequestTiming {
  SchedulerTimePoint due_at{};
  SchedulerTimePoint enqueued_at{};
  std::optional<SchedulerTimePoint> sent_at{};
  std::optional<SchedulerTimePoint> first_byte_at{};
  SchedulerTimePoint completed_at{};
};

struct SlavePollStatistics {
  std::uint64_t requests_dispatched{};
  std::uint64_t requests_completed{};
  std::uint64_t requests_failed{};
  std::uint64_t poll_periods_coalesced{};
  std::optional<SchedulerTimePoint> last_dispatched_at{};
  std::optional<SchedulerTimePoint> last_completed_at{};
  std::chrono::microseconds maximum_dispatch_lateness{};
  std::optional<RequestTiming> last_timing{};
};

} // namespace industrial_iot_gateway::scheduler
