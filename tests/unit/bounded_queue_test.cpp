#include "industrial_iot_gateway/concurrency/bounded_queue.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <optional>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace industrial_iot_gateway::concurrency {
namespace {

using namespace std::chrono_literals;

template <typename T> [[nodiscard]] T require_value(const std::optional<T> &value) {
  if (!value.has_value()) {
    ADD_FAILURE() << "expected an optional value";
  }
  return value.value_or(T{});
}

TEST(BoundedQueueValidationTest, RejectsInvalidConfiguration) {
  BoundedQueue<int> empty({0U, 0U});
  EXPECT_FALSE(empty.valid());
  EXPECT_EQ(empty.validation_error(), QueueErrorCategory::invalid_capacity);

  BoundedQueue<int> bad_watermark({4U, 4U});
  EXPECT_FALSE(bad_watermark.valid());
  EXPECT_EQ(bad_watermark.validation_error(), QueueErrorCategory::invalid_high_watermark);
}

TEST(BoundedQueueTest, EnforcesCapacityAndTracksStatistics) {
  BoundedQueue<int> queue({2U, 1U});
  ASSERT_TRUE(queue.valid());
  EXPECT_EQ(queue.try_push(1), QueuePushStatus::accepted);
  EXPECT_EQ(queue.try_push(2), QueuePushStatus::accepted);
  EXPECT_EQ(queue.try_push(3), QueuePushStatus::full);
  const auto statistics = queue.statistics();
  EXPECT_EQ(statistics.current_depth, 2U);
  EXPECT_EQ(statistics.maximum_depth, 2U);
  EXPECT_EQ(statistics.high_watermark_crossings, 1U);
  EXPECT_EQ(statistics.full, 1U);
}

TEST(BoundedQueueTest, PreservesFifoOrder) {
  BoundedQueue<int> queue({3U, 2U});
  ASSERT_EQ(queue.try_push(10), QueuePushStatus::accepted);
  ASSERT_EQ(queue.try_push(20), QueuePushStatus::accepted);
  const auto first = queue.try_pop();
  const auto second = queue.try_pop();
  ASSERT_TRUE(first.value.has_value());
  ASSERT_TRUE(second.value.has_value());
  EXPECT_EQ(require_value(first.value), 10);
  EXPECT_EQ(require_value(second.value), 20);
  EXPECT_EQ(queue.try_pop().status, QueuePopStatus::empty);
}

TEST(BoundedQueueTest, ReplacesMatchingPendingValueWithoutGrowing) {
  BoundedQueue<int> queue({2U, 1U});
  ASSERT_EQ(queue.try_push(10), QueuePushStatus::accepted);
  const auto status = queue.try_push_or_replace(12, [](int existing) { return existing == 10; });
  EXPECT_EQ(status, QueuePushStatus::coalesced);
  EXPECT_EQ(queue.statistics().current_depth, 1U);
  const auto value = queue.try_pop();
  ASSERT_TRUE(value.value.has_value());
  EXPECT_EQ(require_value(value.value), 12);
}

TEST(BoundedQueueTest, CloseWakesBlockedConsumerAndRejectsNewPush) {
  BoundedQueue<int> queue({2U, 1U});
  auto waiting = std::async(std::launch::async, [&queue] {
    return queue.wait_pop_until(std::chrono::steady_clock::now() + 5s).status;
  });
  std::this_thread::sleep_for(10ms);
  queue.close();
  EXPECT_EQ(waiting.get(), QueuePopStatus::closed);
  EXPECT_EQ(queue.try_push(1), QueuePushStatus::closed);
  EXPECT_TRUE(queue.closed());
}

TEST(BoundedQueueTest, FiniteProducerAndConsumerWaitsTimeout) {
  BoundedQueue<int> queue({2U, 1U});
  ASSERT_TRUE(queue.valid());
  ASSERT_EQ(queue.try_push(1), QueuePushStatus::accepted);
  ASSERT_EQ(queue.try_push(2), QueuePushStatus::accepted);
  EXPECT_EQ(queue.push_until(3, std::chrono::steady_clock::now() + 2ms),
            QueuePushStatus::timed_out);

  BoundedQueue<int> empty({2U, 1U});
  EXPECT_EQ(empty.wait_pop_until(std::chrono::steady_clock::now() + 2ms).status,
            QueuePopStatus::timed_out);
}

TEST(BoundedQueueTest, SupportsConcurrentProducersAndConsumerWithoutLoss) {
  BoundedQueue<int> queue({32U, 24U});
  constexpr int producer_count = 4;
  constexpr int values_per_producer = 100;
  std::atomic<int> consumed{};

  std::thread consumer([&queue, &consumed] {
    while (consumed.load() < producer_count * values_per_producer) {
      const auto result =
          queue.wait_pop_until(std::chrono::steady_clock::now() + std::chrono::seconds{2});
      if (result.status != QueuePopStatus::item) {
        return;
      }
      ++consumed;
    }
  });

  std::vector<std::thread> producers{};
  producers.reserve(producer_count);
  for (int producer = 0; producer < producer_count; ++producer) {
    producers.emplace_back([producer, &queue] {
      for (int value = 0; value < values_per_producer; ++value) {
        const auto status =
            queue.push_until(producer * values_per_producer + value,
                             std::chrono::steady_clock::now() + std::chrono::seconds{2});
        EXPECT_EQ(status, QueuePushStatus::accepted);
      }
    });
  }
  for (auto &producer : producers) {
    producer.join();
  }
  consumer.join();
  EXPECT_EQ(consumed.load(), producer_count * values_per_producer);
  EXPECT_LE(queue.statistics().maximum_depth, 32U);
}

} // namespace
} // namespace industrial_iot_gateway::concurrency
