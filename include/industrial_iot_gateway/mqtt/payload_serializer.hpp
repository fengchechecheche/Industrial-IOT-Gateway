#pragma once

#include <optional>
#include <string>

#include "industrial_iot_gateway/mqtt/mqtt_publication.hpp"
#include "industrial_iot_gateway/pipeline/pipeline_messages.hpp"

namespace industrial_iot_gateway::mqtt {

class PayloadSerializer {
public:
  explicit PayloadSerializer(std::string gateway_id);

  [[nodiscard]] std::optional<MqttPublication>
  serialize(const pipeline::PublishMessage &message) const;

private:
  std::string gateway_id_{};
};

} // namespace industrial_iot_gateway::mqtt
