#include "industrial_iot_gateway/lifecycle/shutdown_coordinator.hpp"

#include <chrono>
#include <iterator>

#include <gtest/gtest.h>

namespace industrial_iot_gateway::lifecycle {
namespace {

using namespace std::chrono_literals;

[[nodiscard]] LifecycleTimePoint at(std::chrono::milliseconds offset) {
  return LifecycleTimePoint{} + offset;
}

TEST(ShutdownCoordinatorValidationTest, RejectsInvalidBudgets) {
  ShutdownCoordinator zero_total({0ms, 0ms});
  EXPECT_FALSE(zero_total.valid());
  EXPECT_EQ(zero_total.validation_error(), ShutdownErrorCategory::invalid_graceful_timeout);

  ShutdownCoordinator zero_mqtt({5000ms, 0ms});
  EXPECT_FALSE(zero_mqtt.valid());
  EXPECT_EQ(zero_mqtt.validation_error(), ShutdownErrorCategory::invalid_mqtt_drain_timeout);

  ShutdownCoordinator exceeds({1000ms, 2000ms});
  EXPECT_FALSE(exceeds.valid());
  EXPECT_EQ(exceeds.validation_error(), ShutdownErrorCategory::mqtt_drain_exceeds_graceful_timeout);
}

TEST(ShutdownCoordinatorTest, UsesFrozenDefaultsAndIsIdempotent) {
  ShutdownCoordinator coordinator{};
  ASSERT_TRUE(coordinator.valid());
  EXPECT_TRUE(coordinator.request_stop(ShutdownReason::sigterm, at(100ms)));
  EXPECT_FALSE(coordinator.request_stop(ShutdownReason::sigint, at(101ms)));
  const auto snapshot = coordinator.snapshot();
  EXPECT_EQ(snapshot.stage, ShutdownStage::stop_accepting);
  EXPECT_EQ(snapshot.reason, ShutdownReason::sigterm);
  EXPECT_EQ(snapshot.stop_requests, 2U);
  EXPECT_EQ(snapshot.repeated_stop_requests, 1U);
}

TEST(ShutdownCoordinatorTest, EnforcesFrozenStageOrder) {
  ShutdownCoordinator coordinator{};
  ASSERT_TRUE(coordinator.request_stop(ShutdownReason::service_stop, at(0ms)));
  EXPECT_EQ(coordinator.advance_to(ShutdownStage::wake_serial_io, at(1ms)),
            ShutdownErrorCategory::invalid_stage_transition);
  EXPECT_EQ(coordinator.advance_to(ShutdownStage::cancel_queued_and_retry_wait, at(1ms)),
            ShutdownErrorCategory::none);
  EXPECT_EQ(coordinator.advance_to(ShutdownStage::wake_serial_io, at(2ms)),
            ShutdownErrorCategory::none);
}

TEST(ShutdownCoordinatorTest, DetectsTimeRegressionWithoutChangingStage) {
  ShutdownCoordinator coordinator{};
  ASSERT_TRUE(coordinator.request_stop(ShutdownReason::service_stop, at(10ms)));
  EXPECT_EQ(coordinator.advance_to(ShutdownStage::cancel_queued_and_retry_wait, at(9ms)),
            ShutdownErrorCategory::time_regression);
  EXPECT_EQ(coordinator.snapshot().stage, ShutdownStage::stop_accepting);
}

TEST(ShutdownCoordinatorTest, DetectsExactGracefulTimeoutBoundary) {
  ShutdownCoordinator coordinator{};
  ASSERT_TRUE(coordinator.request_stop(ShutdownReason::service_stop, at(0ms)));
  EXPECT_FALSE(coordinator.deadline_exceeded(at(4999ms)));
  EXPECT_TRUE(coordinator.deadline_exceeded(at(5000ms)));
  EXPECT_TRUE(coordinator.snapshot().timed_out);
}

TEST(ShutdownCoordinatorTest, CompletesAllStagesAndRecordsStopTime) {
  ShutdownCoordinator coordinator{};
  ASSERT_TRUE(coordinator.request_stop(ShutdownReason::service_stop, at(0ms)));
  const ShutdownStage stages[]{ShutdownStage::cancel_queued_and_retry_wait,
                               ShutdownStage::wake_serial_io,
                               ShutdownStage::stop_polling,
                               ShutdownStage::drain_or_expire_measurements,
                               ShutdownStage::drain_mqtt_with_limit,
                               ShutdownStage::close_mqtt,
                               ShutdownStage::serial_io_owner_closes_fd,
                               ShutdownStage::join_all_threads,
                               ShutdownStage::stopped};
  for (std::size_t index = 0U; index < std::size(stages); ++index) {
    ASSERT_EQ(coordinator.advance_to(stages[index], at(std::chrono::milliseconds{index + 1U})),
              ShutdownErrorCategory::none);
  }
  const auto snapshot = coordinator.snapshot();
  EXPECT_EQ(snapshot.stage, ShutdownStage::stopped);
  EXPECT_EQ(snapshot.stopped_at, at(9ms));
}

} // namespace
} // namespace industrial_iot_gateway::lifecycle
