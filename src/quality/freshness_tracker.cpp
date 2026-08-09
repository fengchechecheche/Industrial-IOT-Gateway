#include "industrial_iot_gateway/quality/freshness_tracker.hpp"

#include <algorithm>
#include <utility>

namespace industrial_iot_gateway::quality {

FreshnessTracker::FreshnessTracker(const std::vector<RegisterFreshnessConfig> &configs) {
  registers_.reserve(configs.size());
  for (const auto &config : configs) {
    registers_.push_back(RegisterState{config});
  }
  validation_error_ = validate();
  valid_ = validation_error_.category == FreshnessErrorCategory::none;
}

bool FreshnessTracker::valid() const noexcept { return valid_; }

FreshnessError FreshnessTracker::validation_error() const noexcept { return validation_error_; }

std::size_t FreshnessTracker::register_count() const noexcept { return registers_.size(); }

RegisterQuality FreshnessTracker::quality(std::uint32_t poll_job_id) const noexcept {
  const auto index = find(poll_job_id);
  return index.has_value() ? effective_quality(registers_[*index])
                           : RegisterQuality::no_valid_sample;
}

std::optional<scheduler::SchedulerTimePoint>
FreshnessTracker::last_valid_time(std::uint32_t poll_job_id) const noexcept {
  const auto index = find(poll_job_id);
  return index.has_value() ? registers_[*index].last_valid
                           : std::optional<scheduler::SchedulerTimePoint>{};
}

QualityUpdateBatch
FreshnessTracker::record_valid_sample(std::uint32_t poll_job_id,
                                      scheduler::SchedulerTimePoint observed_at) {
  QualityUpdateBatch batch{};
  const auto index = find(poll_job_id);
  if (!index.has_value()) {
    batch.error = {FreshnessErrorCategory::unknown_poll_job, poll_job_id};
    return batch;
  }
  if (!accept_time(observed_at)) {
    batch.error = {FreshnessErrorCategory::time_regression, poll_job_id};
    return batch;
  }

  auto &state = registers_[*index];
  const auto previous = effective_quality(state);
  state.last_valid = observed_at;
  state.base_quality =
      device_offline_[state.config.slave_id] ? RegisterQuality::stale : RegisterQuality::fresh;
  append_transition(batch, state, previous, effective_quality(state), observed_at);
  return batch;
}

QualityUpdateBatch
FreshnessTracker::record_invalid_sample(std::uint32_t poll_job_id,
                                        scheduler::SchedulerTimePoint observed_at) {
  QualityUpdateBatch batch{};
  const auto index = find(poll_job_id);
  if (!index.has_value()) {
    batch.error = {FreshnessErrorCategory::unknown_poll_job, poll_job_id};
    return batch;
  }
  if (!accept_time(observed_at)) {
    batch.error = {FreshnessErrorCategory::time_regression, poll_job_id};
    return batch;
  }

  auto &state = registers_[*index];
  const auto previous = effective_quality(state);
  state.base_quality = RegisterQuality::invalid;
  append_transition(batch, state, previous, effective_quality(state), observed_at);
  return batch;
}

QualityUpdateBatch
FreshnessTracker::record_poll_failure(std::uint32_t poll_job_id,
                                      scheduler::SchedulerTimePoint observed_at) {
  QualityUpdateBatch batch{};
  const auto index = find(poll_job_id);
  if (!index.has_value()) {
    batch.error = {FreshnessErrorCategory::unknown_poll_job, poll_job_id};
    return batch;
  }
  if (!accept_time(observed_at)) {
    batch.error = {FreshnessErrorCategory::time_regression, poll_job_id};
    return batch;
  }

  auto &state = registers_[*index];
  const auto previous = effective_quality(state);
  state.base_quality =
      state.last_valid.has_value() ? RegisterQuality::stale : RegisterQuality::no_valid_sample;
  append_transition(batch, state, previous, effective_quality(state), observed_at);
  return batch;
}

QualityUpdateBatch FreshnessTracker::set_device_offline(std::uint8_t slave_id, bool offline,
                                                        scheduler::SchedulerTimePoint observed_at) {
  QualityUpdateBatch batch{};
  if (slave_id == 0U || slave_id > 247U) {
    batch.error = {FreshnessErrorCategory::invalid_slave_address};
    return batch;
  }
  if (!accept_time(observed_at)) {
    batch.error = {FreshnessErrorCategory::time_regression};
    return batch;
  }

  std::vector<std::pair<std::size_t, RegisterQuality>> affected{};
  for (std::size_t index = 0U; index < registers_.size(); ++index) {
    if (registers_[index].config.slave_id == slave_id) {
      affected.emplace_back(index, effective_quality(registers_[index]));
    }
  }

  device_offline_[slave_id] = offline;
  for (const auto &[index, previous] : affected) {
    auto &state = registers_[index];
    if (!offline && state.last_valid.has_value() && state.base_quality == RegisterQuality::fresh) {
      state.base_quality = RegisterQuality::stale;
    }
    append_transition(batch, state, previous, effective_quality(state), observed_at);
  }
  return batch;
}

QualityUpdateBatch FreshnessTracker::advance_time(scheduler::SchedulerTimePoint now) {
  QualityUpdateBatch batch{};
  if (!accept_time(now)) {
    batch.error = {FreshnessErrorCategory::time_regression};
    return batch;
  }

  for (auto &state : registers_) {
    const auto previous = effective_quality(state);
    if (state.base_quality == RegisterQuality::fresh && state.last_valid.has_value() &&
        now - *state.last_valid > state.config.freshness) {
      state.base_quality = RegisterQuality::stale;
    }
    append_transition(batch, state, previous, effective_quality(state), now);
  }
  return batch;
}

FreshnessError FreshnessTracker::validate() const noexcept {
  if (registers_.empty()) {
    return {FreshnessErrorCategory::empty_config};
  }
  for (std::size_t index = 0U; index < registers_.size(); ++index) {
    const auto &config = registers_[index].config;
    if (config.slave_id == 0U || config.slave_id > 247U) {
      return {FreshnessErrorCategory::invalid_slave_address, config.poll_job_id};
    }
    if (config.poll_period.count() <= 0) {
      return {FreshnessErrorCategory::invalid_poll_period, config.poll_job_id};
    }
    if (config.freshness.count() <= 0 || config.freshness < config.poll_period) {
      return {FreshnessErrorCategory::invalid_freshness, config.poll_job_id};
    }
    for (std::size_t other = index + 1U; other < registers_.size(); ++other) {
      if (config.poll_job_id == registers_[other].config.poll_job_id) {
        return {FreshnessErrorCategory::duplicate_poll_job_id, config.poll_job_id};
      }
    }
  }
  return {};
}

std::optional<std::size_t> FreshnessTracker::find(std::uint32_t poll_job_id) const noexcept {
  for (std::size_t index = 0U; index < registers_.size(); ++index) {
    if (registers_[index].config.poll_job_id == poll_job_id) {
      return index;
    }
  }
  return std::nullopt;
}

RegisterQuality FreshnessTracker::effective_quality(const RegisterState &state) const noexcept {
  return device_offline_[state.config.slave_id] ? RegisterQuality::offline : state.base_quality;
}

bool FreshnessTracker::accept_time(scheduler::SchedulerTimePoint now) noexcept {
  if (last_time_.has_value() && now < *last_time_) {
    return false;
  }
  last_time_ = now;
  return true;
}

void FreshnessTracker::append_transition(QualityUpdateBatch &batch, const RegisterState &state,
                                         RegisterQuality previous, RegisterQuality current,
                                         scheduler::SchedulerTimePoint now) const {
  if (previous == current) {
    return;
  }
  batch.transitions.push_back(
      QualityTransition{state.config.poll_job_id, state.config.slave_id, previous, current, now,
                        current == RegisterQuality::stale || current == RegisterQuality::offline});
}

} // namespace industrial_iot_gateway::quality
