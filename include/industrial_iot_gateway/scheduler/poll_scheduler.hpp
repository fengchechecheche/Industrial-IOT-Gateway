#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "industrial_iot_gateway/scheduler/scheduled_request.hpp"

namespace industrial_iot_gateway::scheduler {

enum class SchedulerErrorCategory {
  none,
  empty_poll_plan,
  duplicate_poll_job_id,
  invalid_slave_address,
  unsupported_poll_function,
  invalid_poll_quantity,
  address_range_overflow,
  invalid_poll_period,
  no_in_flight_request,
  request_id_mismatch,
  poll_job_id_mismatch,
  invalid_observation_times,
  request_id_exhausted,
};

struct SchedulerError {
  SchedulerErrorCategory category{SchedulerErrorCategory::none};
  std::uint32_t poll_job_id{};
  std::uint64_t request_id{};
};

class PollScheduler {
public:
  PollScheduler(const std::vector<PollJob> &jobs, SchedulerTimePoint start_time);

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] SchedulerError validation_error() const noexcept;
  [[nodiscard]] std::size_t poll_job_count() const noexcept;

  [[nodiscard]] std::optional<ScheduledRequest> dispatch_next(SchedulerTimePoint now) noexcept;
  [[nodiscard]] SchedulerError complete_request(const RequestObservation &observation) noexcept;

  [[nodiscard]] bool has_in_flight_request() const noexcept;
  [[nodiscard]] std::optional<SchedulerTimePoint> next_due_time() const noexcept;
  [[nodiscard]] const SlavePollStatistics &slave_statistics(std::uint8_t slave_id) const noexcept;

private:
  struct JobState {
    PollJob job{};
    SchedulerTimePoint next_due_at{};
    std::optional<SchedulerTimePoint> last_dispatched_at{};
  };

  [[nodiscard]] SchedulerError validate_jobs() const noexcept;
  [[nodiscard]] std::optional<std::size_t> select_due_job(SchedulerTimePoint now) const noexcept;
  [[nodiscard]] bool candidate_precedes(std::size_t candidate_index,
                                        std::size_t current_index) const noexcept;
  [[nodiscard]] bool observation_times_valid(const RequestObservation &observation) const noexcept;

  static constexpr std::size_t kSlaveStatisticsCount = 256U;
  std::vector<JobState> jobs_{};
  std::array<SlavePollStatistics, kSlaveStatisticsCount> slave_statistics_{};
  bool valid_{};
  SchedulerError validation_error_{};
  std::optional<ScheduledRequest> in_flight_{};
  std::uint64_t next_request_id_{1U};
  std::optional<std::uint8_t> last_dispatched_slave_id_{};
};

} // namespace industrial_iot_gateway::scheduler
