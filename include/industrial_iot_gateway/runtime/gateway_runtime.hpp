#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <memory>

#include "industrial_iot_gateway/concurrency/bounded_queue.hpp"
#include "industrial_iot_gateway/config/runtime_config.hpp"
#include "industrial_iot_gateway/lifecycle/shutdown_coordinator.hpp"
#include "industrial_iot_gateway/scheduler/poll_scheduler.hpp"
#include "industrial_iot_gateway/transport/serial_port.hpp"

namespace industrial_iot_gateway::runtime {

enum class RuntimeErrorCategory {
  none,
  invalid_serial_configuration,
  invalid_register_configuration,
  invalid_scheduler_configuration,
  invalid_timing,
  already_started,
  thread_start_failed,
};

struct GatewayRuntimeConfig {
  transport::SerialConfig serial{};
  config::RuntimeConfiguration registers{};
  scheduler::SchedulerPolicyConfig scheduler_policy{};
  std::chrono::milliseconds response_timeout{500};
  std::chrono::milliseconds serial_reopen_backoff{200};
};

struct SlaveRuntimeStatistics {
  std::uint64_t attempts_sent{};
  std::uint64_t requests_succeeded{};
  std::uint64_t requests_failed{};
};

struct GatewayRuntimeStatistics {
  std::uint64_t requests_sent{};
  std::uint64_t requests_succeeded{};
  std::uint64_t requests_failed{};
  std::uint64_t response_timeouts{};
  std::uint64_t crc_errors{};
  std::uint64_t truncated_frames{};
  std::uint64_t remote_exceptions{};
  std::uint64_t serial_errors{};
  std::uint64_t serial_open_successes{};
  std::uint64_t serial_close_count{};
  std::uint64_t telemetry_events{};
  std::uint64_t quality_transitions{};
  std::uint64_t explicit_write_successes{};
  std::size_t in_flight_requests{};
  std::size_t maximum_in_flight_requests{};
  bool running{};
  bool stopped{};
  concurrency::QueueStatistics request_queue{};
  concurrency::QueueStatistics measurement_queue{};
  concurrency::QueueStatistics publish_queue{};
  std::array<SlaveRuntimeStatistics, 256U> slaves{};
};

class GatewayRuntime {
public:
  GatewayRuntime(GatewayRuntimeConfig config, std::ostream &event_output);
  ~GatewayRuntime();
  GatewayRuntime(const GatewayRuntime &) = delete;
  GatewayRuntime &operator=(const GatewayRuntime &) = delete;
  GatewayRuntime(GatewayRuntime &&) = delete;
  GatewayRuntime &operator=(GatewayRuntime &&) = delete;

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] RuntimeErrorCategory validation_error() const noexcept;
  [[nodiscard]] bool start();
  [[nodiscard]] concurrency::QueuePushStatus
  submit_write(std::uint8_t slave_id, std::uint16_t address, std::uint16_t value);
  void request_stop(lifecycle::ShutdownReason reason) noexcept;
  void join() noexcept;
  [[nodiscard]] GatewayRuntimeStatistics statistics() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace industrial_iot_gateway::runtime
