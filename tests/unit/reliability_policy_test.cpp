#include "industrial_iot_gateway/scheduler/reliability_policy.hpp"

#include <chrono>

#include <gtest/gtest.h>

namespace industrial_iot_gateway::scheduler {
namespace {

using namespace std::chrono_literals;

[[nodiscard]] SchedulerTimePoint at(const std::chrono::milliseconds offset) {
  return SchedulerTimePoint{} + offset;
}

TEST(ReliabilityPolicyTest, UsesFrozenDefaultsAndValidatesWorstCaseBudget) {
  const ReliabilityPolicy policy{};
  ASSERT_TRUE(policy.valid());
  EXPECT_EQ(policy.config().response_timeout, 500ms);
  EXPECT_EQ(policy.config().max_attempts, 3U);
  EXPECT_EQ(policy.config().backoff_initial, 200ms);
  EXPECT_DOUBLE_EQ(policy.config().backoff_multiplier, 2.0);
  EXPECT_EQ(policy.config().backoff_max, 2000ms);
  EXPECT_EQ(policy.config().request_deadline, 5000ms);
  EXPECT_EQ(policy.request_deadline(at(100ms)), at(5100ms));
}

TEST(ReliabilityPolicyTest, RejectsInvalidConfiguration) {
  ReliabilityConfig config{};
  config.response_timeout = 0ms;
  EXPECT_EQ(ReliabilityPolicy(config).validation_error().category,
            ReliabilityConfigErrorCategory::invalid_response_timeout);

  config = ReliabilityConfig{};
  config.max_attempts = 0U;
  EXPECT_EQ(ReliabilityPolicy(config).validation_error().category,
            ReliabilityConfigErrorCategory::invalid_max_attempts);
  config.max_attempts = 6U;
  EXPECT_EQ(ReliabilityPolicy(config).validation_error().category,
            ReliabilityConfigErrorCategory::invalid_max_attempts);

  config = ReliabilityConfig{};
  config.backoff_initial = 2100ms;
  EXPECT_EQ(ReliabilityPolicy(config).validation_error().category,
            ReliabilityConfigErrorCategory::invalid_backoff);

  config = ReliabilityConfig{};
  config.backoff_multiplier = 0.5;
  EXPECT_EQ(ReliabilityPolicy(config).validation_error().category,
            ReliabilityConfigErrorCategory::invalid_multiplier);

  config = ReliabilityConfig{};
  config.request_deadline = 500ms;
  EXPECT_EQ(ReliabilityPolicy(config).validation_error().category,
            ReliabilityConfigErrorCategory::invalid_deadline);

  config = ReliabilityConfig{};
  config.request_deadline = 2000ms;
  EXPECT_EQ(ReliabilityPolicy(config).validation_error().category,
            ReliabilityConfigErrorCategory::insufficient_deadline_budget);
}

TEST(ReliabilityPolicyTest, ClassifiesRetryableAndTerminalResults) {
  const ReliabilityPolicy policy{};
  EXPECT_TRUE(policy.is_retryable(RequestResultCategory::response_timeout));
  EXPECT_TRUE(policy.is_retryable(RequestResultCategory::crc_mismatch));
  EXPECT_TRUE(policy.is_retryable(RequestResultCategory::truncated_frame));
  EXPECT_TRUE(policy.is_retryable(RequestResultCategory::serial_io_transient));
  EXPECT_FALSE(policy.is_retryable(RequestResultCategory::success));
  EXPECT_FALSE(policy.is_retryable(RequestResultCategory::remote_exception));
  EXPECT_FALSE(policy.is_retryable(RequestResultCategory::invalid_configuration));
  EXPECT_FALSE(policy.is_retryable(RequestResultCategory::deadline_exceeded));
  EXPECT_FALSE(policy.is_retryable(RequestResultCategory::shutdown_cancelled));
}

TEST(ReliabilityPolicyTest, ComputesBackoffAndHonorsMaximum) {
  const ReliabilityPolicy policy{};
  EXPECT_EQ(policy.backoff_after_failed_attempt(1U), 200ms);
  EXPECT_EQ(policy.backoff_after_failed_attempt(2U), 400ms);

  ReliabilityConfig capped_config{};
  capped_config.max_attempts = 5U;
  capped_config.backoff_initial = 1500ms;
  capped_config.backoff_multiplier = 2.0;
  capped_config.backoff_max = 2000ms;
  capped_config.request_deadline = 12000ms;
  const ReliabilityPolicy capped(capped_config);
  ASSERT_TRUE(capped.valid());
  EXPECT_EQ(capped.backoff_after_failed_attempt(1U), 1500ms);
  EXPECT_EQ(capped.backoff_after_failed_attempt(2U), 2000ms);
  EXPECT_EQ(capped.backoff_after_failed_attempt(4U), 2000ms);
}

TEST(ReliabilityPolicyTest, RetriesWithSameBusinessRequestAttemptSequence) {
  const ReliabilityPolicy policy{};
  const auto first =
      policy.evaluate(RequestResultCategory::response_timeout, 1U, at(1000ms), at(5000ms));
  EXPECT_EQ(first.disposition, ReliabilityDisposition::retry_wait);
  EXPECT_EQ(first.next_state, RequestState::retry_wait);
  EXPECT_EQ(first.next_attempt, 2U);
  EXPECT_EQ(first.retry_at, at(1200ms));

  const auto second =
      policy.evaluate(RequestResultCategory::crc_mismatch, 2U, at(1500ms), at(5000ms));
  EXPECT_EQ(second.disposition, ReliabilityDisposition::retry_wait);
  EXPECT_EQ(second.next_attempt, 3U);
  EXPECT_EQ(second.retry_at, at(1900ms));

  const auto third =
      policy.evaluate(RequestResultCategory::truncated_frame, 3U, at(2200ms), at(5000ms));
  EXPECT_EQ(third.disposition, ReliabilityDisposition::failed);
  EXPECT_EQ(third.next_state, RequestState::failed);
  EXPECT_TRUE(third.retry_exhausted);
  EXPECT_FALSE(third.retry_at.has_value());
}

TEST(ReliabilityPolicyTest, DeadlineTruncatesWaitingAndRetry) {
  const ReliabilityPolicy policy{};
  EXPECT_EQ(policy.attempt_deadline(at(1000ms), at(5000ms)), at(1500ms));
  EXPECT_EQ(policy.attempt_deadline(at(4800ms), at(5000ms)), at(5000ms));

  const auto decision =
      policy.evaluate(RequestResultCategory::response_timeout, 1U, at(4900ms), at(5000ms));
  EXPECT_EQ(decision.disposition, ReliabilityDisposition::failed);
  EXPECT_EQ(decision.result, RequestResultCategory::deadline_exceeded);
  EXPECT_FALSE(decision.retry_at.has_value());
}

TEST(ReliabilityPolicyTest, CompletesSuccessAndRemoteExceptionWithoutRetry) {
  const ReliabilityPolicy policy{};
  const auto success = policy.evaluate(RequestResultCategory::success, 1U, at(100ms), at(5000ms));
  EXPECT_EQ(success.disposition, ReliabilityDisposition::completed);
  EXPECT_EQ(success.next_state, RequestState::completed);

  const auto remote =
      policy.evaluate(RequestResultCategory::remote_exception, 1U, at(100ms), at(5000ms));
  EXPECT_EQ(remote.disposition, ReliabilityDisposition::completed);
  EXPECT_EQ(remote.next_state, RequestState::completed);

  const auto cancelled =
      policy.evaluate(RequestResultCategory::shutdown_cancelled, 1U, at(100ms), at(5000ms));
  EXPECT_EQ(cancelled.disposition, ReliabilityDisposition::cancelled);
  EXPECT_EQ(cancelled.next_state, RequestState::cancelled);
}

} // namespace
} // namespace industrial_iot_gateway::scheduler
