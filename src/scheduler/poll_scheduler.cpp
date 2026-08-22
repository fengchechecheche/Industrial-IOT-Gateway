#include "industrial_iot_gateway/scheduler/poll_scheduler.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

namespace industrial_iot_gateway::scheduler {
namespace {

[[nodiscard]] bool is_supported_poll_function(protocol::FunctionCode function) noexcept {
  return function == protocol::FunctionCode::read_holding_registers ||
         function == protocol::FunctionCode::read_input_registers;
}

[[nodiscard]] std::uint16_t
cyclic_slave_distance(std::uint8_t slave_id, std::optional<std::uint8_t> last_slave_id) noexcept {
  if (!last_slave_id.has_value()) {
    return slave_id;
  }
  constexpr std::uint16_t slave_count = 247U;
  const auto current = static_cast<std::uint16_t>(slave_id);
  const auto last = static_cast<std::uint16_t>(*last_slave_id);
  const auto distance = static_cast<std::uint16_t>((current + slave_count - last) % slave_count);
  return distance == 0U ? slave_count : distance;
}

[[nodiscard]] bool is_final_communication_failure(RequestResultCategory category) noexcept {
  return category == RequestResultCategory::response_timeout ||
         category == RequestResultCategory::crc_mismatch ||
         category == RequestResultCategory::truncated_frame ||
         category == RequestResultCategory::serial_io_transient ||
         category == RequestResultCategory::deadline_exceeded;
}

} // namespace

PollScheduler::PollScheduler(const std::vector<PollJob> &jobs, SchedulerTimePoint start_time)
    : PollScheduler(jobs, start_time, SchedulerPolicyConfig{}) {}

PollScheduler::PollScheduler(const std::vector<PollJob> &jobs, SchedulerTimePoint start_time,
                             SchedulerPolicyConfig policy_config)
    : policy_config_(policy_config), reliability_policy_(policy_config.reliability) {
  jobs_.reserve(jobs.size());
  for (const auto &job : jobs) {
    jobs_.push_back(JobState{job, start_time, std::nullopt, std::nullopt});
  }
  initialize_devices();

  validation_error_ = validate_jobs();
  if (validation_error_.category == SchedulerErrorCategory::none) {
    validation_error_ = validate_policy();
  }
  valid_ = validation_error_.category == SchedulerErrorCategory::none;
}

bool PollScheduler::valid() const noexcept { return valid_; }

SchedulerError PollScheduler::validation_error() const noexcept { return validation_error_; }

std::size_t PollScheduler::poll_job_count() const noexcept { return jobs_.size(); }

std::optional<ScheduledRequest> PollScheduler::dispatch_next(SchedulerTimePoint now) noexcept {
  if (!valid_ || in_flight_job_index_.has_value()) {
    return std::nullopt;
  }

  const auto candidate = select_due_candidate(now);
  if (!candidate.has_value()) {
    return std::nullopt;
  }

  auto &state = jobs_[candidate->job_index];
  const auto slave_id = state.job.request.slave_id;
  auto &statistics = slave_statistics_[slave_id];

  if (candidate->is_retry) {
    if (!state.active.has_value()) {
      return std::nullopt;
    }
    auto &active = *state.active;
    active.request.state = RequestState::queued;
    active.request.enqueued_at = now;
    active.request.attempt_eligible_at = candidate->eligible_at;
    active.sent_at.reset();
    active.attempt_deadline.reset();
    in_flight_job_index_ = candidate->job_index;
    ++statistics.attempts_dispatched;
    last_dispatched_slave_id_ = slave_id;
    return active.request;
  }

  if (next_request_id_ == std::numeric_limits<std::uint64_t>::max()) {
    validation_error_ = SchedulerError{SchedulerErrorCategory::request_id_exhausted,
                                       state.job.poll_job_id, next_request_id_};
    valid_ = false;
    return std::nullopt;
  }

  const auto due_at = candidate->eligible_at;
  if (!candidate->is_probe) {
    advance_period(state, now);
  }

  ScheduledRequest request{};
  request.request_id = next_request_id_++;
  request.poll_job_id = state.job.poll_job_id;
  request.request_kind = RequestKind::poll_read;
  request.request = state.job.request;
  request.due_at = due_at;
  request.created_at = now;
  request.enqueued_at = now;
  request.deadline = reliability_policy_.request_deadline(now);
  request.attempt_eligible_at = now;
  request.attempt = 1U;
  request.max_attempts = policy_config_.reliability.max_attempts;
  request.state = RequestState::queued;
  request.source = RequestSource::poll_scheduler;
  request.is_recovery_probe = candidate->is_probe;

  state.active = ActiveRequest{request, std::nullopt, std::nullopt};
  in_flight_job_index_ = candidate->job_index;
  ++statistics.requests_dispatched;
  ++statistics.attempts_dispatched;
  statistics.last_dispatched_at = now;
  if (candidate->is_probe) {
    ++device_health_[slave_id].probes_dispatched;
  }
  last_dispatched_slave_id_ = slave_id;
  return request;
}

SchedulerError PollScheduler::complete_request(const RequestObservation &observation) noexcept {
  if (!in_flight_job_index_.has_value()) {
    return {SchedulerErrorCategory::no_in_flight_request, observation.poll_job_id,
            observation.request_id};
  }
  auto &state = jobs_[*in_flight_job_index_];
  if (!state.active.has_value()) {
    return {SchedulerErrorCategory::no_in_flight_request, observation.poll_job_id,
            observation.request_id};
  }
  if (observation.request_id != state.active->request.request_id) {
    return {SchedulerErrorCategory::request_id_mismatch, observation.poll_job_id,
            observation.request_id};
  }
  if (observation.poll_job_id != state.active->request.poll_job_id) {
    return {SchedulerErrorCategory::poll_job_id_mismatch, observation.poll_job_id,
            observation.request_id};
  }
  if (!observation_times_valid(observation)) {
    return {SchedulerErrorCategory::invalid_observation_times, observation.poll_job_id,
            observation.request_id};
  }

  auto &statistics = slave_statistics_[state.job.request.slave_id];
  ++statistics.requests_completed;
  if (!observation.succeeded) {
    ++statistics.requests_failed;
  }
  statistics.last_completed_at = observation.completed_at;
  statistics.last_timing =
      RequestTiming{state.active->request.due_at, state.active->request.enqueued_at,
                    observation.sent_at, observation.first_byte_at, observation.completed_at};
  state.active.reset();
  in_flight_job_index_.reset();
  return {};
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): public API uses distinct ID widths.
SchedulerError PollScheduler::mark_attempt_sent(std::uint64_t request_id, std::uint32_t attempt,
                                                SchedulerTimePoint sent_at) noexcept {
  if (!in_flight_job_index_.has_value()) {
    return {SchedulerErrorCategory::no_in_flight_request, 0U, request_id};
  }
  auto &state = jobs_[*in_flight_job_index_];
  if (!state.active.has_value()) {
    return {SchedulerErrorCategory::no_in_flight_request, state.job.poll_job_id, request_id};
  }
  auto &active = *state.active;
  if (request_id != active.request.request_id) {
    return {SchedulerErrorCategory::request_id_mismatch, active.request.poll_job_id, request_id};
  }
  if (attempt != active.request.attempt) {
    return {SchedulerErrorCategory::attempt_mismatch, active.request.poll_job_id, request_id};
  }
  if (active.request.state != RequestState::queued) {
    return {SchedulerErrorCategory::invalid_request_state, active.request.poll_job_id, request_id};
  }
  if (sent_at < active.request.enqueued_at || sent_at >= active.request.deadline) {
    return {SchedulerErrorCategory::invalid_observation_times, active.request.poll_job_id,
            request_id};
  }

  active.request.state = RequestState::waiting_response;
  active.sent_at = sent_at;
  active.attempt_deadline = reliability_policy_.attempt_deadline(sent_at, active.request.deadline);
  return {};
}

SchedulerTransition PollScheduler::record_attempt_result(const AttemptResult &result) noexcept {
  if (!in_flight_job_index_.has_value()) {
    SchedulerTransition transition{};
    transition.error = {SchedulerErrorCategory::no_in_flight_request, result.poll_job_id,
                        result.request_id};
    return transition;
  }
  auto &state = jobs_[*in_flight_job_index_];
  if (!state.active.has_value()) {
    SchedulerTransition transition{};
    transition.error = {SchedulerErrorCategory::no_in_flight_request, result.poll_job_id,
                        result.request_id};
    return transition;
  }
  const auto &active = *state.active;
  if (result.request_id != active.request.request_id) {
    SchedulerTransition transition{};
    transition.error = {SchedulerErrorCategory::request_id_mismatch, result.poll_job_id,
                        result.request_id};
    return transition;
  }
  if (result.poll_job_id != active.request.poll_job_id) {
    SchedulerTransition transition{};
    transition.error = {SchedulerErrorCategory::poll_job_id_mismatch, result.poll_job_id,
                        result.request_id};
    return transition;
  }
  if (result.attempt != active.request.attempt) {
    SchedulerTransition transition{};
    transition.error = {SchedulerErrorCategory::attempt_mismatch, result.poll_job_id,
                        result.request_id};
    return transition;
  }
  if (active.request.state == RequestState::queued &&
      result.category == RequestResultCategory::deadline_exceeded &&
      result.observed_at >= active.request.deadline) {
    return process_attempt_result(result);
  }
  if (active.request.state != RequestState::waiting_response || !active.sent_at.has_value() ||
      !active.attempt_deadline.has_value()) {
    SchedulerTransition transition{};
    transition.error = {SchedulerErrorCategory::invalid_request_state, result.poll_job_id,
                        result.request_id};
    return transition;
  }
  if (result.observed_at < *active.sent_at) {
    SchedulerTransition transition{};
    transition.error = {SchedulerErrorCategory::invalid_observation_times, result.poll_job_id,
                        result.request_id};
    return transition;
  }

  auto effective = result;
  if (result.observed_at > *active.attempt_deadline) {
    effective.category = *active.attempt_deadline == active.request.deadline
                             ? RequestResultCategory::deadline_exceeded
                             : RequestResultCategory::response_timeout;
    effective.remote_exception_code.reset();
  }
  return process_attempt_result(effective);
}

std::optional<SchedulerTransition>
PollScheduler::on_time_advanced(SchedulerTimePoint now) noexcept {
  if (!in_flight_job_index_.has_value()) {
    return std::nullopt;
  }
  const auto &state = jobs_[*in_flight_job_index_];
  if (!state.active.has_value()) {
    return std::nullopt;
  }
  const auto &active = *state.active;
  if (active.request.state == RequestState::queued && now >= active.request.deadline) {
    return process_attempt_result(
        AttemptResult{active.request.request_id, active.request.poll_job_id, active.request.attempt,
                      RequestResultCategory::deadline_exceeded, now, std::nullopt});
  }
  if (active.request.state == RequestState::waiting_response &&
      active.attempt_deadline.has_value() && now >= *active.attempt_deadline) {
    const auto category = *active.attempt_deadline == active.request.deadline
                              ? RequestResultCategory::deadline_exceeded
                              : RequestResultCategory::response_timeout;
    return process_attempt_result(AttemptResult{active.request.request_id,
                                                active.request.poll_job_id, active.request.attempt,
                                                category, now, std::nullopt});
  }
  return std::nullopt;
}

bool PollScheduler::has_in_flight_request() const noexcept {
  return in_flight_job_index_.has_value();
}

std::optional<RequestState> PollScheduler::in_flight_request_state() const noexcept {
  if (!in_flight_job_index_.has_value()) {
    return std::nullopt;
  }
  const auto &state = jobs_[*in_flight_job_index_];
  if (!state.active.has_value()) {
    return std::nullopt;
  }
  return state.active->request.state;
}

std::optional<SchedulerTimePoint> PollScheduler::next_due_time() const noexcept {
  return next_wake_time();
}

std::optional<SchedulerTimePoint> PollScheduler::next_wake_time() const noexcept {
  if (!valid_) {
    return std::nullopt;
  }
  if (in_flight_job_index_.has_value()) {
    const auto &state = jobs_[*in_flight_job_index_];
    if (!state.active.has_value()) {
      return std::nullopt;
    }
    if (state.active->request.state == RequestState::waiting_response) {
      return state.active->attempt_deadline;
    }
    return state.active->request.deadline;
  }

  std::optional<SchedulerTimePoint> wake{};
  for (std::size_t index = 0U; index < jobs_.size(); ++index) {
    const auto &state = jobs_[index];
    const auto slave_id = state.job.request.slave_id;
    if (state.active.has_value()) {
      if (state.active->request.state == RequestState::retry_wait) {
        wake = !wake.has_value() ? state.active->request.attempt_eligible_at
                                 : std::min(*wake, state.active->request.attempt_eligible_at);
      }
      continue;
    }
    if (has_active_request_for_slave(slave_id)) {
      continue;
    }
    const auto &health = device_health_[slave_id];
    if (health.state == DeviceHealthState::online) {
      wake = !wake.has_value() ? state.next_due_at : std::min(*wake, state.next_due_at);
    } else if (recovery_job_id_[slave_id] == state.job.poll_job_id &&
               health.next_probe_at.has_value()) {
      wake = !wake.has_value() ? *health.next_probe_at : std::min(*wake, *health.next_probe_at);
    }
  }
  return wake;
}

const SlavePollStatistics &PollScheduler::slave_statistics(std::uint8_t slave_id) const noexcept {
  return slave_statistics_[slave_id];
}

const DeviceHealthStatistics &
PollScheduler::device_health_statistics(std::uint8_t slave_id) const noexcept {
  return device_health_[slave_id];
}

SchedulerError PollScheduler::validate_jobs() const noexcept {
  if (jobs_.empty()) {
    return {SchedulerErrorCategory::empty_poll_plan, 0U, 0U};
  }
  for (std::size_t index = 0U; index < jobs_.size(); ++index) {
    const auto &job = jobs_[index].job;
    if (job.request.slave_id < 1U || job.request.slave_id > 247U) {
      return {SchedulerErrorCategory::invalid_slave_address, job.poll_job_id, 0U};
    }
    if (!is_supported_poll_function(job.request.function)) {
      return {SchedulerErrorCategory::unsupported_poll_function, job.poll_job_id, 0U};
    }
    if (job.request.quantity < 1U ||
        static_cast<std::size_t>(job.request.quantity) > protocol::kMaxReadRegisterQuantity) {
      return {SchedulerErrorCategory::invalid_poll_quantity, job.poll_job_id, 0U};
    }
    const auto last_address = static_cast<std::uint32_t>(job.request.start_address) +
                              static_cast<std::uint32_t>(job.request.quantity) - 1U;
    if (last_address > std::numeric_limits<std::uint16_t>::max()) {
      return {SchedulerErrorCategory::address_range_overflow, job.poll_job_id, 0U};
    }
    if (job.poll_period.count() <= 0) {
      return {SchedulerErrorCategory::invalid_poll_period, job.poll_job_id, 0U};
    }
    for (std::size_t earlier = 0U; earlier < index; ++earlier) {
      if (jobs_[earlier].job.poll_job_id == job.poll_job_id) {
        return {SchedulerErrorCategory::duplicate_poll_job_id, job.poll_job_id, 0U};
      }
    }
  }
  return {};
}

SchedulerError PollScheduler::validate_policy() const noexcept {
  if (!reliability_policy_.valid()) {
    return {SchedulerErrorCategory::invalid_policy_configuration, 0U, 0U};
  }
  if (policy_config_.consecutive_final_failures_to_offline == 0U ||
      policy_config_.offline_probe_interval.count() <= 0 ||
      policy_config_.consecutive_probe_successes_to_recover == 0U) {
    return {SchedulerErrorCategory::invalid_device_health_configuration, 0U, 0U};
  }
  return {};
}

std::optional<PollScheduler::Candidate>
PollScheduler::select_due_candidate(SchedulerTimePoint now) const noexcept {
  std::optional<Candidate> selected{};
  for (std::size_t index = 0U; index < jobs_.size(); ++index) {
    const auto &state = jobs_[index];
    const auto slave_id = state.job.request.slave_id;
    std::optional<Candidate> candidate{};

    if (state.active.has_value()) {
      if (state.active->request.state == RequestState::retry_wait &&
          state.active->request.attempt_eligible_at <= now) {
        candidate = Candidate{index, state.active->request.attempt_eligible_at, true, false};
      }
    } else if (!has_active_request_for_slave(slave_id)) {
      const auto &health = device_health_[slave_id];
      if (health.state == DeviceHealthState::online && state.next_due_at <= now) {
        candidate = Candidate{index, state.next_due_at, false, false};
      } else if (health.state != DeviceHealthState::online &&
                 recovery_job_id_[slave_id] == state.job.poll_job_id &&
                 health.next_probe_at.has_value() && *health.next_probe_at <= now) {
        candidate = Candidate{index, *health.next_probe_at, false, true};
      }
    }

    if (candidate.has_value() &&
        (!selected.has_value() || candidate_precedes(*candidate, *selected))) {
      selected = candidate;
    }
  }
  return selected;
}

bool PollScheduler::candidate_precedes(const Candidate &candidate,
                                       const Candidate &current) const noexcept {
  if (candidate.eligible_at != current.eligible_at) {
    return candidate.eligible_at < current.eligible_at;
  }
  const auto &candidate_state = jobs_[candidate.job_index];
  const auto &current_state = jobs_[current.job_index];
  if (candidate_state.job.priority != current_state.job.priority) {
    return static_cast<std::uint8_t>(candidate_state.job.priority) >
           static_cast<std::uint8_t>(current_state.job.priority);
  }
  const auto candidate_distance =
      cyclic_slave_distance(candidate_state.job.request.slave_id, last_dispatched_slave_id_);
  const auto current_distance =
      cyclic_slave_distance(current_state.job.request.slave_id, last_dispatched_slave_id_);
  if (candidate_distance != current_distance) {
    return candidate_distance < current_distance;
  }
  if (candidate_state.last_dispatched_at != current_state.last_dispatched_at) {
    if (!candidate_state.last_dispatched_at.has_value()) {
      return true;
    }
    if (!current_state.last_dispatched_at.has_value()) {
      return false;
    }
    return *candidate_state.last_dispatched_at < *current_state.last_dispatched_at;
  }
  return candidate_state.job.poll_job_id < current_state.job.poll_job_id;
}

bool PollScheduler::observation_times_valid(const RequestObservation &observation) const noexcept {
  if (!in_flight_job_index_.has_value()) {
    return false;
  }
  const auto &state = jobs_[*in_flight_job_index_];
  if (!state.active.has_value() || observation.completed_at < state.active->request.enqueued_at) {
    return false;
  }
  if (observation.sent_at.has_value() &&
      (*observation.sent_at < state.active->request.enqueued_at ||
       *observation.sent_at > observation.completed_at)) {
    return false;
  }
  if (observation.first_byte_at.has_value() &&
      (!observation.sent_at.has_value() || *observation.first_byte_at < *observation.sent_at ||
       *observation.first_byte_at > observation.completed_at)) {
    return false;
  }
  return true;
}

bool PollScheduler::has_active_request_for_slave(std::uint8_t slave_id) const noexcept {
  return std::any_of(jobs_.begin(), jobs_.end(), [slave_id](const JobState &state) {
    return state.job.request.slave_id == slave_id && state.active.has_value();
  });
}

std::optional<std::size_t>
PollScheduler::active_job_index(std::uint64_t request_id) const noexcept {
  for (std::size_t index = 0U; index < jobs_.size(); ++index) {
    const auto &active = jobs_[index].active;
    if (active.has_value() && active->request.request_id == request_id) {
      return index;
    }
  }
  return std::nullopt;
}

SchedulerTransition PollScheduler::process_attempt_result(const AttemptResult &result) noexcept {
  SchedulerTransition transition{};
  const auto index = active_job_index(result.request_id);
  if (!index.has_value()) {
    transition.error = {SchedulerErrorCategory::no_in_flight_request, result.poll_job_id,
                        result.request_id};
    return transition;
  }

  const auto job_index = *index;
  auto &job = jobs_[job_index];
  if (!job.active.has_value()) {
    transition.error = {SchedulerErrorCategory::no_in_flight_request, result.poll_job_id,
                        result.request_id};
    return transition;
  }
  auto &active = *job.active;
  const auto decision = reliability_policy_.evaluate(result.category, active.request.attempt,
                                                     result.observed_at, active.request.deadline);
  transition.occurred = true;
  transition.request_id = result.request_id;
  transition.poll_job_id = result.poll_job_id;
  transition.attempt = result.attempt;
  transition.previous_state = active.request.state;
  transition.next_state = decision.next_state;
  transition.result = decision.result;
  transition.remote_exception_code = result.remote_exception_code;
  transition.retry_at = decision.retry_at;
  in_flight_job_index_.reset();

  if (decision.disposition == ReliabilityDisposition::retry_wait) {
    active.request.attempt = decision.next_attempt;
    active.request.state = RequestState::retry_wait;
    active.request.attempt_eligible_at = decision.retry_at.value_or(active.request.deadline);
    active.sent_at.reset();
    active.attempt_deadline.reset();
    ++slave_statistics_[job.job.request.slave_id].retries_scheduled;
    return transition;
  }

  transition.terminal = true;
  if (decision.retry_exhausted) {
    ++slave_statistics_[job.job.request.slave_id].retry_exhausted;
  }
  finish_active_request(job_index,
                        AttemptResult{result.request_id, result.poll_job_id, result.attempt,
                                      decision.result, result.observed_at,
                                      result.remote_exception_code},
                        transition);
  return transition;
}

void PollScheduler::advance_period(JobState &state, SchedulerTimePoint now) noexcept {
  const auto due_at = state.next_due_at;
  const auto lateness = now - due_at;
  const auto period = std::chrono::duration_cast<SchedulerClock::duration>(state.job.poll_period);
  const auto coalesced_periods = lateness / period;
  state.next_due_at += period * (coalesced_periods + 1);
  state.last_dispatched_at = now;

  auto &statistics = slave_statistics_[state.job.request.slave_id];
  statistics.poll_periods_coalesced += static_cast<std::uint64_t>(coalesced_periods);
  const auto lateness_us = std::chrono::duration_cast<std::chrono::microseconds>(lateness);
  statistics.maximum_dispatch_lateness =
      std::max(statistics.maximum_dispatch_lateness, lateness_us);
}

void PollScheduler::finish_active_request(std::size_t job_index, const AttemptResult &result,
                                          SchedulerTransition &transition) noexcept {
  auto &state = jobs_[job_index];
  if (!state.active.has_value()) {
    transition.error = {SchedulerErrorCategory::no_in_flight_request, result.poll_job_id,
                        result.request_id};
    return;
  }
  auto &active = *state.active;
  auto &statistics = slave_statistics_[state.job.request.slave_id];
  ++statistics.requests_completed;
  if (result.category != RequestResultCategory::success) {
    ++statistics.requests_failed;
  }
  if (result.category == RequestResultCategory::deadline_exceeded) {
    ++statistics.deadline_exceeded;
  }
  statistics.last_completed_at = result.observed_at;
  statistics.last_timing = RequestTiming{active.request.due_at, active.request.enqueued_at,
                                         active.sent_at, std::nullopt, result.observed_at};
  active.request.state = transition.next_state;
  apply_terminal_health_result(job_index, active.request.is_recovery_probe, result, transition);
  state.active.reset();
}

void PollScheduler::apply_terminal_health_result(std::size_t job_index, bool is_recovery_probe,
                                                 const AttemptResult &result,
                                                 SchedulerTransition &transition) noexcept {
  const auto slave_id = jobs_[job_index].job.request.slave_id;
  auto &health = device_health_[slave_id];
  const auto previous_state = health.state;
  const bool communication_success = result.category == RequestResultCategory::success ||
                                     result.category == RequestResultCategory::remote_exception;

  if (is_recovery_probe) {
    health.state = DeviceHealthState::probing;
    health.next_probe_at = result.observed_at + policy_config_.offline_probe_interval;
    if (communication_success) {
      ++health.probe_successes;
      ++health.consecutive_probe_successes;
      if (health.consecutive_probe_successes >=
          policy_config_.consecutive_probe_successes_to_recover) {
        health.state = DeviceHealthState::online;
        health.consecutive_final_failures = 0U;
        health.next_probe_at.reset();
        ++health.recovery_transitions;
      }
    } else {
      health.consecutive_probe_successes = 0U;
    }
  } else if (communication_success) {
    health.consecutive_final_failures = 0U;
  } else if (is_final_communication_failure(result.category)) {
    ++health.consecutive_final_failures;
    if (health.consecutive_final_failures >= policy_config_.consecutive_final_failures_to_offline) {
      health.state = DeviceHealthState::offline;
      health.consecutive_probe_successes = 0U;
      health.next_probe_at = result.observed_at + policy_config_.offline_probe_interval;
      if (previous_state != DeviceHealthState::offline) {
        ++health.offline_transitions;
      }
    }
  }

  transition.device_state_changed = health.state != previous_state;
  transition.device_state = health.state;
}

void PollScheduler::initialize_devices() noexcept {
  for (const auto &state : jobs_) {
    const auto slave_id = state.job.request.slave_id;
    if (!recovery_job_id_[slave_id].has_value()) {
      recovery_job_id_[slave_id] = state.job.poll_job_id;
    }
  }
}

} // namespace industrial_iot_gateway::scheduler
