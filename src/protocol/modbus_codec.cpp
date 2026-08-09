#include "industrial_iot_gateway/protocol/modbus_codec.hpp"

#include <optional>

#include "industrial_iot_gateway/protocol/crc16_modbus.hpp"

namespace industrial_iot_gateway::protocol {
namespace {

constexpr std::size_t kMinimumAduSize = 4U;
constexpr std::size_t kFixedRequestAduSize = 8U;
constexpr std::size_t kExceptionAduSize = 5U;
constexpr std::size_t kReadResponseBaseAduSize = 5U;
constexpr std::uint8_t kExceptionFunctionMask = 0x80U;

CodecError make_error(const CodecErrorCategory category, const std::size_t offset = 0U,
                      const std::uint32_t expected = 0U, const std::uint32_t actual = 0U) noexcept {
  return CodecError{category, offset, expected, actual};
}

std::optional<CodecError> validate_slave(const std::uint8_t slave_id,
                                         const bool request_context) noexcept {
  if (slave_id == 0U) {
    return make_error(request_context ? CodecErrorCategory::broadcast_unsupported
                                      : CodecErrorCategory::invalid_slave_address,
                      0U, 1U, slave_id);
  }
  if (slave_id > 247U) {
    return make_error(CodecErrorCategory::invalid_slave_address, 0U, 247U, slave_id);
  }
  return std::nullopt;
}

bool is_read_function(const FunctionCode function) noexcept {
  return function == FunctionCode::read_holding_registers ||
         function == FunctionCode::read_input_registers;
}

std::uint8_t to_byte(const FunctionCode function) noexcept {
  return static_cast<std::uint8_t>(function);
}

std::optional<CodecError> validate_read_request(const ReadRequest &request) noexcept {
  if (const auto slave_error = validate_slave(request.slave_id, true)) {
    return slave_error;
  }
  if (!is_read_function(request.function)) {
    return make_error(CodecErrorCategory::unsupported_function, 1U, 0U, to_byte(request.function));
  }
  if (request.quantity == 0U || request.quantity > kMaxReadRegisterQuantity) {
    return make_error(CodecErrorCategory::invalid_quantity, 4U,
                      static_cast<std::uint32_t>(kMaxReadRegisterQuantity), request.quantity);
  }
  const auto end_address = static_cast<std::uint32_t>(request.start_address) +
                           static_cast<std::uint32_t>(request.quantity) - 1U;
  if (end_address > 0xFFFFU) {
    return make_error(CodecErrorCategory::address_range_overflow, 2U, 0xFFFFU, end_address);
  }
  return std::nullopt;
}

std::optional<CodecError>
validate_write_request(const WriteSingleRegisterRequest &request) noexcept {
  return validate_slave(request.slave_id, true);
}

void append_byte(Adu &adu, const std::uint8_t value) noexcept {
  adu.bytes[adu.size] = value;
  ++adu.size;
}

void append_u16_big_endian(Adu &adu, const std::uint16_t value) noexcept {
  append_byte(adu, static_cast<std::uint8_t>((value >> 8U) & 0x00FFU));
  append_byte(adu, static_cast<std::uint8_t>(value & 0x00FFU));
}

std::uint16_t read_u16_big_endian(const std::uint8_t *data, const std::size_t offset) noexcept {
  return static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[offset]) << 8U |
                                    static_cast<std::uint16_t>(data[offset + 1U]));
}

void append_crc(Adu &adu) noexcept {
  const auto crc = crc16_modbus(adu.bytes.data(), adu.size);
  append_byte(adu, static_cast<std::uint8_t>(crc & 0x00FFU));
  append_byte(adu, static_cast<std::uint8_t>((crc >> 8U) & 0x00FFU));
}

std::optional<CodecError> validate_frame_crc(const std::uint8_t *data,
                                             const std::size_t size) noexcept {
  if (size > kMaxModbusAduSize) {
    return make_error(CodecErrorCategory::frame_too_long, 0U,
                      static_cast<std::uint32_t>(kMaxModbusAduSize),
                      static_cast<std::uint32_t>(size));
  }
  if (data == nullptr || size < kMinimumAduSize) {
    return make_error(CodecErrorCategory::frame_too_short, 0U,
                      static_cast<std::uint32_t>(kMinimumAduSize),
                      static_cast<std::uint32_t>(size));
  }

  const auto crc_offset = size - 2U;
  const auto received_crc = static_cast<std::uint16_t>(
      static_cast<std::uint16_t>(data[crc_offset]) |
      static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[crc_offset + 1U]) << 8U));
  const auto computed_crc = crc16_modbus(data, crc_offset);
  if (received_crc != computed_crc) {
    return make_error(CodecErrorCategory::crc_mismatch, crc_offset, computed_crc, received_crc);
  }
  return std::nullopt;
}

