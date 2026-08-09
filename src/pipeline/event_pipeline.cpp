#include "industrial_iot_gateway/pipeline/event_pipeline.hpp"

#include <tuple>
#include <utility>

namespace industrial_iot_gateway::pipeline {
namespace {

[[nodiscard]] bool same_measurement_key(const MeasurementRaw &left,
                                        const MeasurementRaw &right) noexcept {
  return std::tie(left.slave_id, left.function, left.start_address) ==
         std::tie(right.slave_id, right.function, right.start_address);
}

[[nodiscard]] bool telemetry_is_coalescible(const TelemetryEvent &event) noexcept {
  return event.quality == quality::RegisterQuality::fresh;
}

} // namespace

struct MeasurementQueue::Impl {
  explicit Impl(concurrency::QueueConfig config) : queue(config) {}
  concurrency::BoundedQueue<MeasurementMessage> queue;
};

MeasurementQueue::MeasurementQueue(concurrency::QueueConfig config)
    : impl_(std::make_unique<Impl>(config)) {}

MeasurementQueue::~MeasurementQueue() = default;

bool MeasurementQueue::valid() const noexcept { return impl_->queue.valid(); }

concurrency::QueuePushStatus MeasurementQueue::push(MeasurementMessage message) {
  const auto *incoming = std::get_if<MeasurementRaw>(&message);
  if (incoming == nullptr) {
    return impl_->queue.try_push(std::move(message));
  }
  const auto key = *incoming;
  return impl_->queue.try_push_or_replace(
      std::move(message), [&key](const MeasurementMessage &pending) {
        const auto *measurement = std::get_if<MeasurementRaw>(&pending);
        return measurement != nullptr && same_measurement_key(*measurement, key);
      });
}

concurrency::QueuePopResult<MeasurementMessage> MeasurementQueue::try_pop() {
  return impl_->queue.try_pop();
}

concurrency::QueuePopResult<MeasurementMessage>
MeasurementQueue::wait_pop_until(std::chrono::steady_clock::time_point deadline) {
  return impl_->queue.wait_pop_until(deadline);
}

void MeasurementQueue::close() { impl_->queue.close(); }

concurrency::QueueStatistics MeasurementQueue::statistics() const {
  return impl_->queue.statistics();
}

struct PublishQueue::Impl {
  explicit Impl(concurrency::QueueConfig config) : queue(config) {}
  concurrency::BoundedQueue<PublishMessage> queue;
};

PublishQueue::PublishQueue(concurrency::QueueConfig config)
    : impl_(std::make_unique<Impl>(config)) {}

PublishQueue::~PublishQueue() = default;

bool PublishQueue::valid() const noexcept { return impl_->queue.valid(); }

concurrency::QueuePushStatus PublishQueue::push(PublishMessage message) {
  const auto *incoming = std::get_if<TelemetryEvent>(&message);
  if (incoming == nullptr || !telemetry_is_coalescible(*incoming)) {
    return impl_->queue.try_push(std::move(message));
  }
  const auto topic = incoming->topic;
  return impl_->queue.try_push_or_replace(
      std::move(message), [&topic](const PublishMessage &pending) {
        const auto *telemetry = std::get_if<TelemetryEvent>(&pending);
        return telemetry != nullptr && telemetry_is_coalescible(*telemetry) &&
               telemetry->topic == topic;
      });
}

concurrency::QueuePopResult<PublishMessage> PublishQueue::try_pop() {
  return impl_->queue.try_pop();
}

concurrency::QueuePopResult<PublishMessage>
PublishQueue::wait_pop_until(std::chrono::steady_clock::time_point deadline) {
  return impl_->queue.wait_pop_until(deadline);
}

void PublishQueue::close() { impl_->queue.close(); }

concurrency::QueueStatistics PublishQueue::statistics() const { return impl_->queue.statistics(); }

} // namespace industrial_iot_gateway::pipeline
