#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace industrial_iot_gateway::lifecycle {

using LifecycleClock = std::chrono::steady_clock;
using LifecycleTimePoint = LifecycleClock::time_point;

enum class ShutdownReason {
  none,
  sigint,
  sigterm,
  service_stop,
  fatal_component_error,
};

enum class ShutdownStage {
  running,
  stop_accepting,
  cancel_queued_and_retry_wait,
  wake_serial_io,
  stop_polling,
  drain_or_expire_measurements,
  drain_mqtt_with_limit,
  close_mqtt,
  serial_io_owner_closes_fd,
  join_all_threads,
  stopped,
};

struct ShutdownConfig {
  std::chrono::milliseconds graceful_timeout{5000};
  std::chrono::milliseconds mqtt_drain_timeout{2000};
};

enum class ShutdownErrorCategory {
  none,
  invalid_graceful_timeout,
  invalid_mqtt_drain_timeout,
  mqtt_drain_exceeds_graceful_timeout,
  invalid_stage_transition,
  time_regression,
};

struct ShutdownSnapshot {
  ShutdownStage stage{ShutdownStage::running};
  ShutdownReason reason{ShutdownReason::none};
  std::uint64_t stop_requests{};
  std::uint64_t repeated_stop_requests{};
  bool timed_out{};
  std::optional<LifecycleTimePoint> started_at{};
  std::optional<LifecycleTimePoint> stopped_at{};
};

class ShutdownCoordinator {
public:
  explicit ShutdownCoordinator(ShutdownConfig config = {});
  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] ShutdownErrorCategory validation_error() const noexcept;
  [[nodiscard]] bool request_stop(ShutdownReason reason, LifecycleTimePoint now) noexcept;
  [[nodiscard]] ShutdownErrorCategory advance_to(ShutdownStage stage,
                                                 LifecycleTimePoint now) noexcept;
  [[nodiscard]] bool deadline_exceeded(LifecycleTimePoint now) noexcept;
  [[nodiscard]] ShutdownSnapshot snapshot() const noexcept;

private:
  ShutdownConfig config_{};
  ShutdownErrorCategory validation_error_{ShutdownErrorCategory::none};
  ShutdownSnapshot snapshot_{};
  std::optional<LifecycleTimePoint> last_time_{};
};

class SynchronousSignalWaiter {
public:
  [[nodiscard]] static bool block_shutdown_signals_for_current_thread() noexcept;
  [[nodiscard]] static ShutdownReason wait() noexcept;
};

} // namespace industrial_iot_gateway::lifecycle
