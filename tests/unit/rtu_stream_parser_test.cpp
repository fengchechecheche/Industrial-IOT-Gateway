#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "industrial_iot_gateway/protocol/rtu_stream_parser.hpp"

namespace {

using industrial_iot_gateway::protocol::encode_response;
using industrial_iot_gateway::protocol::ExceptionResponse;
using industrial_iot_gateway::protocol::FunctionCode;
using industrial_iot_gateway::protocol::kRtuParserBufferCapacity;
using industrial_iot_gateway::protocol::MonotonicTimeUs;
using industrial_iot_gateway::protocol::ParseBatchResult;
using industrial_iot_gateway::protocol::ParsedRequestEvent;
using industrial_iot_gateway::protocol::ParsedResponseEvent;
using industrial_iot_gateway::protocol::ParserError;
using industrial_iot_gateway::protocol::ParserErrorCategory;
using industrial_iot_gateway::protocol::ParserEvent;
using industrial_iot_gateway::protocol::ParserTiming;
using industrial_iot_gateway::protocol::ReadRequest;
using industrial_iot_gateway::protocol::ReadResponse;
using industrial_iot_gateway::protocol::Request;
using industrial_iot_gateway::protocol::Response;
using industrial_iot_gateway::protocol::RtuStreamParser;

constexpr ParserTiming kTiming{1'000U, 3'000U};

constexpr MonotonicTimeUs at(const std::uint64_t value) { return MonotonicTimeUs{value}; }

std::vector<std::uint8_t> parse_hex(const std::string &text) {
  std::vector<std::uint8_t> bytes;
  std::istringstream input(text);
  std::string token;
  while (input >> token) {
    bytes.push_back(static_cast<std::uint8_t>(std::stoul(token, nullptr, 16)));
  }
  return bytes;
}

std::vector<std::uint8_t> load_corpus(const std::string &name) {
  const std::string path = std::string(GATEWAY_TEST_DATA_DIR) + "/parser_corpus/" + name + ".hex";
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("failed to open parser corpus: " + path);
  }
  std::ostringstream text;
  text << input.rdbuf();
  return parse_hex(text.str());
}

Request expected_read_holding(const std::uint16_t quantity = 2U) {
  return ReadRequest{1U, FunctionCode::read_holding_registers, 0U, quantity};
}

std::size_t count_request_events(const ParserEvent *events, const std::size_t count) {
  std::size_t result = 0U;
  for (std::size_t index = 0; index < count; ++index) {
    if (std::holds_alternative<ParsedRequestEvent>(events[index].payload)) {
      ++result;
    }
  }
  return result;
}

std::size_t count_response_events(const ParserEvent *events, const std::size_t count) {
  std::size_t result = 0U;
  for (std::size_t index = 0; index < count; ++index) {
    if (std::holds_alternative<ParsedResponseEvent>(events[index].payload)) {
      ++result;
    }
  }
  return result;
}

