#include "industrial_iot_gateway/quality/freshness_tracker.hpp"

#include <chrono>
#include <optional>
#include <vector>

#include <gtest/gtest.h>

namespace industrial_iot_gateway::quality {
namespace {

using namespace std::chrono_literals;

[[nodiscard]] scheduler::SchedulerTimePoint at(const std::chrono::milliseconds offset) {
  return scheduler::SchedulerTimePoint{} + offset;
}

[[nodiscard]] RegisterFreshnessConfig
make_config(const std::uint32_t job_id, const std::uint8_t slave_id,
            const std::chrono::milliseconds poll = 200ms,
            const std::chrono::milliseconds freshness = 600ms) {
  return RegisterFreshnessConfig{job_id, slave_id, poll, freshness};
}

TEST(FreshnessTrackerValidationTest, RejectsInvalidConfiguration) {
  EXPECT_EQ(FreshnessTracker({}).validation_error().category, FreshnessErrorCategory::empty_config);
  EXPECT_EQ(
      FreshnessTracker({make_config(1U, 1U), make_config(1U, 2U)}).validation_error().category,
      FreshnessErrorCategory::duplicate_poll_job_id);
  EXPECT_EQ(FreshnessTracker({make_config(1U, 0U)}).validation_error().category,
            FreshnessErrorCategory::invalid_slave_address);
  EXPECT_EQ(FreshnessTracker({make_config(1U, 1U, 0ms, 600ms)}).validation_error().category,
            FreshnessErrorCategory::invalid_poll_period);
  EXPECT_EQ(FreshnessTracker({make_config(1U, 1U, 500ms, 499ms)}).validation_error().category,
            FreshnessErrorCategory::invalid_freshness);
}

TEST(FreshnessTrackerTest, TracksExactFreshnessBoundary) {
  FreshnessTracker tracker({make_config(1U, 2U)});
  ASSERT_TRUE(tracker.valid());
  EXPECT_EQ(tracker.quality(1U), RegisterQuality::no_valid_sample);

  const auto fresh = tracker.record_valid_sample(1U, at(100ms));
  ASSERT_EQ(fresh.error.category, FreshnessErrorCategory::none);
  ASSERT_EQ(fresh.transitions.size(), 1U);
  EXPECT_EQ(fresh.transitions.front().current, RegisterQuality::fresh);
  EXPECT_EQ(tracker.last_valid_time(1U), at(100ms));

  EXPECT_TRUE(tracker.advance_time(at(700ms)).transitions.empty());
  EXPECT_EQ(tracker.quality(1U), RegisterQuality::fresh);
  const auto stale = tracker.advance_time(at(701ms));
  ASSERT_EQ(stale.transitions.size(), 1U);
  EXPECT_EQ(stale.transitions.front().current, RegisterQuality::stale);
  EXPECT_TRUE(stale.transitions.front().value_is_retained);
}

TEST(FreshnessTrackerTest, PollFailureImmediatelyStalesOnlyHistoricalValue) {
  FreshnessTracker tracker({make_config(1U, 1U), make_config(2U, 1U)});
  ASSERT_EQ(tracker.record_poll_failure(1U, at(10ms)).transitions.size(), 0U);
  EXPECT_EQ(tracker.quality(1U), RegisterQuality::no_valid_sample);

  ASSERT_EQ(tracker.record_valid_sample(2U, at(20ms)).transitions.size(), 1U);
  const auto failed = tracker.record_poll_failure(2U, at(30ms));
  ASSERT_EQ(failed.transitions.size(), 1U);
  EXPECT_EQ(tracker.quality(2U), RegisterQuality::stale);
  EXPECT_TRUE(failed.transitions.front().value_is_retained);
}

TEST(FreshnessTrackerTest, InvalidSampleDoesNotBecomeFresh) {
  FreshnessTracker tracker({make_config(1U, 1U)});
  const auto invalid = tracker.record_invalid_sample(1U, at(10ms));
  ASSERT_EQ(invalid.transitions.size(), 1U);
  EXPECT_EQ(tracker.quality(1U), RegisterQuality::invalid);
  EXPECT_FALSE(tracker.last_valid_time(1U).has_value());
}

TEST(FreshnessTrackerTest, OfflineOverridesQualityAndRecoveryKeepsHistoryStale) {
  FreshnessTracker tracker({make_config(1U, 1U), make_config(2U, 1U)});
  ASSERT_EQ(tracker.record_valid_sample(1U, at(10ms)).error.category, FreshnessErrorCategory::none);
  ASSERT_EQ(tracker.record_invalid_sample(2U, at(11ms)).error.category,
            FreshnessErrorCategory::none);

  const auto offline = tracker.set_device_offline(1U, true, at(20ms));
  ASSERT_EQ(offline.transitions.size(), 2U);
  EXPECT_EQ(tracker.quality(1U), RegisterQuality::offline);
  EXPECT_EQ(tracker.quality(2U), RegisterQuality::offline);

  const auto recovered = tracker.set_device_offline(1U, false, at(30ms));
  ASSERT_EQ(recovered.transitions.size(), 2U);
  EXPECT_EQ(tracker.quality(1U), RegisterQuality::stale);
  EXPECT_EQ(tracker.quality(2U), RegisterQuality::invalid);

  ASSERT_EQ(tracker.record_valid_sample(1U, at(40ms)).transitions.size(), 1U);
  EXPECT_EQ(tracker.quality(1U), RegisterQuality::fresh);
}

TEST(FreshnessTrackerTest, RejectsUnknownJobsAndTimeRegression) {
  FreshnessTracker tracker({make_config(1U, 1U)});
  EXPECT_EQ(tracker.record_valid_sample(99U, at(10ms)).error.category,
            FreshnessErrorCategory::unknown_poll_job);
  ASSERT_EQ(tracker.record_valid_sample(1U, at(20ms)).error.category, FreshnessErrorCategory::none);
  EXPECT_EQ(tracker.advance_time(at(19ms)).error.category, FreshnessErrorCategory::time_regression);
  EXPECT_EQ(tracker.quality(1U), RegisterQuality::fresh);
}

TEST(FreshnessTrackerTest, AcceptsFrozenThreeSlaveFreshnessProjection) {
  const std::vector<RegisterFreshnessConfig> configs{
      make_config(1U, 1U, 500ms, 1500ms),   make_config(2U, 1U, 500ms, 1500ms),
      make_config(3U, 1U, 1000ms, 3000ms),  make_config(4U, 1U, 1000ms, 3000ms),
      make_config(5U, 1U, 5000ms, 15000ms), make_config(6U, 2U, 1000ms, 3000ms),
      make_config(7U, 2U, 1000ms, 3000ms),  make_config(8U, 2U, 200ms, 600ms),
      make_config(9U, 2U, 200ms, 600ms),    make_config(10U, 2U, 500ms, 1500ms),
      make_config(11U, 3U, 1000ms, 3000ms), make_config(12U, 3U, 1000ms, 3000ms),
      make_config(13U, 3U, 1000ms, 3000ms), make_config(14U, 3U, 1000ms, 3000ms),
      make_config(15U, 3U, 1000ms, 3000ms), make_config(16U, 3U, 1000ms, 3000ms),
  };
  FreshnessTracker tracker(configs);
  EXPECT_TRUE(tracker.valid());
  EXPECT_EQ(tracker.register_count(), 16U);
}

} // namespace
} // namespace industrial_iot_gateway::quality
