#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "industrial_iot_gateway/protocol/crc16_modbus.hpp"
#include "industrial_iot_gateway/protocol/modbus_codec.hpp"

namespace {

using industrial_iot_gateway::protocol::Adu;
using industrial_iot_gateway::protocol::CodecError;
using industrial_iot_gateway::protocol::CodecErrorCategory;
using industrial_iot_gateway::protocol::crc16_modbus;
using industrial_iot_gateway::protocol::decode_request;
using industrial_iot_gateway::protocol::decode_response;
using industrial_iot_gateway::protocol::encode_request;
using industrial_iot_gateway::protocol::encode_response;
using industrial_iot_gateway::protocol::ExceptionResponse;
using industrial_iot_gateway::protocol::FunctionCode;
using industrial_iot_gateway::protocol::ReadRequest;
using industrial_iot_gateway::protocol::ReadResponse;
using industrial_iot_gateway::protocol::Request;
using industrial_iot_gateway::protocol::Response;
using industrial_iot_gateway::protocol::WriteSingleRegisterRequest;
using industrial_iot_gateway::protocol::WriteSingleRegisterResponse;

struct TestVector {
  std::string id;
  std::vector<std::uint8_t> frame;
  std::string direction;
  std::string status;
  std::string expected;
};

std::vector<std::uint8_t> parse_hex_bytes(const std::string &text) {
  std::vector<std::uint8_t> bytes;
  std::istringstream input(text);
  std::string token;
  while (input >> token) {
    bytes.push_back(static_cast<std::uint8_t>(std::stoul(token, nullptr, 16)));
  }
  return bytes;
}

std::vector<TestVector> load_vectors() {
  const std::string path = std::string(GATEWAY_TEST_DATA_DIR) + "/modbus_codec_vectors.csv";
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("failed to open codec vectors: " + path);
  }

  std::string line;
  std::getline(input, line);
  std::vector<TestVector> vectors;
  while (std::getline(input, line)) {
    std::istringstream row(line);
    TestVector vector;
    std::string frame_hex;
    std::getline(row, vector.id, ',');
    std::getline(row, frame_hex, ',');
    std::getline(row, vector.direction, ',');
    std::getline(row, vector.status, ',');
    std::getline(row, vector.expected, ',');
    vector.frame = parse_hex_bytes(frame_hex);
    vectors.push_back(std::move(vector));
  }
  return vectors;
}

const std::vector<TestVector> &vectors() {
  static const auto loaded = load_vectors();
  return loaded;
}

const TestVector &vector_by_id(const std::string &id) {
  const auto &all = vectors();
  const auto found = std::find_if(all.begin(), all.end(),
                                  [&id](const TestVector &vector) { return vector.id == id; });
  if (found == all.end()) {
    throw std::runtime_error("missing codec vector: " + id);
  }
  return *found;
}

std::vector<std::uint8_t> to_vector(const Adu &adu) {
  return {adu.bytes.begin(), adu.bytes.begin() + static_cast<std::ptrdiff_t>(adu.size)};
}

std::vector<std::uint8_t> append_crc(std::vector<std::uint8_t> payload) {
  const auto crc = crc16_modbus(payload.data(), payload.size());
  payload.push_back(static_cast<std::uint8_t>(crc & 0x00FFU));
  payload.push_back(static_cast<std::uint8_t>((crc >> 8U) & 0x00FFU));
  return payload;
}

template <typename T>
void expect_error_category(const std::variant<T, CodecError> &result,
                           const CodecErrorCategory category) {
  ASSERT_TRUE(std::holds_alternative<CodecError>(result));
  EXPECT_EQ(std::get<CodecError>(result).category, category);
}

TEST(ModbusCodecVectorTest, LoadsFrozenCodecSubset) {
  EXPECT_EQ(vectors().size(), 16U);
  EXPECT_EQ(vector_by_id("G-03-REQ-01").direction, "request");
  EXPECT_EQ(vector_by_id("G-EX-06-04").status, "remote_exception");
  EXPECT_EQ(vector_by_id("R-BYTE-COUNT").expected, "byte_count_mismatch");
}

