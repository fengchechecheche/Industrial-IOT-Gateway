#include "industrial_iot_gateway/scheduler/reliability_policy.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace industrial_iot_gateway::scheduler {
namespace {

[[nodiscard]] bool is_positive(std::chrono::milliseconds value) noexcept {
  return value.count() > 0;
}

} // namespace

ReliabilityPolicy::ReliabilityPolicy(ReliabilityConfig config) : config_(config) {
  validation_error_ = validate();
  valid_ = validation_error_.category == ReliabilityConfigErrorCategory::none;
}

bool ReliabilityPolicy::valid() const noexcept { return valid_; }

ReliabilityConfigError ReliabilityPolicy::validation_error() const noexcept {
  return validation_error_;
}

const ReliabilityConfig &ReliabilityPolicy::config() const noexcept { return config_; }

SchedulerTimePoint
ReliabilityPolicy::request_deadline(SchedulerTimePoint enqueued_at) const noexcept {
  return enqueued_at + config_.request_deadline;
}

SchedulerTimePoint
ReliabilityPolicy::attempt_deadline(SchedulerTimePoint sent_at,
                                    SchedulerTimePoint request_deadline_at) const noexcept {
  return std::min(sent_at + config_.response_timeout, request_deadline_at);
}

bool ReliabilityPolicy::is_retryable(RequestResultCategory category) const noexcept {
  switch (category) {
  case RequestResultCategory::response_timeout:
  case RequestResultCategory::crc_mismatch:
  case RequestResultCategory::truncated_frame:
  case RequestResultCategory::serial_io_transient:
    return true;
  case RequestResultCategory::success:
  case RequestResultCategory::remote_exception:
  case RequestResultCategory::invalid_configuration:
  case RequestResultCategory::broadcast_unsupported:
  case RequestResultCategory::deadline_exceeded:
  case RequestResultCategory::shutdown_cancelled:
    return false;
  }
  return false;
}

std::chrono::milliseconds
ReliabilityPolicy::backoff_after_failed_attempt(std::uint32_t failed_attempt) const noexcept {
  long double delay = static_cast<long double>(config_.backoff_initial.count());
  const long double maximum = static_cast<long double>(config_.backoff_max.count());

  for (std::uint32_t index = 1U; index < failed_attempt && delay < maximum; ++index) {
    delay *= config_.backoff_multiplier;
    delay = std::min(delay, maximum);
  }

  return std::chrono::milliseconds{
      static_cast<std::chrono::milliseconds::rep>(std::min(delay, maximum))};
}

ReliabilityDecision
ReliabilityPolicy::evaluate(RequestResultCategory category, std::uint32_t current_attempt,
                            SchedulerTimePoint observed_at,
                            SchedulerTimePoint request_deadline_at) const noexcept {
  ReliabilityDecision decision{};
  decision.result = category;

  if (!valid_) {
    decision.disposition = ReliabilityDisposition::failed;
    decision.result = RequestResultCategory::invalid_configuration;
    return decision;
  }

  if (observed_at > request_deadline_at) {
    decision.disposition = ReliabilityDisposition::failed;
    decision.next_state = RequestState::failed;
    decision.result = RequestResultCategory::deadline_exceeded;
    return decision;
  }

  if (category == RequestResultCategory::success ||
      category == RequestResultCategory::remote_exception) {
    decision.disposition = ReliabilityDisposition::completed;
    decision.next_state = RequestState::completed;
    return decision;
  }

  if (category == RequestResultCategory::shutdown_cancelled) {
    decision.disposition = ReliabilityDisposition::cancelled;
    decision.next_state = RequestState::cancelled;
    return decision;
  }

  if (!is_retryable(category)) {
    decision.disposition = ReliabilityDisposition::failed;
    decision.next_state = RequestState::failed;
    return decision;
  }

  if (current_attempt >= config_.max_attempts) {
    decision.disposition = ReliabilityDisposition::failed;
    decision.next_state = RequestState::failed;
    decision.retry_exhausted = true;
    return decision;
  }

  const auto retry_at = observed_at + backoff_after_failed_attempt(current_attempt);
  if (retry_at >= request_deadline_at) {
    decision.disposition = ReliabilityDisposition::failed;
    decision.next_state = RequestState::failed;
    decision.result = RequestResultCategory::deadline_exceeded;
    return decision;
  }

  decision.disposition = ReliabilityDisposition::retry_wait;
  decision.next_state = RequestState::retry_wait;
  decision.next_attempt = current_attempt + 1U;
  decision.retry_at = retry_at;
  return decision;
}

ReliabilityConfigError ReliabilityPolicy::validate() const noexcept {
  if (!is_positive(config_.response_timeout)) {
    return {ReliabilityConfigErrorCategory::invalid_response_timeout};
  }
  if (config_.max_attempts == 0U || config_.max_attempts > 5U) {
    return {ReliabilityConfigErrorCategory::invalid_max_attempts};
  }
  if (!is_positive(config_.backoff_initial) || !is_positive(config_.backoff_max) ||
      config_.backoff_initial > config_.backoff_max) {
    return {ReliabilityConfigErrorCategory::invalid_backoff};
  }
  if (!std::isfinite(config_.backoff_multiplier) || config_.backoff_multiplier < 1.0) {
    return {ReliabilityConfigErrorCategory::invalid_multiplier};
  }
  if (config_.request_deadline <= config_.response_timeout) {
    return {ReliabilityConfigErrorCategory::invalid_deadline};
  }
  if (worst_case_budget() > config_.request_deadline) {
    return {ReliabilityConfigErrorCategory::insufficient_deadline_budget};
  }
  return {};
}

std::chrono::milliseconds ReliabilityPolicy::worst_case_budget() const noexcept {
  using Rep = std::chrono::milliseconds::rep;
  const auto maximum = std::numeric_limits<Rep>::max();
  Rep total = 0;

  const auto add_saturated = [&total](Rep value) {
    total = value > maximum - total ? maximum : total + value;
  };

  for (std::uint32_t attempt = 1U; attempt <= config_.max_attempts; ++attempt) {
    add_saturated(config_.response_timeout.count());
    if (attempt < config_.max_attempts) {
      add_saturated(backoff_after_failed_attempt(attempt).count());
    }
  }
  return std::chrono::milliseconds{total};
}

} // namespace industrial_iot_gateway::scheduler
