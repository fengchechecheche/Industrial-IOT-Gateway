#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "industrial_iot_gateway/scheduler/reliability_policy.hpp"
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
  invalid_policy_configuration,
  invalid_device_health_configuration,
  no_in_flight_request,
  request_id_mismatch,
  poll_job_id_mismatch,
  attempt_mismatch,
  invalid_request_state,
  invalid_observation_times,
  request_id_exhausted,
};

struct SchedulerError {
  SchedulerErrorCategory category{SchedulerErrorCategory::none};
  std::uint32_t poll_job_id{};
  std::uint64_t request_id{};
};

enum class DeviceHealthState {
  online,
  offline,
  probing,
};

struct DeviceHealthStatistics {
  DeviceHealthState state{DeviceHealthState::online};
  std::uint32_t consecutive_final_failures{};
  std::uint32_t consecutive_probe_successes{};
  std::uint64_t offline_transitions{};
  std::uint64_t probes_dispatched{};
  std::uint64_t probe_successes{};
  std::uint64_t recovery_transitions{};
  std::optional<SchedulerTimePoint> next_probe_at{};
};

struct SchedulerPolicyConfig {
  ReliabilityConfig reliability{};
  std::uint32_t consecutive_final_failures_to_offline{3U};
  std::chrono::milliseconds offline_probe_interval{5000};
  std::uint32_t consecutive_probe_successes_to_recover{2U};
};

struct SchedulerTransition {
  SchedulerError error{};
  bool occurred{};
  bool terminal{};
  std::uint64_t request_id{};
  std::uint32_t poll_job_id{};
  std::uint32_t attempt{};
  RequestState previous_state{RequestState::queued};
  RequestState next_state{RequestState::queued};
  RequestResultCategory result{RequestResultCategory::success};
  std::optional<std::uint8_t> remote_exception_code{};
  std::optional<SchedulerTimePoint> retry_at{};
  bool device_state_changed{};
  DeviceHealthState device_state{DeviceHealthState::online};
};

class PollScheduler {
public:
  PollScheduler(const std::vector<PollJob> &jobs, SchedulerTimePoint start_time);
  PollScheduler(const std::vector<PollJob> &jobs, SchedulerTimePoint start_time,
                SchedulerPolicyConfig policy_config);

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] SchedulerError validation_error() const noexcept;
  [[nodiscard]] std::size_t poll_job_count() const noexcept;

  [[nodiscard]] std::optional<ScheduledRequest> dispatch_next(SchedulerTimePoint now) noexcept;
  [[nodiscard]] SchedulerError complete_request(const RequestObservation &observation) noexcept;
  [[nodiscard]] SchedulerError mark_attempt_sent(std::uint64_t request_id, std::uint32_t attempt,
                                                 SchedulerTimePoint sent_at) noexcept;
  [[nodiscard]] SchedulerTransition record_attempt_result(const AttemptResult &result) noexcept;
  [[nodiscard]] std::optional<SchedulerTransition>
  on_time_advanced(SchedulerTimePoint now) noexcept;

  [[nodiscard]] bool has_in_flight_request() const noexcept;
  [[nodiscard]] std::optional<RequestState> in_flight_request_state() const noexcept;
  [[nodiscard]] std::optional<SchedulerTimePoint> next_due_time() const noexcept;
  [[nodiscard]] std::optional<SchedulerTimePoint> next_wake_time() const noexcept;
  [[nodiscard]] const SlavePollStatistics &slave_statistics(std::uint8_t slave_id) const noexcept;
  [[nodiscard]] const DeviceHealthStatistics &
  device_health_statistics(std::uint8_t slave_id) const noexcept;

private:
  struct ActiveRequest {
    ScheduledRequest request{};
    std::optional<SchedulerTimePoint> sent_at{};
    std::optional<SchedulerTimePoint> attempt_deadline{};
  };

  struct JobState {
    PollJob job{};
    SchedulerTimePoint next_due_at{};
    std::optional<SchedulerTimePoint> last_dispatched_at{};
    std::optional<ActiveRequest> active{};
  };

  struct Candidate {
    std::size_t job_index{};
    SchedulerTimePoint eligible_at{};
    bool is_retry{};
    bool is_probe{};
  };

  [[nodiscard]] SchedulerError validate_jobs() const noexcept;
  [[nodiscard]] SchedulerError validate_policy() const noexcept;
  [[nodiscard]] std::optional<Candidate>
  select_due_candidate(SchedulerTimePoint now) const noexcept;
  [[nodiscard]] bool candidate_precedes(const Candidate &candidate,
                                        const Candidate &current) const noexcept;
  [[nodiscard]] bool observation_times_valid(const RequestObservation &observation) const noexcept;
  [[nodiscard]] bool has_active_request_for_slave(std::uint8_t slave_id) const noexcept;
  [[nodiscard]] std::optional<std::size_t>
  active_job_index(std::uint64_t request_id) const noexcept;
  [[nodiscard]] SchedulerTransition process_attempt_result(const AttemptResult &result) noexcept;
  void advance_period(JobState &state, SchedulerTimePoint now) noexcept;
  void finish_active_request(std::size_t job_index, const AttemptResult &result,
                             SchedulerTransition &transition) noexcept;
  void apply_terminal_health_result(std::size_t job_index, bool is_recovery_probe,
                                    const AttemptResult &result,
                                    SchedulerTransition &transition) noexcept;
  void initialize_devices() noexcept;

  static constexpr std::size_t kSlaveStatisticsCount = 256U;
  std::vector<JobState> jobs_{};
  std::array<SlavePollStatistics, kSlaveStatisticsCount> slave_statistics_{};
  std::array<DeviceHealthStatistics, kSlaveStatisticsCount> device_health_{};
  std::array<std::optional<std::uint32_t>, kSlaveStatisticsCount> recovery_job_id_{};
  SchedulerPolicyConfig policy_config_{};
  ReliabilityPolicy reliability_policy_{};
  bool valid_{};
  SchedulerError validation_error_{};
  std::optional<std::size_t> in_flight_job_index_{};
  std::uint64_t next_request_id_{1U};
  std::optional<std::uint8_t> last_dispatched_slave_id_{};
};

} // namespace industrial_iot_gateway::scheduler
