#include <chrono>
#include <future>

#include <gtest/gtest.h>

#include "industrial_iot_gateway/pipeline/event_pipeline.hpp"
#include "industrial_iot_gateway/pipeline/request_queue.hpp"

namespace industrial_iot_gateway::pipeline {
namespace {

using namespace std::chrono_literals;

TEST(RequestQueueWaitTest, PushWakesConsumerAndReturnsEligibleRequest) {
  RequestQueue queue;
  ASSERT_TRUE(queue.valid());
  const auto now = scheduler::SchedulerClock::now();

  auto pending = std::async(std::launch::async, [&queue] {
    return queue.wait_pop_until(scheduler::SchedulerClock::now() + 1s);
  });

  scheduler::ScheduledRequest request{};
  request.request_id = 42U;
  request.poll_job_id = 7U;
  request.due_at = now;
  request.attempt_eligible_at = now;
  ASSERT_EQ(queue.push(request), concurrency::QueuePushStatus::accepted);

  const auto result = pending.get();
  ASSERT_EQ(result.status, concurrency::QueuePopStatus::item);
  ASSERT_TRUE(result.value.has_value());
  EXPECT_EQ(result.value.value_or(scheduler::ScheduledRequest{}).request_id, 42U);
}

TEST(MeasurementQueueWaitTest, CloseWakesBlockedConsumer) {
  MeasurementQueue queue;
  ASSERT_TRUE(queue.valid());

  auto pending = std::async(std::launch::async, [&queue] {
    return queue.wait_pop_until(std::chrono::steady_clock::now() + 1s);
  });
  queue.close();

  const auto result = pending.get();
  EXPECT_EQ(result.status, concurrency::QueuePopStatus::closed);
}

TEST(PublishQueueWaitTest, TimesOutWithFiniteDeadline) {
  PublishQueue queue;
  ASSERT_TRUE(queue.valid());

  const auto result = queue.wait_pop_until(std::chrono::steady_clock::now() + 5ms);
  EXPECT_EQ(result.status, concurrency::QueuePopStatus::timed_out);
}

} // namespace
} // namespace industrial_iot_gateway::pipeline
