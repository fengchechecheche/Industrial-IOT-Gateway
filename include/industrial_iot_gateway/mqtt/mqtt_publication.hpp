#pragma once

#include <string>

namespace industrial_iot_gateway::mqtt {

struct MqttPublication {
  std::string topic{};
  std::string payload{};
  int qos{1};
  bool retain{};
};

} // namespace industrial_iot_gateway::mqtt
