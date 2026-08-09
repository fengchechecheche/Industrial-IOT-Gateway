#pragma once

#include <chrono>
#include <string>

#include "industrial_iot_gateway/concurrency/bounded_queue.hpp"

namespace industrial_iot_gateway::mqtt {

struct MqttConfig {
  std::string broker_uri{"tcp://127.0.0.1:1883"};
  std::string client_id{"iiotgwLab01"};
  std::string gateway_id{"lab_gateway_01"};
  std::string run_id{};
  std::chrono::seconds keep_alive{20};
  std::chrono::seconds connect_timeout{5};
  std::chrono::milliseconds publish_ack_timeout{5000};
  std::chrono::milliseconds drain_timeout{2000};
  concurrency::QueueConfig queue{1024U, 820U};
};

} // namespace industrial_iot_gateway::mqtt
