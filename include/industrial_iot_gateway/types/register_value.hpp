#pragma once

#include <cstdint>
#include <variant>

namespace industrial_iot_gateway::types {

using RegisterValue = std::variant<std::int64_t, std::uint64_t, double>;

} // namespace industrial_iot_gateway::types
