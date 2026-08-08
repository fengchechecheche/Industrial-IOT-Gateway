#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "industrial_iot_gateway/protocol/crc16_modbus.hpp"

namespace {

using industrial_iot_gateway::protocol::crc16_modbus;
using industrial_iot_gateway::protocol::kCrc16ModbusInitialValue;

struct GoldenVector {
  std::string id;
  std::vector<std::uint8_t> frame;
  std::string crc_status;
  std::string expected;
};

std::vector<std::uint8_t> parse_hex_bytes(const std::string &text) {
  std::vector<std::uint8_t> bytes;
  std::istringstream input(text);
  std::string token;

  while (input >> token) {
    const auto value = std::stoul(token, nullptr, 16);
    if (value > 0xFFU) {
      throw std::runtime_error("hex byte is outside 0x00..0xFF");
    }
    bytes.push_back(static_cast<std::uint8_t>(value));
  }

  return bytes;
}

std::vector<GoldenVector> load_golden_vectors() {
  const std::string path = std::string(GATEWAY_TEST_DATA_DIR) + "/modbus_rtu_golden_vectors.csv";
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("failed to open golden vector file: " + path);
  }

  std::vector<GoldenVector> vectors;
  std::string line;
  std::getline(input, line); // Header.

  while (std::getline(input, line)) {
    if (line.empty()) {
      continue;
    }

    std::istringstream row(line);
    GoldenVector vector;
    std::string frame_hex;
    std::getline(row, vector.id, ',');
    std::getline(row, frame_hex, ',');
    std::getline(row, vector.crc_status, ',');
    std::getline(row, vector.expected);
    vector.frame = parse_hex_bytes(frame_hex);
    vectors.push_back(std::move(vector));
  }

  return vectors;
}

TEST(Crc16ModbusTest, ReturnsInitialValueForEmptyInput) {
  EXPECT_EQ(crc16_modbus(nullptr, 0U), kCrc16ModbusInitialValue);
}

TEST(Crc16ModbusTest, MatchesSingleZeroByteReference) {
  constexpr std::uint8_t data[] = {0x00U};
  EXPECT_EQ(crc16_modbus(data, 1U), 0x40BFU);
}

TEST(Crc16ModbusTest, MatchesStandardCheckString) {
  const std::string data = "123456789";
  EXPECT_EQ(crc16_modbus(reinterpret_cast<const std::uint8_t *>(data.data()), data.size()),
            0x4B37U);
}

TEST(Crc16ModbusTest, CoversMaximumModbusAduPayloadLength) {
  std::vector<std::uint8_t> data(254U);
  for (std::size_t index = 0; index < data.size(); ++index) {
    data[index] = static_cast<std::uint8_t>(index);
  }
  EXPECT_EQ(crc16_modbus(data.data(), data.size()), 0x576CU);
}

TEST(Crc16ModbusTest, DetectsOneBitFlip) {
  std::vector<std::uint8_t> data{'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  const auto original_crc = crc16_modbus(data.data(), data.size());
  data[4] ^= 0x01U;
  EXPECT_NE(crc16_modbus(data.data(), data.size()), original_crc);
}

TEST(Crc16ModbusTest, ValidatesFrozenRtuGoldenVectorsAndWireOrder) {
  const auto vectors = load_golden_vectors();
  std::size_t valid_count = 0U;
  std::size_t invalid_count = 0U;
  std::size_t not_applicable_count = 0U;

  for (const auto &vector : vectors) {
    SCOPED_TRACE(vector.id);
    EXPECT_FALSE(vector.expected.empty());

    if (vector.crc_status == "not_applicable") {
      ++not_applicable_count;
      continue;
    }

    ASSERT_GE(vector.frame.size(), 3U);
    const auto payload_size = vector.frame.size() - 2U;
    const auto computed_crc = crc16_modbus(vector.frame.data(), payload_size);
    const auto received_crc =
        static_cast<std::uint16_t>(vector.frame[payload_size]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(vector.frame[payload_size + 1U])
                                   << 8U);

    if (vector.crc_status == "valid") {
      ++valid_count;
      EXPECT_EQ(computed_crc, received_crc);
    } else if (vector.crc_status == "invalid") {
      ++invalid_count;
      EXPECT_NE(computed_crc, received_crc);
    } else {
      FAIL() << "unknown crc_status: " << vector.crc_status;
    }
  }

  EXPECT_EQ(vectors.size(), 19U);
  EXPECT_EQ(valid_count, 15U);
  EXPECT_EQ(invalid_count, 1U);
  EXPECT_EQ(not_applicable_count, 3U);
}

} // namespace
