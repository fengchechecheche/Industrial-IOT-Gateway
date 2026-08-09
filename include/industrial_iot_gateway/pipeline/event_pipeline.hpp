#pragma once

#include <chrono>
#include <memory>

#include "industrial_iot_gateway/concurrency/bounded_queue.hpp"
#include "industrial_iot_gateway/pipeline/pipeline_messages.hpp"

namespace industrial_iot_gateway::pipeline {

class MeasurementQueue {
public:
  explicit MeasurementQueue(concurrency::QueueConfig config = {512U, 410U});
  ~MeasurementQueue();
  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] concurrency::QueuePushStatus push(MeasurementMessage message);
  [[nodiscard]] concurrency::QueuePopResult<MeasurementMessage> try_pop();
  [[nodiscard]] concurrency::QueuePopResult<MeasurementMessage>
  wait_pop_until(std::chrono::steady_clock::time_point deadline);
  void close();
  [[nodiscard]] concurrency::QueueStatistics statistics() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class PublishQueue {
public:
  explicit PublishQueue(concurrency::QueueConfig config = {1024U, 820U});
  ~PublishQueue();
  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] concurrency::QueuePushStatus push(PublishMessage message);
  [[nodiscard]] concurrency::QueuePopResult<PublishMessage> try_pop();
  [[nodiscard]] concurrency::QueuePopResult<PublishMessage>
  wait_pop_until(std::chrono::steady_clock::time_point deadline);
  void close();
  [[nodiscard]] concurrency::QueueStatistics statistics() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace industrial_iot_gateway::pipeline
