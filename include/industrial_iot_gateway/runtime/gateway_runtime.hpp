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
#include "industrial_iot_gateway/observability/structured_log.hpp"
#include "industrial_iot_gateway/pipeline/event_pipeline.hpp"
#include "industrial_iot_gateway/pipeline/request_queue.hpp"
#include "industrial_iot_gateway/publish/publish_sink.hpp"
#include "industrial_iot_gateway/scheduler/poll_scheduler.hpp"
#include "industrial_iot_gateway/transport/serial_port.hpp"

namespace industrial_iot_gateway::runtime {

enum class RuntimeErrorCategory {
  none,
  invalid_serial_configuration,
  invalid_register_configuration,
  invalid_scheduler_configuration,
  invalid_publish_sink,
  invalid_timing,
  invalid_request_queue_configuration,
  invalid_measurement_queue_configuration,
  already_started,
  thread_start_failed,
};

struct GatewayRuntimeConfig {
  transport::SerialConfig serial{};
  config::RuntimeConfiguration registers{};
  scheduler::SchedulerPolicyConfig scheduler_policy{};
  pipeline::RequestQueueConfig request_queue{};
  concurrency::QueueConfig measurement_queue{512U, 410U};
  std::chrono::milliseconds response_timeout{500};
  std::chrono::milliseconds late_response_guard{200};
  std::chrono::milliseconds minimum_request_interval{0};
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
  std::uint64_t late_response_quarantines{};
  std::uint64_t late_response_bytes_discarded{};
  std::uint64_t crc_errors{};
  std::uint64_t truncated_frames{};
  std::uint64_t remote_exceptions{};
  std::uint64_t serial_errors{};
  std::uint64_t serial_open_successes{};
  std::uint64_t serial_close_count{};
  std::uint64_t telemetry_events{};
  std::uint64_t quality_transitions{};
  std::uint64_t explicit_write_successes{};
  std::uint64_t measurement_enqueue_failures{};
  std::uint64_t scheduler_feedback_delivery_failures{};
  std::uint64_t scheduler_transition_errors{};
  std::uint64_t scheduler_deadline_recoveries{};
  std::size_t in_flight_requests{};
  std::size_t maximum_in_flight_requests{};
  bool running{};
  bool stopped{};
  concurrency::QueueStatistics request_queue{};
  concurrency::QueueStatistics measurement_queue{};
  concurrency::QueueStatistics scheduler_feedback_queue{};
  concurrency::QueueStatistics publish_queue{};
  publish::PublishSinkStatistics publisher{};
  std::array<SlaveRuntimeStatistics, 256U> slaves{};
};

class GatewayRuntime {
public:
  GatewayRuntime(GatewayRuntimeConfig config, std::ostream &event_output);
  GatewayRuntime(GatewayRuntimeConfig config, std::shared_ptr<observability::JsonlLogWriter> logger,
                 std::unique_ptr<publish::PublishSink> publish_sink);
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
