#include "industrial_iot_gateway/pipeline/request_queue.hpp"

#include <chrono>
#include <cstdint>
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

[[nodiscard]] scheduler::SchedulerTimePoint at(std::chrono::milliseconds offset) {
  return scheduler::SchedulerTimePoint{} + offset;
}

[[nodiscard]] scheduler::ScheduledRequest
make_request(std::uint64_t request_id, scheduler::RequestKind kind, std::uint32_t poll_job_id,
             scheduler::SchedulerTimePoint due = at(0ms)) {
  scheduler::ScheduledRequest request{};
  request.request_id = request_id;
  request.poll_job_id = poll_job_id;
  request.request_kind = kind;
  request.request = protocol::ReadRequest{1U, protocol::FunctionCode::read_input_registers,
                                          static_cast<std::uint16_t>(poll_job_id), 1U};
  request.due_at = due;
  request.created_at = at(0ms);
  request.enqueued_at = at(0ms);
  request.deadline = at(5000ms);
  request.attempt_eligible_at = due;
  request.source = kind == scheduler::RequestKind::poll_read
                       ? scheduler::RequestSource::poll_scheduler
                       : scheduler::RequestSource::explicit_control_api;
  return request;
}

TEST(RequestQueueValidationTest, RejectsInvalidFairnessLimit) {
  RequestQueueConfig config{};
  config.max_consecutive_explicit_requests = 0U;
  RequestQueue queue(config);
  EXPECT_FALSE(queue.valid());
  EXPECT_EQ(queue.validation_error(), concurrency::QueueErrorCategory::invalid_fairness);
}

TEST(RequestQueueTest, UsesFrozenDefaults) {
  RequestQueue queue{};
  ASSERT_TRUE(queue.valid());
  const auto statistics = queue.statistics();
  EXPECT_EQ(statistics.capacity, 256U);
  EXPECT_EQ(statistics.high_watermark, 205U);
}

TEST(RequestQueueTest, CoalescesOnlyDuplicatePollJobs) {
  RequestQueue queue({{4U, 3U}, 8U});
  EXPECT_EQ(queue.push(make_request(1U, scheduler::RequestKind::poll_read, 7U)),
            concurrency::QueuePushStatus::accepted);
  EXPECT_EQ(queue.push(make_request(2U, scheduler::RequestKind::poll_read, 7U)),
            concurrency::QueuePushStatus::coalesced);
  EXPECT_EQ(queue.statistics().current_depth, 1U);
  const auto popped = queue.try_pop(at(0ms));
  ASSERT_TRUE(popped.value.has_value());
  EXPECT_EQ(require_value(popped.value).request_id, 2U);
}

TEST(RequestQueueTest, ExplicitWriteIsNeverOverwrittenAndFullIsVisible) {
  RequestQueue queue({{2U, 1U}, 8U});
  EXPECT_EQ(queue.push(make_request(1U, scheduler::RequestKind::explicit_write, 0U)),
            concurrency::QueuePushStatus::accepted);
  EXPECT_EQ(queue.push(make_request(2U, scheduler::RequestKind::explicit_write, 0U)),
            concurrency::QueuePushStatus::accepted);
  EXPECT_EQ(queue.push(make_request(3U, scheduler::RequestKind::explicit_write, 0U)),
            concurrency::QueuePushStatus::full);
  EXPECT_EQ(queue.statistics().full, 1U);
}

TEST(RequestQueueTest, YieldsToDuePollAfterEightExplicitRequests) {
  RequestQueue queue({{12U, 9U}, 8U});
  for (std::uint64_t id = 1U; id <= 9U; ++id) {
    ASSERT_EQ(queue.push(make_request(id, scheduler::RequestKind::explicit_read, 0U)),
              concurrency::QueuePushStatus::accepted);
  }
  ASSERT_EQ(queue.push(make_request(100U, scheduler::RequestKind::poll_read, 1U)),
            concurrency::QueuePushStatus::accepted);

  for (std::uint64_t id = 1U; id <= 8U; ++id) {
    const auto request = queue.try_pop(at(0ms));
    ASSERT_TRUE(request.value.has_value());
    EXPECT_EQ(require_value(request.value).request_id, id);
  }
  const auto yielded = queue.try_pop(at(0ms));
  ASSERT_TRUE(yielded.value.has_value());
  EXPECT_EQ(require_value(yielded.value).request_kind, scheduler::RequestKind::poll_read);
  EXPECT_EQ(queue.consecutive_explicit_requests(), 0U);

  const auto remaining = queue.try_pop(at(0ms));
  ASSERT_TRUE(remaining.value.has_value());
  EXPECT_EQ(require_value(remaining.value).request_id, 9U);
}

TEST(RequestQueueTest, DoesNotIdleForFuturePollAtFairnessBoundary) {
  RequestQueue queue({{12U, 9U}, 8U});
  for (std::uint64_t id = 1U; id <= 9U; ++id) {
    ASSERT_EQ(queue.push(make_request(id, scheduler::RequestKind::explicit_read, 0U)),
              concurrency::QueuePushStatus::accepted);
  }
  ASSERT_EQ(queue.push(make_request(100U, scheduler::RequestKind::poll_read, 1U, at(100ms))),
            concurrency::QueuePushStatus::accepted);
  for (std::uint64_t id = 1U; id <= 9U; ++id) {
    const auto request = queue.try_pop(at(0ms));
    ASSERT_TRUE(request.value.has_value());
    EXPECT_EQ(require_value(request.value).request_id, id);
  }
  EXPECT_EQ(queue.try_pop(at(99ms)).status, concurrency::QueuePopStatus::empty);
  EXPECT_EQ(require_value(queue.try_pop(at(100ms)).value).request_id, 100U);
}

TEST(RequestQueueTest, CloseRejectsPushAndAllowsAcceptedItemsToDrain) {
  RequestQueue queue({{2U, 1U}, 8U});
  ASSERT_EQ(queue.push(make_request(1U, scheduler::RequestKind::explicit_read, 0U)),
            concurrency::QueuePushStatus::accepted);
  queue.close();
  EXPECT_EQ(queue.push(make_request(2U, scheduler::RequestKind::explicit_read, 0U)),
            concurrency::QueuePushStatus::closed);
  EXPECT_EQ(queue.try_pop(at(0ms)).status, concurrency::QueuePopStatus::item);
  EXPECT_EQ(queue.try_pop(at(0ms)).status, concurrency::QueuePopStatus::closed);
}

} // namespace
} // namespace industrial_iot_gateway::pipeline
