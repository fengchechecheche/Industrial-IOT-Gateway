#include "industrial_iot_gateway/pipeline/event_pipeline.hpp"

#include <chrono>
#include <optional>
#include <variant>

#include <gtest/gtest.h>

namespace industrial_iot_gateway::pipeline {
namespace {

using namespace std::chrono_literals;

template <typename T> [[nodiscard]] T require_value(const std::optional<T> &value) {
  if (!value.has_value()) {
    ADD_FAILURE() << "expected an optional value";
  }
  return value.value_or(T{});
}

[[nodiscard]] MeasurementRaw raw(std::uint64_t request_id, std::uint16_t value) {
  return MeasurementRaw{request_id, 1U,      protocol::FunctionCode::read_input_registers,
                        10U,        {value}, scheduler::SchedulerTimePoint{} + 10ms};
}

[[nodiscard]] QualityTransitionEvent quality_event(std::uint32_t job_id) {
  return QualityTransitionEvent{quality::QualityTransition{
      job_id, 1U, quality::RegisterQuality::fresh, quality::RegisterQuality::stale,
      scheduler::SchedulerTimePoint{} + 20ms, true}};
}

TEST(MeasurementQueueTest, UsesFrozenDefaults) {
  MeasurementQueue queue{};
  ASSERT_TRUE(queue.valid());
  EXPECT_EQ(queue.statistics().capacity, 512U);
  EXPECT_EQ(queue.statistics().high_watermark, 410U);
}

TEST(MeasurementQueueTest, CoalescesLatestRawMeasurementByRegisterKey) {
  MeasurementQueue queue({3U, 2U});
  EXPECT_EQ(queue.push(MeasurementMessage{raw(1U, 10U)}), concurrency::QueuePushStatus::accepted);
  EXPECT_EQ(queue.push(MeasurementMessage{raw(2U, 20U)}), concurrency::QueuePushStatus::coalesced);
  EXPECT_EQ(queue.statistics().current_depth, 1U);
  const auto popped = queue.try_pop();
  ASSERT_TRUE(popped.value.has_value());
  const auto message = require_value(popped.value);
  const auto *measurement = std::get_if<MeasurementRaw>(&message);
  ASSERT_NE(measurement, nullptr);
  EXPECT_EQ(measurement->request_id, 2U);
  EXPECT_EQ(measurement->registers.front(), 20U);
}

TEST(MeasurementQueueTest, CriticalQualityTransitionIsNeverCoalesced) {
  MeasurementQueue queue({2U, 1U});
  EXPECT_EQ(queue.push(MeasurementMessage{quality_event(1U)}),
            concurrency::QueuePushStatus::accepted);
  EXPECT_EQ(queue.push(MeasurementMessage{quality_event(1U)}),
            concurrency::QueuePushStatus::accepted);
  EXPECT_EQ(queue.push(MeasurementMessage{quality_event(1U)}), concurrency::QueuePushStatus::full);
  EXPECT_EQ(queue.statistics().current_depth, 2U);
}

TEST(PublishQueueTest, UsesFrozenDefaults) {
  PublishQueue queue{};
  ASSERT_TRUE(queue.valid());
  EXPECT_EQ(queue.statistics().capacity, 1024U);
  EXPECT_EQ(queue.statistics().high_watermark, 820U);
}

TEST(PublishQueueTest, CoalescesOrdinaryTelemetryByTopic) {
  PublishQueue queue({3U, 2U});
  TelemetryEvent first{
      1U,    1U,  "gateway/devices/1/temperature", quality::RegisterQuality::fresh, "sample",
      false, 10.0};
  TelemetryEvent second{2U,       2U,    first.topic, quality::RegisterQuality::fresh,
                        "sample", false, 11.0};
  EXPECT_EQ(queue.push(PublishMessage{first}), concurrency::QueuePushStatus::accepted);
  EXPECT_EQ(queue.push(PublishMessage{second}), concurrency::QueuePushStatus::coalesced);
  const auto popped = queue.try_pop();
  ASSERT_TRUE(popped.value.has_value());
  const auto message = require_value(popped.value);
  const auto *telemetry = std::get_if<TelemetryEvent>(&message);
  ASSERT_NE(telemetry, nullptr);
  EXPECT_EQ(telemetry->event_id, 2U);
  EXPECT_EQ(telemetry->value, 11.0);
}

TEST(PublishQueueTest, QualityAndWriteAuditEventsAreNeverSilentlyDropped) {
  PublishQueue queue({2U, 1U});
  WriteAuditEvent audit{1U, 10U, 1U, 20U, scheduler::RequestResultCategory::success};
  EXPECT_EQ(queue.push(PublishMessage{quality_event(1U)}), concurrency::QueuePushStatus::accepted);
  EXPECT_EQ(queue.push(PublishMessage{audit}), concurrency::QueuePushStatus::accepted);
  EXPECT_EQ(queue.push(PublishMessage{quality_event(2U)}), concurrency::QueuePushStatus::full);
  EXPECT_EQ(queue.statistics().full, 1U);
}

} // namespace
} // namespace industrial_iot_gateway::pipeline
