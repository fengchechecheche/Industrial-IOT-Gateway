#pragma once

#include <cstddef>
#include <cstdint>

namespace industrial_iot_gateway::protocol {

inline constexpr std::uint16_t kCrc16ModbusInitialValue = 0xFFFFU;
inline constexpr std::uint16_t kCrc16ModbusReflectedPolynomial = 0xA001U;

// The caller may pass nullptr only when size is zero.
// This API returns the 16-bit CRC value and does not serialize wire-order bytes.
[[nodiscard]] std::uint16_t crc16_modbus(const std::uint8_t *data, std::size_t size) noexcept;

} // namespace industrial_iot_gateway::protocol
