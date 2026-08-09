#pragma once

#include <chrono>
#include <cstdint>

#include "industrial_iot_gateway/concurrency/bounded_queue.hpp"
#include "industrial_iot_gateway/pipeline/pipeline_messages.hpp"

namespace industrial_iot_gateway::publish {

struct PublishSinkStatistics {
  bool connected{};
  std::uint64_t connect_attempts{};
  std::uint64_t connected_events{};
  std::uint64_t disconnected_events{};
  std::uint64_t reconnect_scheduled{};
  std::uint64_t publish_attempts{};
  std::uint64_t publish_successes{};
  std::uint64_t publish_failures{};
  std::uint64_t coalesced{};
  std::uint64_t dropped{};
  std::uint64_t expired_fresh_dropped{};
  std::uint64_t critical_enqueue_failures{};
  std::uint64_t drain_expired{};
  std::uint64_t unconfirmed_on_close{};
  concurrency::QueueStatistics queue{};
};

class PublishSink {
public:
  virtual ~PublishSink() = default;
  [[nodiscard]] virtual bool valid() const noexcept = 0;
  [[nodiscard]] virtual bool start() = 0;
  [[nodiscard]] virtual concurrency::QueuePushStatus submit(pipeline::PublishMessage message) = 0;
  virtual void request_stop(std::chrono::steady_clock::time_point drain_deadline) noexcept = 0;
  virtual void join() noexcept = 0;
  [[nodiscard]] virtual PublishSinkStatistics statistics() const = 0;
};

} // namespace industrial_iot_gateway::publish