std::optional<CodecError> validate_exact_size(const std::size_t actual,
                                              const std::size_t expected) noexcept {
  if (actual < expected) {
    return make_error(CodecErrorCategory::frame_too_short, actual,
                      static_cast<std::uint32_t>(expected), static_cast<std::uint32_t>(actual));
  }
  if (actual > expected) {
    return make_error(CodecErrorCategory::trailing_bytes, expected,
                      static_cast<std::uint32_t>(expected), static_cast<std::uint32_t>(actual));
  }
  return std::nullopt;
}

bool is_known_exception_code(const std::uint8_t code) noexcept {
  return code >= 0x01U && code <= 0x04U;
}

} // namespace

CodecResult<Adu> encode_request(const Request &request) noexcept {
  Adu adu{};
  if (const auto *read = std::get_if<ReadRequest>(&request)) {
    if (const auto error = validate_read_request(*read)) {
      return *error;
    }
    append_byte(adu, read->slave_id);
    append_byte(adu, to_byte(read->function));
    append_u16_big_endian(adu, read->start_address);
    append_u16_big_endian(adu, read->quantity);
  } else if (const auto *write = std::get_if<WriteSingleRegisterRequest>(&request)) {
    if (const auto error = validate_write_request(*write)) {
      return *error;
    }
    append_byte(adu, write->slave_id);
    append_byte(adu, to_byte(FunctionCode::write_single_register));
    append_u16_big_endian(adu, write->register_address);
    append_u16_big_endian(adu, write->register_value);
  } else {
    return make_error(CodecErrorCategory::unsupported_function);
  }
  append_crc(adu);
  return adu;
}

CodecResult<Request> decode_request(const std::uint8_t *data, const std::size_t size) noexcept {
  if (const auto frame_error = validate_frame_crc(data, size)) {
    return *frame_error;
  }
  if (const auto slave_error = validate_slave(data[0], true)) {
    return *slave_error;
  }

  const auto function = data[1];
  if (function != to_byte(FunctionCode::read_holding_registers) &&
      function != to_byte(FunctionCode::read_input_registers) &&
      function != to_byte(FunctionCode::write_single_register)) {
    return make_error(CodecErrorCategory::unsupported_function, 1U, 0U, function);
  }
  if (const auto size_error = validate_exact_size(size, kFixedRequestAduSize)) {
    return *size_error;
  }

  if (function == to_byte(FunctionCode::write_single_register)) {
    return Request{WriteSingleRegisterRequest{data[0], read_u16_big_endian(data, 2U),
                                              read_u16_big_endian(data, 4U)}};
  }

  const auto read_function = static_cast<FunctionCode>(function);
  const ReadRequest request{data[0], read_function, read_u16_big_endian(data, 2U),
                            read_u16_big_endian(data, 4U)};
  if (const auto request_error = validate_read_request(request)) {
    return *request_error;
  }
  return Request{request};
}

CodecResult<Adu> encode_response(const Response &response) noexcept {
  Adu adu{};
  if (const auto *read = std::get_if<ReadResponse>(&response)) {
    if (const auto slave_error = validate_slave(read->slave_id, false)) {
      return *slave_error;
    }
    if (!is_read_function(read->function)) {
      return make_error(CodecErrorCategory::unsupported_function, 1U, 0U, to_byte(read->function));
    }
    if (read->value_count == 0U || read->value_count > kMaxReadRegisterQuantity) {
      return make_error(CodecErrorCategory::invalid_quantity, 2U,
                        static_cast<std::uint32_t>(kMaxReadRegisterQuantity),
                        static_cast<std::uint32_t>(read->value_count));
    }
    append_byte(adu, read->slave_id);
    append_byte(adu, to_byte(read->function));
    append_byte(adu, static_cast<std::uint8_t>(read->value_count * 2U));
    for (std::size_t index = 0; index < read->value_count; ++index) {
      append_u16_big_endian(adu, read->values[index]);
    }
  } else if (const auto *write = std::get_if<WriteSingleRegisterResponse>(&response)) {
    if (const auto slave_error = validate_slave(write->slave_id, false)) {
      return *slave_error;
    }
    append_byte(adu, write->slave_id);
    append_byte(adu, to_byte(FunctionCode::write_single_register));
    append_u16_big_endian(adu, write->register_address);
    append_u16_big_endian(adu, write->register_value);
  } else if (const auto *exception = std::get_if<ExceptionResponse>(&response)) {
    if (const auto slave_error = validate_slave(exception->slave_id, false)) {
      return *slave_error;
    }
    if (exception->request_function == 0U ||
        exception->request_function >= kExceptionFunctionMask) {
      return make_error(CodecErrorCategory::unsupported_function, 1U, kExceptionFunctionMask - 1U,
                        exception->request_function);
    }
    append_byte(adu, exception->slave_id);
    append_byte(adu,
                static_cast<std::uint8_t>(exception->request_function | kExceptionFunctionMask));
    append_byte(adu, exception->exception_code);
  } else {
    return make_error(CodecErrorCategory::unsupported_function);
  }
  append_crc(adu);
  return adu;
}

