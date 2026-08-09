#include "industrial_iot_gateway/publish/jsonl_publish_sink.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <utility>
#include <variant>

#include "industrial_iot_gateway/pipeline/event_pipeline.hpp"

namespace industrial_iot_gateway::publish {
namespace {

[[nodiscard]] std::string quality_name(quality::RegisterQuality quality) {
  switch (quality) {
  case quality::RegisterQuality::no_valid_sample:
    return "no_valid_sample";
  case quality::RegisterQuality::fresh:
    return "fresh";
  case quality::RegisterQuality::stale:
    return "stale";
  case quality::RegisterQuality::invalid:
    return "invalid";
  case quality::RegisterQuality::offline:
    return "offline";
  }
  return "unknown";
}

[[nodiscard]] observability::StructuredEvent to_log_event(const pipeline::PublishMessage &message) {
  observability::StructuredEvent event{};
  event.component = "publish_sink";
  if (const auto *telemetry = std::get_if<pipeline::TelemetryEvent>(&message)) {
    event.event = "telemetry";
    event.request_id = telemetry->request_id;
    event.slave_id = telemetry->slave_id == 0U ? std::optional<std::uint8_t>{}
                                               : std::optional<std::uint8_t>{telemetry->slave_id};
    event.result = telemetry->quality_reason;
    return event;
  }
  if (const auto *transition = std::get_if<pipeline::QualityTransitionEvent>(&message)) {
    event.event = "quality_transition";
    event.slave_id = transition->transition.slave_id;
    event.previous_state = quality_name(transition->transition.previous);
    event.next_state = quality_name(transition->transition.current);
    return event;
  }
  if (const auto *audit = std::get_if<pipeline::WriteAuditEvent>(&message)) {
    event.event = "write_audit";
    event.request_id = audit->request_id;
    event.slave_id = audit->slave_id;
    event.address = audit->address;
    return event;
  }
  if (const auto *status = std::get_if<pipeline::DeviceStatusEvent>(&message)) {
    event.event = "device_status";
    event.slave_id = status->slave_id;
    event.next_state = status->state;
    event.reason = status->reason;
    return event;
  }
  const auto &health = std::get<pipeline::GatewayHealthEvent>(message);
  event.event = "gateway_health";
  event.next_state = health.state;
  event.reason = health.reason;
  return event;
}

} // namespace

class JsonlPublishSink::Impl {
public:
  Impl(std::shared_ptr<observability::JsonlLogWriter> logger, concurrency::QueueConfig queue_config)
      : logger_(std::move(logger)), queue_(queue_config) {}

  ~Impl() {
    request_stop(std::chrono::steady_clock::now());
    join();
  }

  [[nodiscard]] bool valid() const noexcept { return logger_ != nullptr && queue_.valid(); }

  [[nodiscard]] bool start() {
    bool expected = false;
    if (!valid() || !started_.compare_exchange_strong(expected, true)) {
      return false;
    }
    try {
      worker_ = std::thread([this] { run(); });
      return true;
    } catch (...) {
      started_.store(false);
      return false;
    }
  }

  [[nodiscard]] concurrency::QueuePushStatus submit(pipeline::PublishMessage message) {
    if (stopping_.load()) {
      return concurrency::QueuePushStatus::closed;
    }
    const auto status = queue_.push(std::move(message));
    const std::lock_guard<std::mutex> lock(statistics_mutex_);
    if (status == concurrency::QueuePushStatus::coalesced) {
      ++statistics_.coalesced;
    } else if (status == concurrency::QueuePushStatus::full ||
               status == concurrency::QueuePushStatus::closed ||
               status == concurrency::QueuePushStatus::timed_out) {
      ++statistics_.dropped;
    }
    return status;
  }

  void request_stop(std::chrono::steady_clock::time_point) noexcept {
    stopping_.store(true);
    queue_.close();
  }

  void join() noexcept {
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  [[nodiscard]] PublishSinkStatistics statistics() const {
    const std::lock_guard<std::mutex> lock(statistics_mutex_);
    auto copy = statistics_;
    copy.queue = queue_.statistics();
    return copy;
  }

private:
  void run() noexcept {
    try {
      while (true) {
        auto message =
            queue_.wait_pop_until(std::chrono::steady_clock::now() + std::chrono::milliseconds{50});
        if (message.status == concurrency::QueuePopStatus::closed) {
          return;
        }
        if (!message.value.has_value()) {
          continue;
        }
        static_cast<void>(logger_->write(to_log_event(*message.value)));
        const std::lock_guard<std::mutex> lock(statistics_mutex_);
        ++statistics_.publish_attempts;
        ++statistics_.publish_successes;
      }
    } catch (...) {
      const std::lock_guard<std::mutex> lock(statistics_mutex_);
      ++statistics_.publish_failures;
    }
  }

  std::shared_ptr<observability::JsonlLogWriter> logger_{};
  pipeline::PublishQueue queue_;
  std::thread worker_{};
  std::atomic<bool> started_{};
  std::atomic<bool> stopping_{};
  mutable std::mutex statistics_mutex_{};
  PublishSinkStatistics statistics_{};
};

JsonlPublishSink::JsonlPublishSink(std::shared_ptr<observability::JsonlLogWriter> logger,
                                   concurrency::QueueConfig queue_config)
    : impl_(std::make_unique<Impl>(std::move(logger), queue_config)) {}

JsonlPublishSink::~JsonlPublishSink() = default;

bool JsonlPublishSink::valid() const noexcept { return impl_->valid(); }

bool JsonlPublishSink::start() { return impl_->start(); }

concurrency::QueuePushStatus JsonlPublishSink::submit(pipeline::PublishMessage message) {
  return impl_->submit(std::move(message));
}

void JsonlPublishSink::request_stop(std::chrono::steady_clock::time_point drain_deadline) noexcept {
  impl_->request_stop(drain_deadline);
}

void JsonlPublishSink::join() noexcept { impl_->join(); }

PublishSinkStatistics JsonlPublishSink::statistics() const { return impl_->statistics(); }

} // namespace industrial_iot_gateway::publish