TEST(ModbusCodecRequestTest, EncodesFrozenRequests) {
  const Request read_holding = ReadRequest{1U, FunctionCode::read_holding_registers, 0U, 2U};
  const Request read_input = ReadRequest{1U, FunctionCode::read_input_registers, 0U, 1U};
  const Request write = WriteSingleRegisterRequest{1U, 1U, 3U};
  const Request max_read = ReadRequest{1U, FunctionCode::read_holding_registers, 0U, 125U};

  const auto holding_result = encode_request(read_holding);
  const auto input_result = encode_request(read_input);
  const auto write_result = encode_request(write);
  const auto max_result = encode_request(max_read);
  ASSERT_TRUE(std::holds_alternative<Adu>(holding_result));
  ASSERT_TRUE(std::holds_alternative<Adu>(input_result));
  ASSERT_TRUE(std::holds_alternative<Adu>(write_result));
  ASSERT_TRUE(std::holds_alternative<Adu>(max_result));
  EXPECT_EQ(to_vector(std::get<Adu>(holding_result)), vector_by_id("G-03-REQ-01").frame);
  EXPECT_EQ(to_vector(std::get<Adu>(input_result)), vector_by_id("G-04-REQ-01").frame);
  EXPECT_EQ(to_vector(std::get<Adu>(write_result)), vector_by_id("G-06-REQ-01").frame);
  EXPECT_EQ(to_vector(std::get<Adu>(max_result)), vector_by_id("G-03-MAX-QTY").frame);
}

TEST(ModbusCodecRequestTest, DecodesFrozenRequests) {
  const auto &read_frame = vector_by_id("G-03-REQ-01").frame;
  const auto read_result = decode_request(read_frame.data(), read_frame.size());
  ASSERT_TRUE(std::holds_alternative<Request>(read_result));
  const auto &read = std::get<ReadRequest>(std::get<Request>(read_result));
  EXPECT_EQ(read.slave_id, 1U);
  EXPECT_EQ(read.function, FunctionCode::read_holding_registers);
  EXPECT_EQ(read.start_address, 0U);
  EXPECT_EQ(read.quantity, 2U);

  const auto &write_frame = vector_by_id("G-06-REQ-01").frame;
  const auto write_result = decode_request(write_frame.data(), write_frame.size());
  ASSERT_TRUE(std::holds_alternative<Request>(write_result));
  const auto &write = std::get<WriteSingleRegisterRequest>(std::get<Request>(write_result));
  EXPECT_EQ(write.slave_id, 1U);
  EXPECT_EQ(write.register_address, 1U);
  EXPECT_EQ(write.register_value, 3U);
}

TEST(ModbusCodecRequestTest, RejectsInvalidLocalRequests) {
  expect_error_category(
      encode_request(Request{ReadRequest{0U, FunctionCode::read_holding_registers, 0U, 1U}}),
      CodecErrorCategory::broadcast_unsupported);
  expect_error_category(
      encode_request(Request{ReadRequest{248U, FunctionCode::read_holding_registers, 0U, 1U}}),
      CodecErrorCategory::invalid_slave_address);
  expect_error_category(
      encode_request(Request{ReadRequest{1U, FunctionCode::read_holding_registers, 0U, 0U}}),
      CodecErrorCategory::invalid_quantity);
  expect_error_category(
      encode_request(Request{ReadRequest{1U, FunctionCode::read_holding_registers, 0U, 126U}}),
      CodecErrorCategory::invalid_quantity);
  expect_error_category(
      encode_request(Request{ReadRequest{1U, FunctionCode::read_holding_registers, 0xFFFFU, 2U}}),
      CodecErrorCategory::address_range_overflow);
  expect_error_category(
      encode_request(Request{ReadRequest{1U, static_cast<FunctionCode>(0x10U), 0U, 1U}}),
      CodecErrorCategory::unsupported_function);
}

TEST(ModbusCodecRequestTest, RejectsMalformedRequestAdus) {
  for (const auto &entry :
       {std::pair{"R-QTY-ZERO", CodecErrorCategory::invalid_quantity},
        std::pair{"R-ADDR-OVERFLOW", CodecErrorCategory::address_range_overflow},
        std::pair{"R-BROADCAST", CodecErrorCategory::broadcast_unsupported},
        std::pair{"R-SLAVE-248", CodecErrorCategory::invalid_slave_address},
        std::pair{"R-BAD-CRC", CodecErrorCategory::crc_mismatch}}) {
    const auto &frame = vector_by_id(entry.first).frame;
    expect_error_category(decode_request(frame.data(), frame.size()), entry.second);
  }

  auto short_frame = append_crc({0x01U, 0x03U, 0x00U, 0x00U});
  expect_error_category(decode_request(short_frame.data(), short_frame.size()),
                        CodecErrorCategory::frame_too_short);

  auto trailing = append_crc({0x01U, 0x03U, 0x00U, 0x00U, 0x00U, 0x01U, 0xAAU});
  expect_error_category(decode_request(trailing.data(), trailing.size()),
                        CodecErrorCategory::trailing_bytes);
}

