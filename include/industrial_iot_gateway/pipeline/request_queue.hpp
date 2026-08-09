#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "industrial_iot_gateway/concurrency/bounded_queue.hpp"
#include "industrial_iot_gateway/scheduler/scheduled_request.hpp"

namespace industrial_iot_gateway::pipeline {

struct RequestQueueConfig {
  concurrency::QueueConfig queue{256U, 205U};
  std::uint32_t max_consecutive_explicit_requests{8U};
};

class RequestQueue {
public:
  explicit RequestQueue(RequestQueueConfig config = {});
  ~RequestQueue();

  RequestQueue(const RequestQueue &) = delete;
  RequestQueue &operator=(const RequestQueue &) = delete;
  RequestQueue(RequestQueue &&) = delete;
  RequestQueue &operator=(RequestQueue &&) = delete;

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] concurrency::QueueErrorCategory validation_error() const noexcept;
  [[nodiscard]] concurrency::QueuePushStatus push(scheduler::ScheduledRequest request);
  [[nodiscard]] concurrency::QueuePopResult<scheduler::ScheduledRequest>
  try_pop(scheduler::SchedulerTimePoint now);
  void close();
  [[nodiscard]] bool closed() const;
  [[nodiscard]] concurrency::QueueStatistics statistics() const;
  [[nodiscard]] std::uint32_t consecutive_explicit_requests() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace industrial_iot_gateway::pipeline
