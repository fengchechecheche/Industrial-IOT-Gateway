#include "industrial_iot_gateway/protocol/crc16_modbus.hpp"

namespace industrial_iot_gateway::protocol {

std::uint16_t crc16_modbus(const std::uint8_t *data, const std::size_t size) noexcept {
  auto crc = kCrc16ModbusInitialValue;

  for (std::size_t index = 0; index < size; ++index) {
    crc ^= data[index];
    for (unsigned int bit = 0; bit < 8U; ++bit) {
      const bool least_significant_bit_is_set = (crc & 0x0001U) != 0U;
      crc >>= 1U;
      if (least_significant_bit_is_set) {
        crc ^= kCrc16ModbusReflectedPolynomial;
      }
    }
  }

  return crc;
}

} // namespace industrial_iot_gateway::protocol
