#include "industrial_iot_gateway/mqtt/reconnect_policy.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace industrial_iot_gateway::mqtt {

std::chrono::milliseconds
ReconnectPolicy::nominal_delay(const std::size_t failure_count) const noexcept {
  using namespace std::chrono_literals;
  constexpr std::array delays{1s, 2s, 4s, 8s, 16s, 30s};
  return delays[std::min(failure_count, delays.size() - 1U)];
}

std::chrono::milliseconds
ReconnectPolicy::jittered_delay(const std::size_t failure_count,
                                const JitterSample sample) const noexcept {
  const auto nominal = nominal_delay(failure_count);
  const double bounded = std::clamp(sample.value, -1.0, 1.0);
  const double multiplier = 1.0 + bounded * 0.2;
  const auto nominal_count = static_cast<double>(nominal.count());
  return std::chrono::milliseconds{
      static_cast<std::chrono::milliseconds::rep>(std::llround(nominal_count * multiplier))};
}

} // namespace industrial_iot_gateway::mqtt
