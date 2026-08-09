#include "industrial_iot_gateway/lifecycle/shutdown_coordinator.hpp"

#include <cerrno>
#include <csignal>
#include <pthread.h>

namespace industrial_iot_gateway::lifecycle {
namespace {

[[nodiscard]] ShutdownStage next_stage(ShutdownStage current) noexcept {
  return static_cast<ShutdownStage>(static_cast<int>(current) + 1);
}

[[nodiscard]] bool valid_time(const std::optional<LifecycleTimePoint> &previous,
                              LifecycleTimePoint now) noexcept {
  return !previous.has_value() || now >= *previous;
}

[[nodiscard]] sigset_t shutdown_signal_set() noexcept {
  sigset_t signals{};
  static_cast<void>(::sigemptyset(&signals));
  static_cast<void>(::sigaddset(&signals, SIGINT));
  static_cast<void>(::sigaddset(&signals, SIGTERM));
  return signals;
}

} // namespace

ShutdownCoordinator::ShutdownCoordinator(ShutdownConfig config) : config_(config) {
  if (config_.graceful_timeout.count() <= 0) {
    validation_error_ = ShutdownErrorCategory::invalid_graceful_timeout;
  } else if (config_.mqtt_drain_timeout.count() <= 0) {
    validation_error_ = ShutdownErrorCategory::invalid_mqtt_drain_timeout;
  } else if (config_.mqtt_drain_timeout > config_.graceful_timeout) {
    validation_error_ = ShutdownErrorCategory::mqtt_drain_exceeds_graceful_timeout;
  }
}

bool ShutdownCoordinator::valid() const noexcept {
  return validation_error_ == ShutdownErrorCategory::none;
}

ShutdownErrorCategory ShutdownCoordinator::validation_error() const noexcept {
  return validation_error_;
}

bool ShutdownCoordinator::request_stop(ShutdownReason reason, LifecycleTimePoint now) noexcept {
  if (!valid() || !valid_time(last_time_, now)) {
    return false;
  }
  last_time_ = now;
  ++snapshot_.stop_requests;
  if (snapshot_.stage != ShutdownStage::running) {
    ++snapshot_.repeated_stop_requests;
    return false;
  }
  snapshot_.reason = reason;
  snapshot_.stage = ShutdownStage::stop_accepting;
  snapshot_.started_at = now;
  return true;
}

ShutdownErrorCategory ShutdownCoordinator::advance_to(ShutdownStage stage,
                                                      LifecycleTimePoint now) noexcept {
  if (!valid_time(last_time_, now)) {
    return ShutdownErrorCategory::time_regression;
  }
  if (snapshot_.stage == ShutdownStage::running || snapshot_.stage == ShutdownStage::stopped ||
      stage != next_stage(snapshot_.stage)) {
    return ShutdownErrorCategory::invalid_stage_transition;
  }
  last_time_ = now;
  snapshot_.stage = stage;
  if (stage == ShutdownStage::stopped) {
    snapshot_.stopped_at = now;
  }
  return ShutdownErrorCategory::none;
}

bool ShutdownCoordinator::deadline_exceeded(LifecycleTimePoint now) noexcept {
  if (!snapshot_.started_at.has_value() || snapshot_.stage == ShutdownStage::stopped ||
      !valid_time(last_time_, now)) {
    return snapshot_.timed_out;
  }
  last_time_ = now;
  if (now - *snapshot_.started_at >= config_.graceful_timeout) {
    snapshot_.timed_out = true;
  }
  return snapshot_.timed_out;
}

ShutdownSnapshot ShutdownCoordinator::snapshot() const noexcept { return snapshot_; }

bool SynchronousSignalWaiter::block_shutdown_signals_for_current_thread() noexcept {
  auto signals = shutdown_signal_set();
  return ::pthread_sigmask(SIG_BLOCK, &signals, nullptr) == 0;
}

ShutdownReason SynchronousSignalWaiter::wait() noexcept {
  auto signals = shutdown_signal_set();
  int received = 0;
  const int result = ::sigwait(&signals, &received);
  if (result != 0) {
    return ShutdownReason::fatal_component_error;
  }
  if (received == SIGINT) {
    return ShutdownReason::sigint;
  }
  if (received == SIGTERM) {
    return ShutdownReason::sigterm;
  }
  return ShutdownReason::fatal_component_error;
}

} // namespace industrial_iot_gateway::lifecycle