TEST(ModbusCodecResponseTest, EncodesAndDecodesFrozenReadResponses) {
  ReadResponse holding{};
  holding.slave_id = 1U;
  holding.function = FunctionCode::read_holding_registers;
  holding.values[0] = 10U;
  holding.values[1] = 20U;
  holding.value_count = 2U;
  const auto encoded_holding = encode_response(Response{holding});
  ASSERT_TRUE(std::holds_alternative<Adu>(encoded_holding));
  EXPECT_EQ(to_vector(std::get<Adu>(encoded_holding)), vector_by_id("G-03-RSP-01").frame);

  const Request expected = ReadRequest{1U, FunctionCode::read_holding_registers, 0U, 2U};
  const auto &frame = vector_by_id("G-03-RSP-01").frame;
  const auto decoded = decode_response(frame.data(), frame.size(), expected);
  ASSERT_TRUE(std::holds_alternative<Response>(decoded));
  const auto &response = std::get<ReadResponse>(std::get<Response>(decoded));
  EXPECT_EQ(response.value_count, 2U);
  EXPECT_EQ(response.values[0], 10U);
  EXPECT_EQ(response.values[1], 20U);

  ReadResponse input{};
  input.slave_id = 1U;
  input.function = FunctionCode::read_input_registers;
  input.values[0] = 10U;
  input.value_count = 1U;
  const auto encoded_input = encode_response(Response{input});
  ASSERT_TRUE(std::holds_alternative<Adu>(encoded_input));
  EXPECT_EQ(to_vector(std::get<Adu>(encoded_input)), vector_by_id("G-04-RSP-01").frame);

  const Request expected_input = ReadRequest{1U, FunctionCode::read_input_registers, 0U, 1U};
  const auto &input_frame = vector_by_id("G-04-RSP-01").frame;
  const auto decoded_input =
      decode_response(input_frame.data(), input_frame.size(), expected_input);
  ASSERT_TRUE(std::holds_alternative<Response>(decoded_input));
  const auto &input_response = std::get<ReadResponse>(std::get<Response>(decoded_input));
  EXPECT_EQ(input_response.value_count, 1U);
  EXPECT_EQ(input_response.values[0], 10U);
}

TEST(ModbusCodecResponseTest, RejectsInvalidResponseEncoding) {
  ReadResponse response{};
  response.slave_id = 1U;
  response.function = FunctionCode::read_holding_registers;
  response.value_count = 0U;
  expect_error_category(encode_response(Response{response}), CodecErrorCategory::invalid_quantity);

  response.value_count = 126U;
  expect_error_category(encode_response(Response{response}), CodecErrorCategory::invalid_quantity);

  response.value_count = 1U;
  response.function = static_cast<FunctionCode>(0x10U);
  expect_error_category(encode_response(Response{response}),
                        CodecErrorCategory::unsupported_function);

  response.function = FunctionCode::read_holding_registers;
  response.slave_id = 0U;
  expect_error_category(encode_response(Response{response}),
                        CodecErrorCategory::invalid_slave_address);

  expect_error_category(encode_response(Response{ExceptionResponse{1U, 0x80U, 0x01U, true}}),
                        CodecErrorCategory::unsupported_function);
}

TEST(ModbusCodecResponseTest, SupportsMaximumReadResponse) {
  ReadResponse response{};
  response.slave_id = 1U;
  response.function = FunctionCode::read_holding_registers;
  response.value_count = 125U;
  for (std::size_t index = 0; index < response.value_count; ++index) {
    response.values[index] = static_cast<std::uint16_t>(index);
  }
  const auto encoded = encode_response(Response{response});
  ASSERT_TRUE(std::holds_alternative<Adu>(encoded));
  EXPECT_EQ(std::get<Adu>(encoded).size, 255U);

  const auto adu = std::get<Adu>(encoded);
  const Request expected = ReadRequest{1U, FunctionCode::read_holding_registers, 0U, 125U};
  const auto decoded = decode_response(adu.bytes.data(), adu.size, expected);
  ASSERT_TRUE(std::holds_alternative<Response>(decoded));
  const auto &read = std::get<ReadResponse>(std::get<Response>(decoded));
  EXPECT_EQ(read.value_count, 125U);
  EXPECT_EQ(read.values[124], 124U);
}

TEST(ModbusCodecResponseTest, RejectsReadResponseMismatches) {
  const Request expected = ReadRequest{1U, FunctionCode::read_holding_registers, 0U, 2U};
  const auto &byte_count = vector_by_id("R-BYTE-COUNT").frame;
  expect_error_category(decode_response(byte_count.data(), byte_count.size(), expected),
                        CodecErrorCategory::byte_count_mismatch);

  const auto wrong_slave = append_crc({0x02U, 0x03U, 0x04U, 0x00U, 0x0AU, 0x00U, 0x14U});
  expect_error_category(decode_response(wrong_slave.data(), wrong_slave.size(), expected),
                        CodecErrorCategory::unexpected_slave);

  const auto wrong_function = append_crc({0x01U, 0x04U, 0x04U, 0x00U, 0x0AU, 0x00U, 0x14U});
  expect_error_category(decode_response(wrong_function.data(), wrong_function.size(), expected),
                        CodecErrorCategory::unexpected_function);
}