CodecResult<Response> decode_response(const std::uint8_t *data, const std::size_t size,
                                      const Request &expected_request) noexcept {
  if (const auto frame_error = validate_frame_crc(data, size)) {
    return *frame_error;
  }
  if (const auto slave_error = validate_slave(data[0], false)) {
    return *slave_error;
  }

  const auto *read_request = std::get_if<ReadRequest>(&expected_request);
  const auto *write_request = std::get_if<WriteSingleRegisterRequest>(&expected_request);
  if (read_request == nullptr && write_request == nullptr) {
    return make_error(CodecErrorCategory::unsupported_function);
  }
  const auto expected_slave =
      read_request != nullptr ? read_request->slave_id : write_request->slave_id;
  if (data[0] != expected_slave) {
    return make_error(CodecErrorCategory::unexpected_slave, 0U, expected_slave, data[0]);
  }

  const auto expected_function = read_request != nullptr
                                     ? to_byte(read_request->function)
                                     : to_byte(FunctionCode::write_single_register);
  const auto received_function = data[1];
  const auto expected_exception_function =
      static_cast<std::uint8_t>(expected_function | kExceptionFunctionMask);
  if (received_function == expected_exception_function) {
    if (const auto size_error = validate_exact_size(size, kExceptionAduSize)) {
      return *size_error;
    }
    return Response{
        ExceptionResponse{data[0], expected_function, data[2], is_known_exception_code(data[2])}};
  }
  if (received_function != expected_function) {
    return make_error(CodecErrorCategory::unexpected_function, 1U, expected_function,
                      received_function);
  }

  if (read_request != nullptr) {
    if (size < kReadResponseBaseAduSize) {
      return make_error(CodecErrorCategory::frame_too_short, size, kReadResponseBaseAduSize,
                        static_cast<std::uint32_t>(size));
    }
    const auto byte_count = data[2];
    if ((byte_count & 0x01U) != 0U || byte_count > kMaxReadRegisterQuantity * 2U) {
      return make_error(CodecErrorCategory::byte_count_mismatch, 2U,
                        static_cast<std::uint32_t>(read_request->quantity) * 2U, byte_count);
    }
    const auto expected_size = kReadResponseBaseAduSize + byte_count;
    if (const auto size_error = validate_exact_size(size, expected_size)) {
      return *size_error;
    }
    const auto expected_byte_count = static_cast<std::uint32_t>(read_request->quantity) * 2U;
    if (byte_count != expected_byte_count) {
      return make_error(CodecErrorCategory::byte_count_mismatch, 2U, expected_byte_count,
                        byte_count);
    }

    ReadResponse response{};
    response.slave_id = data[0];
    response.function = read_request->function;
    response.value_count = byte_count / 2U;
    for (std::size_t index = 0; index < response.value_count; ++index) {
      response.values[index] = read_u16_big_endian(data, 3U + index * 2U);
    }
    return Response{response};
  }

  if (const auto size_error = validate_exact_size(size, kFixedRequestAduSize)) {
    return *size_error;
  }
  const auto response_address = read_u16_big_endian(data, 2U);
  const auto response_value = read_u16_big_endian(data, 4U);
  if (response_address != write_request->register_address ||
      response_value != write_request->register_value) {
    return make_error(CodecErrorCategory::response_echo_mismatch, 2U,
                      static_cast<std::uint32_t>(write_request->register_address) << 16U |
                          write_request->register_value,
                      static_cast<std::uint32_t>(response_address) << 16U | response_value);
  }
  return Response{WriteSingleRegisterResponse{data[0], response_address, response_value}};
}

} // namespace industrial_iot_gateway::protocol
