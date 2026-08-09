#pragma once

#include <chrono>
#include <cstddef>

namespace industrial_iot_gateway::mqtt {

struct JitterSample {
  double value{};
};

class ReconnectPolicy {
public:
  [[nodiscard]] std::chrono::milliseconds nominal_delay(std::size_t failure_count) const noexcept;
  [[nodiscard]] std::chrono::milliseconds jittered_delay(std::size_t failure_count,
                                                         JitterSample sample) const noexcept;
};

} // namespace industrial_iot_gateway::mqtt