TEST(ModbusCodecResponseTest, HandlesWriteEchoAndMismatch) {
  const WriteSingleRegisterResponse response{1U, 1U, 3U};
  const auto encoded = encode_response(Response{response});
  ASSERT_TRUE(std::holds_alternative<Adu>(encoded));
  EXPECT_EQ(to_vector(std::get<Adu>(encoded)), vector_by_id("G-06-RSP-01").frame);

  const Request expected = WriteSingleRegisterRequest{1U, 1U, 3U};
  const auto &frame = vector_by_id("G-06-RSP-01").frame;
  const auto decoded = decode_response(frame.data(), frame.size(), expected);
  ASSERT_TRUE(std::holds_alternative<Response>(decoded));
  EXPECT_TRUE(std::holds_alternative<WriteSingleRegisterResponse>(std::get<Response>(decoded)));

  const auto wrong_echo = append_crc({0x01U, 0x06U, 0x00U, 0x01U, 0x00U, 0x04U});
  expect_error_category(decode_response(wrong_echo.data(), wrong_echo.size(), expected),
                        CodecErrorCategory::response_echo_mismatch);
}

TEST(ModbusCodecResponseTest, EncodesAndDecodesExceptions) {
  const std::vector<std::pair<std::string, std::uint8_t>> cases{
      {"G-EX-03-02", 0x03U}, {"G-EX-04-03", 0x04U}, {"G-EX-06-04", 0x06U}};
  for (const auto &[id, request_function] : cases) {
    const auto &frame = vector_by_id(id).frame;
    const ExceptionResponse response{1U, request_function, frame[2], true};
    const auto encoded = encode_response(Response{response});
    ASSERT_TRUE(std::holds_alternative<Adu>(encoded));
    EXPECT_EQ(to_vector(std::get<Adu>(encoded)), frame);

    Request expected = ReadRequest{1U, FunctionCode::read_holding_registers, 0U, 1U};
    if (request_function == 0x04U) {
      expected = ReadRequest{1U, FunctionCode::read_input_registers, 0U, 1U};
    } else if (request_function == 0x06U) {
      expected = WriteSingleRegisterRequest{1U, 1U, 3U};
    }
    const auto decoded = decode_response(frame.data(), frame.size(), expected);
    ASSERT_TRUE(std::holds_alternative<Response>(decoded));
    const auto &exception = std::get<ExceptionResponse>(std::get<Response>(decoded));
    EXPECT_EQ(exception.exception_code, frame[2]);
    EXPECT_TRUE(exception.is_known_exception);
  }

  const auto unknown_frame = append_crc({0x01U, 0x83U, 0x7FU});
  const Request expected = ReadRequest{1U, FunctionCode::read_holding_registers, 0U, 1U};
  const auto unknown = decode_response(unknown_frame.data(), unknown_frame.size(), expected);
  ASSERT_TRUE(std::holds_alternative<Response>(unknown));
  const auto &exception = std::get<ExceptionResponse>(std::get<Response>(unknown));
  EXPECT_EQ(exception.exception_code, 0x7FU);
  EXPECT_FALSE(exception.is_known_exception);
}

TEST(ModbusCodecResponseTest, RejectsMalformedResponses) {
  const Request expected = ReadRequest{1U, FunctionCode::read_holding_registers, 0U, 1U};
  auto bad_crc = vector_by_id("G-03-RSP-01").frame;
  bad_crc.back() ^= 0x01U;
  expect_error_category(decode_response(bad_crc.data(), bad_crc.size(), expected),
                        CodecErrorCategory::crc_mismatch);

  const auto wrong_exception = append_crc({0x01U, 0x84U, 0x02U});
  expect_error_category(decode_response(wrong_exception.data(), wrong_exception.size(), expected),
                        CodecErrorCategory::unexpected_function);

  const auto trailing = append_crc({0x01U, 0x03U, 0x02U, 0x00U, 0x0AU, 0xAAU});
  expect_error_category(decode_response(trailing.data(), trailing.size(), expected),
                        CodecErrorCategory::trailing_bytes);
}

TEST(ModbusCodecBoundaryTest, RejectsNullShortAndOversizedInputs) {
  expect_error_category(decode_request(nullptr, 0U), CodecErrorCategory::frame_too_short);

  const Request expected = ReadRequest{1U, FunctionCode::read_holding_registers, 0U, 1U};
  expect_error_category(decode_response(nullptr, 0U, expected),
                        CodecErrorCategory::frame_too_short);

  std::vector<std::uint8_t> oversized(257U, 0U);
  expect_error_category(decode_request(oversized.data(), oversized.size()),
                        CodecErrorCategory::frame_too_long);
  expect_error_category(decode_response(oversized.data(), oversized.size(), expected),
                        CodecErrorCategory::frame_too_long);
}

} // namespace
