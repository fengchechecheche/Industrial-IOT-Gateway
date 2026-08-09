#include "industrial_iot_gateway/mqtt/payload_serializer.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

namespace industrial_iot_gateway::mqtt {
namespace {

using Json = nlohmann::json;

struct CommonPayloadFields {
  std::string gateway_id{};
  std::string message_type{};
  std::string run_id{};
  std::uint64_t sequence{};
  std::chrono::system_clock::time_point source_timestamp{};
  std::chrono::system_clock::time_point gateway_timestamp{};
};

[[nodiscard]] std::string format_timestamp(std::chrono::system_clock::time_point value) {
  const auto milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(value.time_since_epoch());
  auto second_count = std::chrono::duration_cast<std::chrono::seconds>(milliseconds);
  auto remainder = milliseconds - second_count;
  if (remainder.count() < 0) {
    second_count -= std::chrono::seconds{1};
    remainder += std::chrono::seconds{1};
  }
  const std::time_t seconds = second_count.count();
  std::tm utc{};
  static_cast<void>(::gmtime_r(&seconds, &utc));
  std::ostringstream output{};
  output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(3) << std::setfill('0')
         << remainder.count() << 'Z';
  return output.str();
}

[[nodiscard]] std::string_view quality_name(quality::RegisterQuality quality) noexcept {
  switch (quality) {
  case quality::RegisterQuality::fresh:
    return "fresh";
  case quality::RegisterQuality::stale:
    return "stale";
  case quality::RegisterQuality::offline:
    return "offline";
  case quality::RegisterQuality::invalid:
  case quality::RegisterQuality::no_valid_sample:
    return "invalid";
  }
  return "invalid";
}

void add_common(Json &payload, const CommonPayloadFields &fields) {
  payload["schema_version"] = "1.0";
  payload["message_type"] = fields.message_type;
  payload["gateway_id"] = fields.gateway_id;
  payload["run_id"] = fields.run_id;
  payload["sequence"] = fields.sequence;
  payload["source_timestamp"] = format_timestamp(fields.source_timestamp);
  payload["gateway_timestamp"] = format_timestamp(fields.gateway_timestamp);
}

void add_raw_value(Json &payload, const std::optional<types::RegisterValue> &value) {
  if (!value.has_value()) {
    payload["raw_value"] = nullptr;
    return;
  }
  std::visit([&payload](const auto raw) { payload["raw_value"] = raw; }, *value);
}

[[nodiscard]] MqttPublication serialize_telemetry(const pipeline::TelemetryEvent &event,
                                                  const std::string &gateway_id) {
  Json payload{};
  add_common(payload, {gateway_id, "telemetry", event.run_id, event.sequence,
                       event.source_timestamp, event.gateway_timestamp});
  payload["device_name"] = event.device_name;
  payload["slave_id"] = event.slave_id;
  payload["register_name"] = event.register_name;
  add_raw_value(payload, event.raw_value);
  if (event.value.has_value() && event.quality != quality::RegisterQuality::invalid &&
      event.quality != quality::RegisterQuality::no_valid_sample) {
    payload["engineering_value"] = *event.value;
  }
  payload["unit"] = event.unit;
  payload["quality"] = quality_name(event.quality);
  payload["quality_reason"] = event.quality_reason;
  payload["value_is_retained"] = event.value_is_retained;
  return {event.topic, payload.dump(), 1, false};
}

[[nodiscard]] MqttPublication serialize_device_status(const pipeline::DeviceStatusEvent &event,
                                                      const std::string &gateway_id) {
  Json payload{};
  add_common(payload, {gateway_id, "device_status", event.run_id, event.sequence,
                       event.source_timestamp, event.gateway_timestamp});
  payload["device_name"] = event.device_name;
  payload["slave_id"] = event.slave_id;
  payload["state"] = event.state;
  payload["reason"] = event.reason;
  return {"industrial_iot_gateway/devices/" + event.device_name + "/status", payload.dump(), 1,
          true};
}

[[nodiscard]] MqttPublication serialize_health(const pipeline::GatewayHealthEvent &event,
                                               const std::string &gateway_id) {
  Json payload{};
  add_common(payload, {gateway_id, "gateway_health", event.run_id, event.sequence,
                       event.source_timestamp, event.gateway_timestamp});
  payload["state"] = event.state;
  payload["reason"] = event.reason;
  payload["lwt"] = event.lwt;
  return {"industrial_iot_gateway/gateways/" + gateway_id + "/health", payload.dump(), 1, true};
}

} // namespace

PayloadSerializer::PayloadSerializer(std::string gateway_id) : gateway_id_(std::move(gateway_id)) {}

std::optional<MqttPublication>
PayloadSerializer::serialize(const pipeline::PublishMessage &message) const {
  if (const auto *telemetry = std::get_if<pipeline::TelemetryEvent>(&message)) {
    return serialize_telemetry(*telemetry, gateway_id_);
  }
  if (const auto *status = std::get_if<pipeline::DeviceStatusEvent>(&message)) {
    return serialize_device_status(*status, gateway_id_);
  }
  if (const auto *health = std::get_if<pipeline::GatewayHealthEvent>(&message)) {
    return serialize_health(*health, gateway_id_);
  }
  return std::nullopt;
}

} // namespace industrial_iot_gateway::mqtt
