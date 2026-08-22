#include "industrial_iot_gateway/scheduler/poll_scheduler.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

namespace industrial_iot_gateway::scheduler {
namespace {

using namespace std::chrono_literals;

[[nodiscard]] SchedulerTimePoint at(const std::chrono::milliseconds offset) {
  return SchedulerTimePoint{} + offset;
}

[[nodiscard]] PollJob make_job(const std::uint32_t job_id, const std::uint8_t slave_id,
                               const std::chrono::milliseconds period = 10000ms) {
  return PollJob{job_id,
                 protocol::ReadRequest{slave_id, protocol::FunctionCode::read_input_registers,
                                       static_cast<std::uint16_t>(job_id - 1U), 1U},
                 period, PollPriority::normal};
}

template <typename T> [[nodiscard]] T require_value(const std::optional<T> &value) {
  if (!value.has_value()) {
    ADD_FAILURE() << "expected an optional value";
  }
  return value.value_or(T{});
}

void mark_sent(PollScheduler &scheduler, const ScheduledRequest &request,
               const SchedulerTimePoint sent_at) {
  const auto error = scheduler.mark_attempt_sent(request.request_id, request.attempt, sent_at);
  ASSERT_EQ(error.category, SchedulerErrorCategory::none);
}

[[nodiscard]] SchedulerTransition finish(PollScheduler &scheduler, const ScheduledRequest &request,
                                         const RequestResultCategory category,
                                         const SchedulerTimePoint observed_at) {
  return scheduler.record_attempt_result(AttemptResult{request.request_id, request.poll_job_id,
                                                       request.attempt, category, observed_at,
                                                       std::nullopt});
}

[[nodiscard]] SchedulerPolicyConfig single_attempt_policy() {
  SchedulerPolicyConfig config{};
  config.reliability.response_timeout = 100ms;
  config.reliability.max_attempts = 1U;
  config.reliability.request_deadline = 500ms;
  return config;
}

TEST(PollSchedulerReliabilityTest, CreatesFrozenRequestLifecycleFields) {
  PollScheduler scheduler({make_job(1U, 1U)}, at(0ms));
  ASSERT_TRUE(scheduler.valid());
  const auto request = require_value(scheduler.dispatch_next(at(0ms)));
  EXPECT_EQ(request.request_id, 1U);
  EXPECT_EQ(request.attempt, 1U);
  EXPECT_EQ(request.max_attempts, 3U);
  EXPECT_EQ(request.state, RequestState::queued);
  EXPECT_EQ(request.deadline, at(5000ms));
  EXPECT_EQ(request.attempt_eligible_at, at(0ms));
  EXPECT_FALSE(request.is_recovery_probe);
  EXPECT_EQ(scheduler.slave_statistics(1U).requests_dispatched, 1U);
  EXPECT_EQ(scheduler.slave_statistics(1U).attempts_dispatched, 1U);
}

TEST(PollSchedulerReliabilityTest, TimesOutAtBoundaryAndAllowsOtherSlaveDuringBackoff) {
  PollScheduler scheduler({make_job(1U, 1U), make_job(2U, 2U)}, at(0ms));
  const auto first = require_value(scheduler.dispatch_next(at(0ms)));
  mark_sent(scheduler, first, at(0ms));
  EXPECT_FALSE(scheduler.on_time_advanced(at(499ms)).has_value());

  const auto timeout = require_value(scheduler.on_time_advanced(at(500ms)));
  EXPECT_TRUE(timeout.occurred);
  EXPECT_FALSE(timeout.terminal);
  EXPECT_EQ(timeout.result, RequestResultCategory::response_timeout);
  EXPECT_EQ(timeout.next_state, RequestState::retry_wait);
  EXPECT_EQ(timeout.retry_at, at(700ms));
  EXPECT_FALSE(scheduler.has_in_flight_request());

  const auto second_slave = require_value(scheduler.dispatch_next(at(500ms)));
  EXPECT_EQ(std::get<protocol::ReadRequest>(second_slave.request).slave_id, 2U);
  mark_sent(scheduler, second_slave, at(500ms));
  const auto success = finish(scheduler, second_slave, RequestResultCategory::success, at(501ms));
  EXPECT_TRUE(success.terminal);

  EXPECT_FALSE(scheduler.dispatch_next(at(699ms)).has_value());
  const auto retry = require_value(scheduler.dispatch_next(at(700ms)));
  EXPECT_EQ(retry.request_id, first.request_id);
  EXPECT_EQ(retry.attempt, 2U);
  EXPECT_EQ(std::get<protocol::ReadRequest>(retry.request).slave_id, 1U);
}

TEST(PollSchedulerReliabilityTest, ExhaustsThreeAttemptsWithBoundedBackoff) {
  PollScheduler scheduler({make_job(1U, 1U)}, at(0ms));
  const auto first = require_value(scheduler.dispatch_next(at(0ms)));
  mark_sent(scheduler, first, at(0ms));
  const auto first_failure =
      finish(scheduler, first, RequestResultCategory::response_timeout, at(500ms));
  EXPECT_EQ(first_failure.retry_at, at(700ms));

  const auto second = require_value(scheduler.dispatch_next(at(700ms)));
  EXPECT_EQ(second.request_id, first.request_id);
  EXPECT_EQ(second.attempt, 2U);
  mark_sent(scheduler, second, at(700ms));
  const auto second_failure =
      finish(scheduler, second, RequestResultCategory::crc_mismatch, at(1200ms));
  EXPECT_EQ(second_failure.retry_at, at(1600ms));

  const auto third = require_value(scheduler.dispatch_next(at(1600ms)));
  EXPECT_EQ(third.request_id, first.request_id);
  EXPECT_EQ(third.attempt, 3U);
  mark_sent(scheduler, third, at(1600ms));
  const auto terminal =
      finish(scheduler, third, RequestResultCategory::truncated_frame, at(2100ms));
  EXPECT_TRUE(terminal.terminal);
  EXPECT_EQ(terminal.next_state, RequestState::failed);
  EXPECT_EQ(scheduler.slave_statistics(1U).requests_completed, 1U);
  EXPECT_EQ(scheduler.slave_statistics(1U).requests_failed, 1U);
  EXPECT_EQ(scheduler.slave_statistics(1U).retries_scheduled, 2U);
  EXPECT_EQ(scheduler.slave_statistics(1U).retry_exhausted, 1U);
}

TEST(PollSchedulerReliabilityTest, RequestDeadlineTruncatesResponseWait) {
  PollScheduler scheduler({make_job(1U, 1U)}, at(0ms));
  const auto request = require_value(scheduler.dispatch_next(at(0ms)));
  mark_sent(scheduler, request, at(4800ms));
  EXPECT_FALSE(scheduler.on_time_advanced(at(4999ms)).has_value());
  const auto terminal = require_value(scheduler.on_time_advanced(at(5000ms)));
  EXPECT_TRUE(terminal.terminal);
  EXPECT_EQ(terminal.result, RequestResultCategory::deadline_exceeded);
  EXPECT_EQ(scheduler.slave_statistics(1U).deadline_exceeded, 1U);
}

TEST(PollSchedulerReliabilityTest, AcceptsDeadlineExceededBeforeQueuedRetryIsSent) {
  PollScheduler scheduler({make_job(1U, 1U)}, at(0ms));
  const auto first = require_value(scheduler.dispatch_next(at(0ms)));
  mark_sent(scheduler, first, at(0ms));
  const auto first_failure =
      finish(scheduler, first, RequestResultCategory::response_timeout, at(500ms));
  ASSERT_EQ(first_failure.retry_at, at(700ms));

  const auto retry = require_value(scheduler.dispatch_next(at(4900ms)));
  ASSERT_EQ(retry.attempt, 2U);
  ASSERT_EQ(retry.state, RequestState::queued);

  const auto expired =
      finish(scheduler, retry, RequestResultCategory::deadline_exceeded, at(5000ms));
  EXPECT_EQ(expired.error.category, SchedulerErrorCategory::none);
  EXPECT_TRUE(expired.occurred);
  EXPECT_TRUE(expired.terminal);
  EXPECT_EQ(expired.next_state, RequestState::failed);
  EXPECT_EQ(expired.result, RequestResultCategory::deadline_exceeded);
  EXPECT_FALSE(scheduler.has_in_flight_request());
  EXPECT_EQ(scheduler.slave_statistics(1U).deadline_exceeded, 1U);
}

TEST(PollSchedulerReliabilityTest, ThreeFinalFailuresEnterOfflineAndProbeTwiceToRecover) {
  auto config = single_attempt_policy();
  PollScheduler scheduler({make_job(1U, 1U, 100ms)}, at(0ms), config);
  ASSERT_TRUE(scheduler.valid());

  for (std::uint32_t failure = 0U; failure < 3U; ++failure) {
    const auto started_at = std::chrono::milliseconds{failure * 100U};
    const auto request = require_value(scheduler.dispatch_next(at(started_at)));
    mark_sent(scheduler, request, at(started_at));
    const auto transition = require_value(scheduler.on_time_advanced(at(started_at + 100ms)));
    EXPECT_TRUE(transition.terminal);
  }

  const auto &offline = scheduler.device_health_statistics(1U);
  EXPECT_EQ(offline.state, DeviceHealthState::offline);
  EXPECT_EQ(offline.consecutive_final_failures, 3U);
  EXPECT_EQ(offline.offline_transitions, 1U);
  EXPECT_EQ(offline.next_probe_at, at(5300ms));
  EXPECT_FALSE(scheduler.dispatch_next(at(5299ms)).has_value());

  const auto probe_one = require_value(scheduler.dispatch_next(at(5300ms)));
  EXPECT_TRUE(probe_one.is_recovery_probe);
  mark_sent(scheduler, probe_one, at(5300ms));
  const auto probe_one_result =
      finish(scheduler, probe_one, RequestResultCategory::success, at(5301ms));
  EXPECT_TRUE(probe_one_result.device_state_changed);
  EXPECT_EQ(scheduler.device_health_statistics(1U).state, DeviceHealthState::probing);
  EXPECT_EQ(scheduler.device_health_statistics(1U).consecutive_probe_successes, 1U);
  EXPECT_EQ(scheduler.device_health_statistics(1U).next_probe_at, at(10301ms));

  const auto probe_two = require_value(scheduler.dispatch_next(at(10301ms)));
  EXPECT_TRUE(probe_two.is_recovery_probe);
  mark_sent(scheduler, probe_two, at(10301ms));
  const auto recovered = finish(scheduler, probe_two, RequestResultCategory::success, at(10302ms));
  EXPECT_TRUE(recovered.device_state_changed);
  EXPECT_EQ(recovered.device_state, DeviceHealthState::online);
  EXPECT_EQ(scheduler.device_health_statistics(1U).consecutive_final_failures, 0U);
  EXPECT_EQ(scheduler.device_health_statistics(1U).recovery_transitions, 1U);
  EXPECT_TRUE(scheduler.dispatch_next(at(10302ms)).has_value());
}

TEST(PollSchedulerReliabilityTest, RemoteExceptionProvesCommunicationWithoutFreshSuccess) {
  auto config = single_attempt_policy();
  PollScheduler scheduler({make_job(1U, 1U, 100ms)}, at(0ms), config);

  const auto failed = require_value(scheduler.dispatch_next(at(0ms)));
  mark_sent(scheduler, failed, at(0ms));
  ASSERT_TRUE(scheduler.on_time_advanced(at(100ms)).has_value());
  ASSERT_EQ(scheduler.device_health_statistics(1U).consecutive_final_failures, 1U);

  const auto remote = require_value(scheduler.dispatch_next(at(100ms)));
  mark_sent(scheduler, remote, at(100ms));
  auto result = AttemptResult{remote.request_id, remote.poll_job_id,
                              remote.attempt,    RequestResultCategory::remote_exception,
                              at(110ms),         4U};
  const auto transition = scheduler.record_attempt_result(result);
  EXPECT_TRUE(transition.terminal);
  EXPECT_EQ(transition.next_state, RequestState::completed);
  EXPECT_EQ(transition.remote_exception_code, 4U);
  EXPECT_EQ(scheduler.device_health_statistics(1U).consecutive_final_failures, 0U);
  EXPECT_EQ(scheduler.slave_statistics(1U).requests_failed, 2U);
}

TEST(PollSchedulerReliabilityTest, RejectsInvalidHealthConfiguration) {
  SchedulerPolicyConfig config{};
  config.consecutive_final_failures_to_offline = 0U;
  PollScheduler invalid_failures({make_job(1U, 1U)}, at(0ms), config);
  EXPECT_FALSE(invalid_failures.valid());
  EXPECT_EQ(invalid_failures.validation_error().category,
            SchedulerErrorCategory::invalid_device_health_configuration);

  config = SchedulerPolicyConfig{};
  config.offline_probe_interval = 0ms;
  PollScheduler invalid_probe({make_job(1U, 1U)}, at(0ms), config);
  EXPECT_FALSE(invalid_probe.valid());
  EXPECT_EQ(invalid_probe.validation_error().category,
            SchedulerErrorCategory::invalid_device_health_configuration);
}

} // namespace
} // namespace industrial_iot_gateway::scheduler
