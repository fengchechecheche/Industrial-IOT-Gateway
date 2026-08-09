#include "industrial_iot_gateway/observability/structured_log.hpp"
#include "industrial_iot_gateway/publish/jsonl_publish_sink.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <sstream>
#include <string>

namespace {

using industrial_iot_gateway::concurrency::QueuePushStatus;
using industrial_iot_gateway::observability::JsonlLogWriter;
using industrial_iot_gateway::pipeline::PublishMessage;
using industrial_iot_gateway::pipeline::TelemetryEvent;
using industrial_iot_gateway::publish::JsonlPublishSink;
using industrial_iot_gateway::quality::RegisterQuality;
using namespace std::chrono_literals;

TEST(JsonlPublishSinkTest, OwnsQueueWorkerAndDrainsAcceptedMessages) {
  std::ostringstream output{};
  auto logger = std::make_shared<JsonlLogWriter>(output);
  JsonlPublishSink sink(logger, {4U, 3U});
  ASSERT_TRUE(sink.valid());
  ASSERT_TRUE(sink.start());

  TelemetryEvent telemetry{
      1U,    1U,  "gateway/devices/1/registers/temperature", RegisterQuality::fresh, "valid_sample",
      false, 25.0};
  EXPECT_EQ(sink.submit(PublishMessage{telemetry}), QueuePushStatus::accepted);
  sink.request_stop(std::chrono::steady_clock::now() + 100ms);
  sink.join();

  EXPECT_NE(output.str().find("\"event\":\"telemetry\""), std::string::npos);
  EXPECT_EQ(sink.statistics().queue.accepted, 1U);
  EXPECT_EQ(sink.statistics().queue.popped, 1U);
}

TEST(JsonlPublishSinkTest, RejectsSubmissionAfterStopWithoutRestarting) {
  std::ostringstream output{};
  auto logger = std::make_shared<JsonlLogWriter>(output);
  JsonlPublishSink sink(logger, {4U, 3U});
  ASSERT_TRUE(sink.start());
  sink.request_stop(std::chrono::steady_clock::now() + 10ms);
  sink.join();

  TelemetryEvent telemetry{};
  EXPECT_EQ(sink.submit(PublishMessage{telemetry}), QueuePushStatus::closed);
  EXPECT_FALSE(sink.start());
}

} // namespace
