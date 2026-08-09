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

[[nodiscard]] bool is_supported_poll_function(const protocol::FunctionCode function) noexcept {
  return function == protocol::FunctionCode::read_holding_registers ||
         function == protocol::FunctionCode::read_input_registers;
}

[[nodiscard]] std::uint16_t
cyclic_slave_distance(const std::uint8_t slave_id,
                      const std::optional<std::uint8_t> last_slave_id) noexcept {
  if (!last_slave_id.has_value()) {
    return slave_id;
  }

  constexpr std::uint16_t slave_count = 247U;
  const auto current = static_cast<std::uint16_t>(slave_id);
  const auto last = static_cast<std::uint16_t>(*last_slave_id);
  const auto raw_distance =
      static_cast<std::uint16_t>((current + slave_count - last) % slave_count);
  return raw_distance == 0U ? slave_count : raw_distance;
}

} // namespace

PollScheduler::PollScheduler(const std::vector<PollJob> &jobs,
                             const SchedulerTimePoint start_time) {
  jobs_.reserve(jobs.size());
  for (const auto &job : jobs) {
    jobs_.push_back(JobState{job, start_time, std::nullopt});
  }

  validation_error_ = validate_jobs();
  valid_ = validation_error_.category == SchedulerErrorCategory::none;
}

bool PollScheduler::valid() const noexcept { return valid_; }

SchedulerError PollScheduler::validation_error() const noexcept { return validation_error_; }

std::size_t PollScheduler::poll_job_count() const noexcept { return jobs_.size(); }

std::optional<ScheduledRequest>
PollScheduler::dispatch_next(const SchedulerTimePoint now) noexcept {
  if (!valid_ || in_flight_.has_value()) {
    return std::nullopt;
  }
  if (next_request_id_ == std::numeric_limits<std::uint64_t>::max()) {
    validation_error_ =
        SchedulerError{SchedulerErrorCategory::request_id_exhausted, 0U, next_request_id_};
    valid_ = false;
    return std::nullopt;
  }

  const auto selected_index = select_due_job(now);
  if (!selected_index.has_value()) {
    return std::nullopt;
  }

  auto &state = jobs_[*selected_index];
  const auto due_at = state.next_due_at;
  const auto lateness = now - due_at;
  const auto period = std::chrono::duration_cast<SchedulerClock::duration>(state.job.poll_period);
  const auto coalesced_periods = lateness / period;
  state.next_due_at += period * (coalesced_periods + 1);
  state.last_dispatched_at = now;

  ScheduledRequest request{};
  request.request_id = next_request_id_++;
  request.poll_job_id = state.job.poll_job_id;
  request.request_kind = RequestKind::poll_read;
  request.request = state.job.request;
  request.due_at = due_at;
  request.created_at = now;
  request.enqueued_at = now;
  request.attempt = 1U;
  request.source = RequestSource::poll_scheduler;

  const auto slave_id = state.job.request.slave_id;
  auto &statistics = slave_statistics_[slave_id];
  ++statistics.requests_dispatched;
  statistics.poll_periods_coalesced += static_cast<std::uint64_t>(coalesced_periods);
  statistics.last_dispatched_at = now;
  const auto lateness_us = std::chrono::duration_cast<std::chrono::microseconds>(lateness);
  statistics.maximum_dispatch_lateness =
      std::max(statistics.maximum_dispatch_lateness, lateness_us);

  last_dispatched_slave_id_ = slave_id;
  in_flight_ = request;
  return request;
}

SchedulerError PollScheduler::complete_request(const RequestObservation &observation) noexcept {
  if (!in_flight_.has_value()) {
    return SchedulerError{SchedulerErrorCategory::no_in_flight_request, observation.poll_job_id,
                          observation.request_id};
  }
  if (observation.request_id != in_flight_->request_id) {
    return SchedulerError{SchedulerErrorCategory::request_id_mismatch, observation.poll_job_id,
                          observation.request_id};
  }
  if (observation.poll_job_id != in_flight_->poll_job_id) {
    return SchedulerError{SchedulerErrorCategory::poll_job_id_mismatch, observation.poll_job_id,
                          observation.request_id};
  }
  if (!observation_times_valid(observation)) {
    return SchedulerError{SchedulerErrorCategory::invalid_observation_times,
                          observation.poll_job_id, observation.request_id};
  }

  const auto *request = std::get_if<protocol::ReadRequest>(&in_flight_->request);
  if (request == nullptr) {
    return SchedulerError{SchedulerErrorCategory::unsupported_poll_function,
                          observation.poll_job_id, observation.request_id};
  }
  auto &statistics = slave_statistics_[request->slave_id];
  ++statistics.requests_completed;
  if (!observation.succeeded) {
    ++statistics.requests_failed;
  }
  statistics.last_completed_at = observation.completed_at;
  statistics.last_timing =
      RequestTiming{in_flight_->due_at, in_flight_->enqueued_at, observation.sent_at,
                    observation.first_byte_at, observation.completed_at};
  in_flight_.reset();
  return SchedulerError{};
}

