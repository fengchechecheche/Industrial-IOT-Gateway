#pragma once

#include <memory>

#include "industrial_iot_gateway/mqtt/mqtt_config.hpp"
#include "industrial_iot_gateway/observability/structured_log.hpp"
#include "industrial_iot_gateway/publish/publish_sink.hpp"

namespace industrial_iot_gateway::mqtt {

class MqttPublishSink final : public publish::PublishSink {
public:
  MqttPublishSink(MqttConfig config, std::shared_ptr<observability::JsonlLogWriter> logger);
  ~MqttPublishSink() override;
  MqttPublishSink(const MqttPublishSink &) = delete;
  MqttPublishSink &operator=(const MqttPublishSink &) = delete;
  MqttPublishSink(MqttPublishSink &&) = delete;
  MqttPublishSink &operator=(MqttPublishSink &&) = delete;

  [[nodiscard]] bool valid() const noexcept override;
  [[nodiscard]] bool start() override;
  [[nodiscard]] concurrency::QueuePushStatus submit(pipeline::PublishMessage message) override;
  void request_stop(std::chrono::steady_clock::time_point drain_deadline) noexcept override;
  void join() noexcept override;
  [[nodiscard]] publish::PublishSinkStatistics statistics() const override;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace industrial_iot_gateway::mqtt
