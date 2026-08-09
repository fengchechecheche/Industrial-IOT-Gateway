#include "industrial_iot_gateway/mqtt/payload_serializer.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <ctime>
#include <optional>
#include <string>

namespace {

using industrial_iot_gateway::mqtt::PayloadSerializer;
using industrial_iot_gateway::pipeline::DeviceStatusEvent;
using industrial_iot_gateway::pipeline::GatewayHealthEvent;
using industrial_iot_gateway::pipeline::PublishMessage;
using industrial_iot_gateway::pipeline::TelemetryEvent;
using industrial_iot_gateway::quality::RegisterQuality;

[[nodiscard]] std::chrono::system_clock::time_point utc(int year, int month, int day, int hour,
                                                        int minute, int second, int millisecond) {
  std::tm value{};
  value.tm_year = year - 1900;
  value.tm_mon = month - 1;
  value.tm_mday = day;
  value.tm_hour = hour;
  value.tm_min = minute;
  value.tm_sec = second;
  return std::chrono::system_clock::from_time_t(::timegm(&value)) +
         std::chrono::milliseconds{millisecond};
}

[[nodiscard]] TelemetryEvent fresh_telemetry() {
  TelemetryEvent event{};
  event.topic = "industrial_iot_gateway/devices/environment_sensor/registers/ambient_temperature_c";
  event.quality = RegisterQuality::fresh;
  event.quality_reason = "valid_sample";
  event.value = 25.3;
  event.device_name = "environment_sensor";
  event.slave_id = 1U;
  event.register_name = "ambient_temperature_c";
  event.raw_value = std::uint64_t{253U};
  event.unit = "degC";
  event.run_id = "8b2144dc-3701-4b80-958f-2bcf918cf307";
  event.sequence = 101U;
  event.source_timestamp = utc(2026, 8, 9, 10, 0, 0, 123);
  event.gateway_timestamp = utc(2026, 8, 9, 10, 0, 0, 126);
  return event;
}

TEST(MqttPayloadSerializerTest, MatchesFrozenFreshTelemetryPayload) {
  const PayloadSerializer serializer("lab_gateway_01");
  const auto publication = serializer.serialize(PublishMessage{fresh_telemetry()});
  ASSERT_TRUE(publication.has_value());
  EXPECT_EQ(publication->qos, 1);
  EXPECT_FALSE(publication->retain);
  EXPECT_EQ(publication->topic, "industrial_iot_gateway/devices/environment_sensor/registers/"
                                "ambient_temperature_c");

  const auto payload = nlohmann::json::parse(publication->payload);
  EXPECT_EQ(payload.at("schema_version"), "1.0");
  EXPECT_EQ(payload.at("message_type"), "telemetry");
  EXPECT_EQ(payload.at("gateway_id"), "lab_gateway_01");
  EXPECT_EQ(payload.at("sequence"), 101U);
  EXPECT_EQ(payload.at("source_timestamp"), "2026-08-09T10:00:00.123Z");
  EXPECT_EQ(payload.at("gateway_timestamp"), "2026-08-09T10:00:00.126Z");
  EXPECT_EQ(payload.at("raw_value"), 253U);
  EXPECT_DOUBLE_EQ(payload.at("engineering_value").get<double>(), 25.3);
}

TEST(MqttPayloadSerializerTest, InvalidTelemetryOmitsEngineeringValue) {
  auto event = fresh_telemetry();
  event.quality = RegisterQuality::invalid;
  event.quality_reason = "invalid_raw_value";
  event.value.reset();
  event.raw_value = std::uint64_t{65535U};
  const PayloadSerializer serializer("lab_gateway_01");
  const auto publication = serializer.serialize(PublishMessage{event});
  ASSERT_TRUE(publication.has_value());
  const auto payload = nlohmann::json::parse(publication->payload);
  EXPECT_EQ(payload.at("quality"), "invalid");
  EXPECT_FALSE(payload.contains("engineering_value"));
  EXPECT_EQ(payload.at("raw_value"), 65535U);
}

TEST(MqttPayloadSerializerTest, StaleTelemetryKeepsHistoricalValueWithoutMqttRetain) {
  auto event = fresh_telemetry();
  event.quality = RegisterQuality::stale;
  event.quality_reason = "freshness_expired";
  event.value_is_retained = true;
  event.sequence = 102U;
  event.gateway_timestamp = utc(2026, 8, 9, 10, 0, 2, 0);
  const PayloadSerializer serializer("lab_gateway_01");
  const auto publication = serializer.serialize(PublishMessage{event});
  ASSERT_TRUE(publication.has_value());
  EXPECT_FALSE(publication->retain);
  const auto payload = nlohmann::json::parse(publication->payload);
  EXPECT_EQ(payload.at("quality"), "stale");
  EXPECT_EQ(payload.at("quality_reason"), "freshness_expired");
  EXPECT_TRUE(payload.at("value_is_retained").get<bool>());
  EXPECT_EQ(payload.at("raw_value"), 253U);
  EXPECT_DOUBLE_EQ(payload.at("engineering_value").get<double>(), 25.3);
}

TEST(MqttPayloadSerializerTest, OfflineWithoutHistoryUsesNullRawAndOmitsEngineeringValue) {
  auto event = fresh_telemetry();
  event.quality = RegisterQuality::offline;
  event.quality_reason = "device_offline";
  event.value_is_retained = false;
  event.value.reset();
  event.raw_value.reset();
  event.sequence = 105U;
  const PayloadSerializer serializer("lab_gateway_01");
  const auto publication = serializer.serialize(PublishMessage{event});
  ASSERT_TRUE(publication.has_value());
  EXPECT_FALSE(publication->retain);
  const auto payload = nlohmann::json::parse(publication->payload);
  EXPECT_EQ(payload.at("quality"), "offline");
  EXPECT_TRUE(payload.at("raw_value").is_null());
  EXPECT_FALSE(payload.contains("engineering_value"));
  EXPECT_FALSE(payload.at("value_is_retained").get<bool>());
}

TEST(MqttPayloadSerializerTest, StatusAndHealthUseRetainedTopics) {
  DeviceStatusEvent status{};
  status.device_name = "motor_actuator";
  status.slave_id = 2U;
  status.state = "offline";
  status.reason = "consecutive_final_failures";
  status.run_id = "run";
  status.sequence = 106U;
  status.source_timestamp = utc(2026, 8, 9, 10, 0, 10, 450);
  status.gateway_timestamp = utc(2026, 8, 9, 10, 0, 10, 452);

  GatewayHealthEvent health{};
  health.state = "online";
  health.reason = "started";
  health.run_id = "run";
  health.sequence = 2U;
  health.source_timestamp = utc(2026, 8, 9, 9, 59, 59, 500);
  health.gateway_timestamp = utc(2026, 8, 9, 9, 59, 59, 502);

  const PayloadSerializer serializer("lab_gateway_01");
  const auto status_publication = serializer.serialize(PublishMessage{status});
  const auto health_publication = serializer.serialize(PublishMessage{health});
  ASSERT_TRUE(status_publication.has_value());
  ASSERT_TRUE(health_publication.has_value());
  EXPECT_TRUE(status_publication->retain);
  EXPECT_TRUE(health_publication->retain);
  EXPECT_EQ(status_publication->topic, "industrial_iot_gateway/devices/motor_actuator/status");
  EXPECT_EQ(health_publication->topic, "industrial_iot_gateway/gateways/lab_gateway_01/health");
}

} // namespace