TEST(RtuStreamParserConfigurationTest, ValidatesTimingAndModes) {
  RtuStreamParser valid(kTiming);
  EXPECT_TRUE(valid.timing_valid());
  EXPECT_TRUE(valid.begin_response_stream(42U, expected_read_holding()));
  valid.clear_request_context();
  valid.begin_request_stream();

  RtuStreamParser zero_inter_character(ParserTiming{0U, 3'000U});
  EXPECT_FALSE(zero_inter_character.timing_valid());
  RtuStreamParser inverted(ParserTiming{3'000U, 1'000U});
  EXPECT_FALSE(inverted.timing_valid());
}

TEST(RtuStreamParserRequestTest, ParsesEveryTwoPartSplitAndByteByByte) {
  const auto frame = load_corpus("valid_request");
  for (std::size_t split = 0U; split <= frame.size(); ++split) {
    RtuStreamParser parser(kTiming);
    parser.begin_request_stream();
    std::array<ParserEvent, 2U> events{};
    const auto first = parser.ingest(frame.data(), split, at(100U), events.data(), events.size());
    const auto second = parser.ingest(
        frame.data() + static_cast<std::ptrdiff_t>(split), frame.size() - split, at(200U),
        events.data() + first.events_written, events.size() - first.events_written);
    EXPECT_EQ(first.events_written + second.events_written, 1U) << "split=" << split;
    EXPECT_EQ(count_request_events(events.data(), 1U), 1U) << "split=" << split;
    EXPECT_EQ(parser.buffered_size(), 0U);
  }

  RtuStreamParser parser(kTiming);
  parser.begin_request_stream();
  std::array<ParserEvent, 2U> events{};
  std::size_t event_count = 0U;
  for (std::size_t index = 0U; index < frame.size(); ++index) {
    const auto result = parser.ingest(&frame[index], 1U, at(100U + index),
                                      events.data() + event_count, events.size() - event_count);
    event_count += result.events_written;
  }
  EXPECT_EQ(event_count, 1U);
  EXPECT_EQ(count_request_events(events.data(), event_count), 1U);
}

TEST(RtuStreamParserRequestTest, ParsesConcatenatedRequestsWithOutputBackpressure) {
  const auto bytes = load_corpus("concatenated_requests");
  RtuStreamParser parser(kTiming);
  parser.begin_request_stream();

  std::array<ParserEvent, 1U> first_event{};
  const auto first = parser.ingest(bytes.data(), bytes.size(), at(100U), first_event.data(), 1U);
  EXPECT_EQ(first.events_written, 1U);
  EXPECT_TRUE(first.output_full);
  EXPECT_EQ(first.bytes_consumed, 8U);
  EXPECT_EQ(count_request_events(first_event.data(), 1U), 1U);

  std::array<ParserEvent, 1U> second_event{};
  const auto second =
      parser.ingest(bytes.data() + static_cast<std::ptrdiff_t>(first.bytes_consumed),
                    bytes.size() - first.bytes_consumed, at(200U), second_event.data(), 1U);
  EXPECT_EQ(second.bytes_consumed, 8U);
  EXPECT_EQ(second.events_written, 1U);
  EXPECT_EQ(count_request_events(second_event.data(), 1U), 1U);
}

TEST(RtuStreamParserRequestTest, DiscardsNoiseAndRecoversAfterBadCrc) {
  {
    const auto bytes = load_corpus("noise_prefix");
    RtuStreamParser parser(kTiming);
    parser.begin_request_stream();
    std::array<ParserEvent, 4U> events{};
    const auto result =
        parser.ingest(bytes.data(), bytes.size(), at(100U), events.data(), events.size());
    EXPECT_EQ(result.events_written, 1U);
    EXPECT_EQ(count_request_events(events.data(), result.events_written), 1U);
    EXPECT_EQ(parser.discarded_noise_bytes(), 2U);
  }

  {
    const auto bytes = load_corpus("bad_crc_then_valid");
    RtuStreamParser parser(kTiming);
    parser.begin_request_stream();
    std::array<ParserEvent, 4U> events{};
    const auto result =
        parser.ingest(bytes.data(), bytes.size(), at(100U), events.data(), events.size());
    ASSERT_EQ(result.events_written, 2U);
    ASSERT_TRUE(std::holds_alternative<ParserError>(events[0].payload));
    EXPECT_EQ(std::get<ParserError>(events[0].payload).category, ParserErrorCategory::crc_mismatch);
    EXPECT_TRUE(std::holds_alternative<ParsedRequestEvent>(events[1].payload));
  }
}

TEST(RtuStreamParserResponseTest, ParsesSplitReadAndExceptionResponses) {
  const auto response = parse_hex("01 03 04 00 0A 00 14 DA 3E");
  for (std::size_t split = 0U; split <= response.size(); ++split) {
    RtuStreamParser parser(kTiming);
    ASSERT_TRUE(parser.begin_response_stream(42U, expected_read_holding()));
    std::array<ParserEvent, 2U> events{};
    const auto first =
        parser.ingest(response.data(), split, at(100U), events.data(), events.size());
    const auto second = parser.ingest(
        response.data() + static_cast<std::ptrdiff_t>(split), response.size() - split, at(200U),
        events.data() + first.events_written, events.size() - first.events_written);
    EXPECT_EQ(first.events_written + second.events_written, 1U) << "split=" << split;
    EXPECT_EQ(count_response_events(events.data(), 1U), 1U) << "split=" << split;
    if (std::holds_alternative<ParsedResponseEvent>(events[0].payload)) {
      EXPECT_EQ(std::get<ParsedResponseEvent>(events[0].payload).request_id, 42U);
    }
  }

  const auto exception = load_corpus("exception_response");
  RtuStreamParser parser(kTiming);
  ASSERT_TRUE(parser.begin_response_stream(43U, expected_read_holding()));
  std::array<ParserEvent, 2U> events{};
  const auto result =
      parser.ingest(exception.data(), exception.size(), at(300U), events.data(), events.size());
  ASSERT_EQ(result.events_written, 1U);
  const auto &parsed = std::get<ParsedResponseEvent>(events[0].payload);
  const auto &remote = std::get<ExceptionResponse>(parsed.response);
  EXPECT_EQ(remote.exception_code, 0x02U);
}

TEST(RtuStreamParserResponseTest, RejectsWrongContextAndRecovers) {
  const auto wrong_slave = parse_hex("02 03 02 00 0A 7C 43");
  const auto valid = parse_hex("01 03 02 00 0A 38 43");
  std::vector<std::uint8_t> input = wrong_slave;
  input.insert(input.end(), valid.begin(), valid.end());

  RtuStreamParser parser(kTiming);
  ASSERT_TRUE(parser.begin_response_stream(50U, expected_read_holding(1U)));
  std::array<ParserEvent, 4U> events{};
  const auto result =
      parser.ingest(input.data(), input.size(), at(100U), events.data(), events.size());
  ASSERT_EQ(result.events_written, 2U);
  ASSERT_TRUE(std::holds_alternative<ParserError>(events[0].payload));
  EXPECT_EQ(std::get<ParserError>(events[0].payload).category,
            ParserErrorCategory::unexpected_slave);
  EXPECT_TRUE(std::holds_alternative<ParsedResponseEvent>(events[1].payload));
}

TEST(RtuStreamParserResponseTest, RecoversAfterInvalidByteCount) {
  const auto bad_byte_count = parse_hex("01 03 03 00 0A 00 43 2E");
  const auto valid = parse_hex("01 03 04 00 0A 00 14 DA 3E");
  std::vector<std::uint8_t> input = bad_byte_count;
  input.insert(input.end(), valid.begin(), valid.end());

  RtuStreamParser parser(kTiming);
  ASSERT_TRUE(parser.begin_response_stream(51U, expected_read_holding()));
  std::array<ParserEvent, 4U> events{};
  const auto result =
      parser.ingest(input.data(), input.size(), at(100U), events.data(), events.size());
  ASSERT_GE(result.events_written, 2U);
  ASSERT_TRUE(std::holds_alternative<ParserError>(events[0].payload));
  EXPECT_EQ(std::get<ParserError>(events[0].payload).category,
            ParserErrorCategory::byte_count_mismatch);
  EXPECT_TRUE(
      std::holds_alternative<ParsedResponseEvent>(events[result.events_written - 1U].payload));
}

TEST(RtuStreamParserResponseTest, RejectsOversizedCandidateAndRecovers) {
  const auto input = load_corpus("oversized_response_prefix");
  RtuStreamParser parser(kTiming);
  ASSERT_TRUE(parser.begin_response_stream(54U, expected_read_holding(1U)));
  std::array<ParserEvent, 4U> events{};
  const auto result =
      parser.ingest(input.data(), input.size(), at(100U), events.data(), events.size());
  ASSERT_GE(result.events_written, 2U);
  ASSERT_TRUE(std::holds_alternative<ParserError>(events[0].payload));
  EXPECT_EQ(std::get<ParserError>(events[0].payload).category, ParserErrorCategory::frame_too_long);
  EXPECT_TRUE(
      std::holds_alternative<ParsedResponseEvent>(events[result.events_written - 1U].payload));
}

TEST(RtuStreamParserResponseTest, RequiresActiveRequestContext) {
  const auto response = parse_hex("01 03 02 00 0A 38 43");
  RtuStreamParser parser(kTiming);
  ASSERT_TRUE(parser.begin_response_stream(52U, expected_read_holding(1U)));
  parser.clear_request_context();
  std::array<ParserEvent, 16U> events{};
  const auto result =
      parser.ingest(response.data(), response.size(), at(100U), events.data(), events.size());
  ASSERT_GT(result.events_written, 0U);
  ASSERT_TRUE(std::holds_alternative<ParserError>(events[0].payload));
  EXPECT_EQ(std::get<ParserError>(events[0].payload).category,
            ParserErrorCategory::no_request_context);
  EXPECT_EQ(result.bytes_consumed, response.size());
}

TEST(RtuStreamParserResponseTest, ParsesMaximumResponseByteByByte) {
  ReadResponse response{};
  response.slave_id = 1U;
  response.function = FunctionCode::read_holding_registers;
  response.value_count = 125U;
  for (std::size_t index = 0U; index < response.value_count; ++index) {
    response.values[index] = static_cast<std::uint16_t>(index);
  }
  const auto encoded = encode_response(Response{response});
  ASSERT_TRUE(std::holds_alternative<industrial_iot_gateway::protocol::Adu>(encoded));
  const auto &adu = std::get<industrial_iot_gateway::protocol::Adu>(encoded);
  ASSERT_EQ(adu.size, 255U);

  RtuStreamParser parser(kTiming);
  ASSERT_TRUE(parser.begin_response_stream(53U, expected_read_holding(125U)));
  std::array<ParserEvent, 2U> events{};
  std::size_t event_count = 0U;
  for (std::size_t index = 0U; index < adu.size; ++index) {
    const auto result = parser.ingest(&adu.bytes[index], 1U, at(100U + index),
                                      events.data() + event_count, events.size() - event_count);
    event_count += result.events_written;
  }
  EXPECT_EQ(event_count, 1U);
  EXPECT_EQ(count_response_events(events.data(), event_count), 1U);
  EXPECT_LE(parser.maximum_buffered_size(), 255U);
}

TEST(RtuStreamParserTimingTest, ReportsInterCharacterAndFrameBoundaryErrors) {
  const auto partial = load_corpus("truncated_response");
  {
    RtuStreamParser parser(kTiming);
    ASSERT_TRUE(parser.begin_response_stream(60U, expected_read_holding()));
    std::array<ParserEvent, 2U> events{};
    const auto input = parser.ingest(partial.data(), partial.size(), at(100U), events.data(), 2U);
    EXPECT_EQ(input.events_written, 0U);
    const auto timeout = parser.on_time_advanced(at(1'200U), events.data(), 2U);
    ASSERT_EQ(timeout.events_written, 1U);
    EXPECT_EQ(std::get<ParserError>(events[0].payload).category,
              ParserErrorCategory::inter_character_timeout);
  }

  {
    RtuStreamParser parser(kTiming);
    ASSERT_TRUE(parser.begin_response_stream(61U, expected_read_holding()));
    std::array<ParserEvent, 2U> events{};
    static_cast<void>(parser.ingest(partial.data(), partial.size(), at(100U), events.data(), 2U));
    const auto boundary = parser.on_time_advanced(at(3'200U), events.data(), 2U);
    ASSERT_EQ(boundary.events_written, 1U);
    EXPECT_EQ(std::get<ParserError>(events[0].payload).category,
              ParserErrorCategory::frame_too_short);
    EXPECT_EQ(parser.buffered_size(), 0U);
  }
}

TEST(RtuStreamParserTimingTest, RejectsOldPartialBeforeConsumingNewFrame) {
  const auto partial = parse_hex("01 03 04");
  const auto valid = parse_hex("01 03 04 00 0A 00 14 DA 3E");
  RtuStreamParser parser(kTiming);
  ASSERT_TRUE(parser.begin_response_stream(62U, expected_read_holding()));
  std::array<ParserEvent, 4U> events{};
  const auto first =
      parser.ingest(partial.data(), partial.size(), at(100U), events.data(), events.size());
  EXPECT_EQ(first.events_written, 0U);
  const auto second =
      parser.ingest(valid.data(), valid.size(), at(1'200U), events.data(), events.size());
  ASSERT_EQ(second.events_written, 2U);
  EXPECT_EQ(std::get<ParserError>(events[0].payload).category,
            ParserErrorCategory::inter_character_timeout);
  EXPECT_TRUE(std::holds_alternative<ParsedResponseEvent>(events[1].payload));
}

TEST(RtuStreamParserBoundednessTest, ConsumesLargeNoiseWithoutGrowthOrLooping) {
  std::vector<std::uint8_t> noise(4'096U, 0xAAU);
  RtuStreamParser parser(kTiming);
  parser.begin_request_stream();
  std::array<ParserEvent, 4U> events{};
  const auto result =
      parser.ingest(noise.data(), noise.size(), at(100U), events.data(), events.size());
  EXPECT_EQ(result.bytes_consumed, noise.size());
  EXPECT_LE(parser.maximum_buffered_size(), kRtuParserBufferCapacity);
  EXPECT_LE(parser.buffered_size(), 1U);
}

} // namespace
