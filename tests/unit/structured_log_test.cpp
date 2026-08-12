#include "industrial_iot_gateway/observability/structured_log.hpp"

#include <algorithm>
#include <cstdint>
#include <ios>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace industrial_iot_gateway::observability {
namespace {

TEST(StructuredLogTest, FormatsRequiredRequestTransitionFieldsAsOneJsonLine) {
  StructuredEvent event{};
  event.event = "request_transition";
  event.component = "scheduler";
  event.request_id = 42U;
  event.attempt = 2U;
  event.slave_id = 3U;
  event.function = 4U;
  event.address = 100U;
  event.deadline_ms = 5000;
  event.monotonic_ms = 12345U;
  event.duration_ms = 27U;
  event.previous_state = "waiting_response";
  event.next_state = "retry_wait";
  event.result = "response_timeout";
  event.reason = "attempt timed out";
  const auto line = format_jsonl(event);
  EXPECT_FALSE(line.empty());
  EXPECT_EQ(line.back(), '\n');
  EXPECT_NE(line.find("\"request_id\":42"), std::string::npos);
  EXPECT_NE(line.find("\"attempt\":2"), std::string::npos);
  EXPECT_NE(line.find("\"previous_state\":\"waiting_response\""), std::string::npos);
  EXPECT_NE(line.find("\"result\":\"response_timeout\""), std::string::npos);
  EXPECT_NE(line.find("\"monotonic_ms\":12345"), std::string::npos);
  EXPECT_NE(line.find("\"duration_ms\":27"), std::string::npos);
}

TEST(StructuredLogTest, EscapesQuotesBackslashesAndControlCharacters) {
  StructuredEvent event{};
  event.event = "queue\"full";
  event.component = "pipeline\\request";
  event.reason = "line1\nline2\tend";
  const auto line = format_jsonl(event);
  EXPECT_NE(line.find("queue\\\"full"), std::string::npos);
  EXPECT_NE(line.find("pipeline\\\\request"), std::string::npos);
  EXPECT_NE(line.find("line1\\nline2\\tend"), std::string::npos);
  EXPECT_EQ(line.find('\n'), line.size() - 1U);
}

TEST(StructuredLogTest, OmitsAbsentOptionalFields) {
  StructuredEvent event{};
  event.event = "shutdown_started";
  event.component = "lifecycle";
  const auto line = format_jsonl(event);
  EXPECT_EQ(line.find("request_id"), std::string::npos);
  EXPECT_EQ(line.find("slave_id"), std::string::npos);
}

TEST(JsonlLogWriterTest, WritesConcurrentEventsAsCompleteLines) {
  std::ostringstream output{};
  JsonlLogWriter writer(output);
  std::vector<std::thread> threads{};
  for (std::uint64_t id = 1U; id <= 8U; ++id) {
    threads.emplace_back([id, &writer] {
      StructuredEvent event{};
      event.event = "queue_event";
      event.component = "test";
      event.request_id = id;
      EXPECT_TRUE(writer.write(event));
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }
  const auto text = output.str();
  EXPECT_EQ(static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n')), 8U);
  EXPECT_EQ(writer.statistics().written, 8U);
  EXPECT_EQ(writer.statistics().failures, 0U);
}

TEST(JsonlLogWriterTest, ReportsOutputFailureWithoutRecursiveLogging) {
  std::ostringstream output{};
  output.setstate(std::ios::badbit);
  JsonlLogWriter writer(output);
  StructuredEvent event{};
  event.event = "failure";
  event.component = "test";
  EXPECT_FALSE(writer.write(event));
  EXPECT_EQ(writer.statistics().failures, 1U);
}

} // namespace
} // namespace industrial_iot_gateway::observability
