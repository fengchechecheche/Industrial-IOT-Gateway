#pragma once

#include <memory>

#include "industrial_iot_gateway/observability/structured_log.hpp"
#include "industrial_iot_gateway/publish/publish_sink.hpp"

namespace industrial_iot_gateway::publish {

class JsonlPublishSink final : public PublishSink {
public:
  explicit JsonlPublishSink(std::shared_ptr<observability::JsonlLogWriter> logger,
                            concurrency::QueueConfig queue_config = {1024U, 820U});
  ~JsonlPublishSink() override;
  JsonlPublishSink(const JsonlPublishSink &) = delete;
  JsonlPublishSink &operator=(const JsonlPublishSink &) = delete;
  JsonlPublishSink(JsonlPublishSink &&) = delete;
  JsonlPublishSink &operator=(JsonlPublishSink &&) = delete;

  [[nodiscard]] bool valid() const noexcept override;
  [[nodiscard]] bool start() override;
  [[nodiscard]] concurrency::QueuePushStatus submit(pipeline::PublishMessage message) override;
  void request_stop(std::chrono::steady_clock::time_point drain_deadline) noexcept override;
  void join() noexcept override;
  [[nodiscard]] PublishSinkStatistics statistics() const override;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace industrial_iot_gateway::publish
