#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <variant>

namespace industrial_iot_gateway::protocol {

inline constexpr std::size_t kMaxModbusAduSize = 256U;
inline constexpr std::size_t kMaxReadRegisterQuantity = 125U;

enum class FunctionCode : std::uint8_t {
  read_holding_registers = 0x03U,
  read_input_registers = 0x04U,
  write_single_register = 0x06U,
};

enum class CodecErrorCategory {
  invalid_slave_address,
  broadcast_unsupported,
  unsupported_function,
  invalid_quantity,
  address_range_overflow,
  frame_too_short,
  frame_too_long,
  crc_mismatch,
  byte_count_mismatch,
  unexpected_slave,
  unexpected_function,
  response_echo_mismatch,
  trailing_bytes,
};

struct CodecError {
  CodecErrorCategory category{};
  std::size_t offset{};
  std::uint32_t expected{};
  std::uint32_t actual{};
};

struct Adu {
  std::array<std::uint8_t, kMaxModbusAduSize> bytes{};
  std::size_t size{};
};

struct ReadRequest {
  std::uint8_t slave_id{};
  FunctionCode function{};
  std::uint16_t start_address{};
  std::uint16_t quantity{};
};

struct WriteSingleRegisterRequest {
  std::uint8_t slave_id{};
  std::uint16_t register_address{};
  std::uint16_t register_value{};
};

using Request = std::variant<ReadRequest, WriteSingleRegisterRequest>;

struct ReadResponse {
  std::uint8_t slave_id{};
  FunctionCode function{};
  std::array<std::uint16_t, kMaxReadRegisterQuantity> values{};
  std::size_t value_count{};
};

struct WriteSingleRegisterResponse {
  std::uint8_t slave_id{};
  std::uint16_t register_address{};
  std::uint16_t register_value{};
};

struct ExceptionResponse {
  std::uint8_t slave_id{};
  std::uint8_t request_function{};
  std::uint8_t exception_code{};
  bool is_known_exception{};
};

using Response = std::variant<ReadResponse, WriteSingleRegisterResponse, ExceptionResponse>;

template <typename T> using CodecResult = std::variant<T, CodecError>;

[[nodiscard]] CodecResult<Adu> encode_request(const Request &request) noexcept;
[[nodiscard]] CodecResult<Request> decode_request(const std::uint8_t *data,
                                                  std::size_t size) noexcept;
[[nodiscard]] CodecResult<Adu> encode_response(const Response &response) noexcept;
[[nodiscard]] CodecResult<Response> decode_response(const std::uint8_t *data, std::size_t size,
                                                    const Request &expected_request) noexcept;

} // namespace industrial_iot_gateway::protocol