bool PollScheduler::has_in_flight_request() const noexcept { return in_flight_.has_value(); }

std::optional<SchedulerTimePoint> PollScheduler::next_due_time() const noexcept {
  if (!valid_ || in_flight_.has_value() || jobs_.empty()) {
    return std::nullopt;
  }

  auto next_due = jobs_.front().next_due_at;
  for (const auto &state : jobs_) {
    next_due = std::min(next_due, state.next_due_at);
  }
  return next_due;
}

const SlavePollStatistics &
PollScheduler::slave_statistics(const std::uint8_t slave_id) const noexcept {
  return slave_statistics_[slave_id];
}

SchedulerError PollScheduler::validate_jobs() const noexcept {
  if (jobs_.empty()) {
    return SchedulerError{SchedulerErrorCategory::empty_poll_plan, 0U, 0U};
  }

  for (std::size_t index = 0U; index < jobs_.size(); ++index) {
    const auto &job = jobs_[index].job;
    if (job.request.slave_id < 1U || job.request.slave_id > 247U) {
      return SchedulerError{SchedulerErrorCategory::invalid_slave_address, job.poll_job_id, 0U};
    }
    if (!is_supported_poll_function(job.request.function)) {
      return SchedulerError{SchedulerErrorCategory::unsupported_poll_function, job.poll_job_id, 0U};
    }
    if (job.request.quantity < 1U ||
        static_cast<std::size_t>(job.request.quantity) > protocol::kMaxReadRegisterQuantity) {
      return SchedulerError{SchedulerErrorCategory::invalid_poll_quantity, job.poll_job_id, 0U};
    }
    const auto last_address = static_cast<std::uint32_t>(job.request.start_address) +
                              static_cast<std::uint32_t>(job.request.quantity) - 1U;
    if (last_address > std::numeric_limits<std::uint16_t>::max()) {
      return SchedulerError{SchedulerErrorCategory::address_range_overflow, job.poll_job_id, 0U};
    }
    if (job.poll_period.count() <= 0) {
      return SchedulerError{SchedulerErrorCategory::invalid_poll_period, job.poll_job_id, 0U};
    }

    for (std::size_t earlier = 0U; earlier < index; ++earlier) {
      if (jobs_[earlier].job.poll_job_id == job.poll_job_id) {
        return SchedulerError{SchedulerErrorCategory::duplicate_poll_job_id, job.poll_job_id, 0U};
      }
    }
  }

  return SchedulerError{};
}

std::optional<std::size_t>
PollScheduler::select_due_job(const SchedulerTimePoint now) const noexcept {
  std::optional<std::size_t> selected{};
  for (std::size_t index = 0U; index < jobs_.size(); ++index) {
    if (jobs_[index].next_due_at > now) {
      continue;
    }
    if (!selected.has_value() || candidate_precedes(index, *selected)) {
      selected = index;
    }
  }
  return selected;
}

bool PollScheduler::candidate_precedes(const std::size_t candidate_index,
                                       const std::size_t current_index) const noexcept {
  const auto &candidate = jobs_[candidate_index];
  const auto &current = jobs_[current_index];
  if (candidate.next_due_at != current.next_due_at) {
    return candidate.next_due_at < current.next_due_at;
  }
  if (candidate.job.priority != current.job.priority) {
    return static_cast<std::uint8_t>(candidate.job.priority) >
           static_cast<std::uint8_t>(current.job.priority);
  }

  const auto candidate_distance =
      cyclic_slave_distance(candidate.job.request.slave_id, last_dispatched_slave_id_);
  const auto current_distance =
      cyclic_slave_distance(current.job.request.slave_id, last_dispatched_slave_id_);
  if (candidate_distance != current_distance) {
    return candidate_distance < current_distance;
  }

  if (candidate.last_dispatched_at != current.last_dispatched_at) {
    if (!candidate.last_dispatched_at.has_value()) {
      return true;
    }
    if (!current.last_dispatched_at.has_value()) {
      return false;
    }
    return *candidate.last_dispatched_at < *current.last_dispatched_at;
  }
  return candidate.job.poll_job_id < current.job.poll_job_id;
}

bool PollScheduler::observation_times_valid(const RequestObservation &observation) const noexcept {
  if (!in_flight_.has_value() || observation.completed_at < in_flight_->enqueued_at) {
    return false;
  }
  if (observation.sent_at.has_value()) {
    if (*observation.sent_at < in_flight_->enqueued_at ||
        *observation.sent_at > observation.completed_at) {
      return false;
    }
  }
  if (observation.first_byte_at.has_value()) {
    if (!observation.sent_at.has_value() || *observation.first_byte_at < *observation.sent_at ||
        *observation.first_byte_at > observation.completed_at) {
      return false;
    }
  }
  return true;
}

} // namespace industrial_iot_gateway::scheduler
