#include "industrial_iot_gateway/mqtt/mqtt_publish_sink.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <sstream>
#include <string>

namespace {

using industrial_iot_gateway::concurrency::QueuePushStatus;
using industrial_iot_gateway::mqtt::MqttConfig;
using industrial_iot_gateway::mqtt::MqttPublishSink;
using industrial_iot_gateway::observability::JsonlLogWriter;
using industrial_iot_gateway::pipeline::DeviceStatusEvent;
using industrial_iot_gateway::pipeline::PublishMessage;
using industrial_iot_gateway::pipeline::TelemetryEvent;
using industrial_iot_gateway::quality::RegisterQuality;

[[nodiscard]] TelemetryEvent telemetry(std::uint64_t id, std::string topic) {
  TelemetryEvent event{};
  event.event_id = id;
  event.topic = std::move(topic);
  event.quality = RegisterQuality::fresh;
  event.quality_reason = "valid_sample";
  event.value = static_cast<double>(id);
  return event;
}

[[nodiscard]] MqttConfig config() {
  MqttConfig value{};
  value.run_id = "8b2144dc-3701-4b80-958f-2bcf918cf307";
  return value;
}

TEST(MqttPublishSinkTest, ValidatesFrozenClientIdentifier) {
  std::ostringstream output{};
  auto logger = std::make_shared<JsonlLogWriter>(output);
  MqttPublishSink valid(config(), logger);
  EXPECT_TRUE(valid.valid());

  auto bad = config();
  bad.client_id = "client-id-with-dashes";
  MqttPublishSink invalid(std::move(bad), logger);
  EXPECT_FALSE(invalid.valid());
}

TEST(MqttPublishSinkTest, CoalescesFreshTelemetryBeforeNetworkThreadStarts) {
  std::ostringstream output{};
  auto logger = std::make_shared<JsonlLogWriter>(output);
  MqttPublishSink sink(config(), logger);
  ASSERT_EQ(sink.submit(PublishMessage{telemetry(1U, "topic/temperature")}),
            QueuePushStatus::accepted);
  EXPECT_EQ(sink.submit(PublishMessage{telemetry(2U, "topic/temperature")}),
            QueuePushStatus::coalesced);
  EXPECT_EQ(sink.statistics().queue.current_depth, 1U);
  EXPECT_EQ(sink.statistics().coalesced, 1U);
}

TEST(MqttPublishSinkTest, PreservesCriticalCapacityAfterNormalLimit) {
  std::ostringstream output{};
  auto logger = std::make_shared<JsonlLogWriter>(output);
  auto settings = config();
  settings.queue = {4U, 3U};
  MqttPublishSink sink(std::move(settings), logger);
  ASSERT_EQ(sink.submit(PublishMessage{telemetry(1U, "topic/1")}), QueuePushStatus::accepted);
  ASSERT_EQ(sink.submit(PublishMessage{telemetry(2U, "topic/2")}), QueuePushStatus::accepted);
  ASSERT_EQ(sink.submit(PublishMessage{telemetry(3U, "topic/3")}), QueuePushStatus::accepted);
  EXPECT_EQ(sink.submit(PublishMessage{telemetry(4U, "topic/4")}), QueuePushStatus::full);

  DeviceStatusEvent status{};
  status.device_name = "motor_actuator";
  status.slave_id = 2U;
  status.state = "offline";
  status.reason = "consecutive_final_failures";
  EXPECT_EQ(sink.submit(PublishMessage{status}), QueuePushStatus::accepted);
  EXPECT_EQ(sink.statistics().queue.current_depth, 4U);
}

} // namespace
