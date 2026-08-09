#include "industrial_iot_gateway/scheduler/poll_scheduler.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

namespace industrial_iot_gateway::scheduler {
namespace {

using namespace std::chrono_literals;

[[nodiscard]] SchedulerTimePoint at(const std::chrono::milliseconds offset) {
  return SchedulerTimePoint{} + offset;
}

[[nodiscard]] PollJob
make_job(const std::uint32_t job_id, const std::uint8_t slave_id,
         const std::chrono::milliseconds period = 1000ms,
         const protocol::FunctionCode function = protocol::FunctionCode::read_input_registers,
         const std::uint16_t address = 0U, const std::uint16_t quantity = 1U,
         const PollPriority priority = PollPriority::normal) {
  return PollJob{job_id, protocol::ReadRequest{slave_id, function, address, quantity}, period,
                 priority};
}

[[nodiscard]] RequestObservation observation_for(const ScheduledRequest &request,
                                                 const SchedulerTimePoint completed_at,
                                                 const bool succeeded = true) {
  return RequestObservation{request.request_id,  request.poll_job_id, succeeded,
                            request.enqueued_at, request.enqueued_at, completed_at};
}

void complete(PollScheduler &scheduler, const ScheduledRequest &request,
              const SchedulerTimePoint completed_at, const bool succeeded = true) {
  const auto result = scheduler.complete_request(observation_for(request, completed_at, succeeded));
  ASSERT_EQ(result.category, SchedulerErrorCategory::none);
}

template <typename T> [[nodiscard]] T require_value(const std::optional<T> &value) {
  if (!value.has_value()) {
    ADD_FAILURE() << "expected an optional value";
  }
  return value.value_or(T{});
}

[[nodiscard]] std::vector<PollJob> frozen_three_slave_plan() {
  return {
      make_job(1U, 1U, 500ms, protocol::FunctionCode::read_input_registers, 0U),
      make_job(2U, 1U, 500ms, protocol::FunctionCode::read_input_registers, 1U),
      make_job(3U, 1U, 1000ms, protocol::FunctionCode::read_input_registers, 2U),
      make_job(4U, 1U, 1000ms, protocol::FunctionCode::read_input_registers, 3U, 2U),
      make_job(5U, 1U, 5000ms, protocol::FunctionCode::read_holding_registers, 0U),
      make_job(6U, 2U, 1000ms, protocol::FunctionCode::read_holding_registers, 0U),
      make_job(7U, 2U, 1000ms, protocol::FunctionCode::read_holding_registers, 1U),
      make_job(8U, 2U, 200ms, protocol::FunctionCode::read_input_registers, 0U),
      make_job(9U, 2U, 200ms, protocol::FunctionCode::read_input_registers, 1U),
      make_job(10U, 2U, 500ms, protocol::FunctionCode::read_input_registers, 2U),
      make_job(11U, 3U, 1000ms, protocol::FunctionCode::read_holding_registers, 0U),
      make_job(12U, 3U, 1000ms, protocol::FunctionCode::read_holding_registers, 1U),
      make_job(13U, 3U, 1000ms, protocol::FunctionCode::read_holding_registers, 2U),
      make_job(14U, 3U, 1000ms, protocol::FunctionCode::read_input_registers, 0U, 2U),
      make_job(15U, 3U, 1000ms, protocol::FunctionCode::read_input_registers, 2U, 2U),
      make_job(16U, 3U, 1000ms, protocol::FunctionCode::read_input_registers, 4U, 2U),
  };
}

TEST(PollSchedulerValidationTest, RejectsEmptyPollPlan) {
  PollScheduler scheduler({}, at(0ms));
  EXPECT_FALSE(scheduler.valid());
  EXPECT_EQ(scheduler.validation_error().category, SchedulerErrorCategory::empty_poll_plan);
}

TEST(PollSchedulerValidationTest, RejectsDuplicateJobIds) {
  PollScheduler scheduler({make_job(7U, 1U), make_job(7U, 2U)}, at(0ms));
  EXPECT_FALSE(scheduler.valid());
  EXPECT_EQ(scheduler.validation_error().category, SchedulerErrorCategory::duplicate_poll_job_id);
  EXPECT_EQ(scheduler.validation_error().poll_job_id, 7U);
}

TEST(PollSchedulerValidationTest, RejectsInvalidProtocolFields) {
  PollScheduler invalid_slave({make_job(1U, 0U)}, at(0ms));
  EXPECT_EQ(invalid_slave.validation_error().category,
            SchedulerErrorCategory::invalid_slave_address);

  PollScheduler invalid_function(
      {make_job(2U, 1U, 1000ms, protocol::FunctionCode::write_single_register)}, at(0ms));
  EXPECT_EQ(invalid_function.validation_error().category,
            SchedulerErrorCategory::unsupported_poll_function);

  PollScheduler invalid_quantity(
      {make_job(3U, 1U, 1000ms, protocol::FunctionCode::read_input_registers, 0U, 0U)}, at(0ms));
  EXPECT_EQ(invalid_quantity.validation_error().category,
            SchedulerErrorCategory::invalid_poll_quantity);

  PollScheduler overflowing_address(
      {make_job(4U, 1U, 1000ms, protocol::FunctionCode::read_input_registers, 65535U, 2U)},
      at(0ms));
  EXPECT_EQ(overflowing_address.validation_error().category,
            SchedulerErrorCategory::address_range_overflow);

  PollScheduler invalid_period({make_job(5U, 1U, 0ms)}, at(0ms));
  EXPECT_EQ(invalid_period.validation_error().category,
            SchedulerErrorCategory::invalid_poll_period);
}

TEST(PollSchedulerTest, AllowsOnlyOneInFlightRequestAndUsesMonotonicIds) {
  PollScheduler scheduler({make_job(1U, 1U)}, at(0ms));
  ASSERT_TRUE(scheduler.valid());

  const auto first = require_value(scheduler.dispatch_next(at(0ms)));
  EXPECT_EQ(first.request_id, 1U);
  EXPECT_TRUE(scheduler.has_in_flight_request());
  EXPECT_FALSE(scheduler.dispatch_next(at(1000ms)).has_value());

  auto wrong = observation_for(first, at(1ms));
  wrong.request_id = 99U;
  EXPECT_EQ(scheduler.complete_request(wrong).category,
            SchedulerErrorCategory::request_id_mismatch);
  EXPECT_TRUE(scheduler.has_in_flight_request());

  complete(scheduler, first, at(1ms));
  EXPECT_FALSE(scheduler.has_in_flight_request());
  EXPECT_FALSE(scheduler.dispatch_next(at(999ms)).has_value());

  const auto second = require_value(scheduler.dispatch_next(at(1000ms)));
  EXPECT_EQ(second.request_id, 2U);
}

TEST(PollSchedulerTest, RotatesAcrossSlavesWhenJobsHaveTheSameDueTime) {
  PollScheduler scheduler({make_job(1U, 1U), make_job(2U, 1U), make_job(3U, 2U), make_job(4U, 2U),
                           make_job(5U, 3U), make_job(6U, 3U)},
                          at(0ms));
  ASSERT_TRUE(scheduler.valid());

  std::vector<std::uint8_t> slaves{};
  for (std::size_t index = 0U; index < 6U; ++index) {
    const auto request = require_value(scheduler.dispatch_next(at(0ms)));
    const auto &read_request = std::get<protocol::ReadRequest>(request.request);
    slaves.push_back(read_request.slave_id);
    complete(scheduler, request, at(0ms));
  }

  EXPECT_EQ(slaves, (std::vector<std::uint8_t>{1U, 2U, 3U, 1U, 2U, 3U}));
}

TEST(PollSchedulerTest, UsesPriorityOnlyAfterDueTimeMatches) {
  PollScheduler scheduler({make_job(1U, 1U, 1000ms, protocol::FunctionCode::read_input_registers,
                                    0U, 1U, PollPriority::normal),
                           make_job(2U, 2U, 1000ms, protocol::FunctionCode::read_input_registers,
                                    0U, 1U, PollPriority::high)},
                          at(0ms));

  const auto request = require_value(scheduler.dispatch_next(at(0ms)));
  EXPECT_EQ(request.poll_job_id, 2U);
}

TEST(PollSchedulerTest, SchedulesDifferentPeriodsWithoutEarlyDispatch) {
  PollScheduler scheduler(
      {make_job(1U, 1U, 500ms), make_job(2U, 2U, 200ms), make_job(3U, 3U, 1000ms)}, at(0ms));

  for (std::size_t index = 0U; index < 3U; ++index) {
    const auto request = require_value(scheduler.dispatch_next(at(0ms)));
    complete(scheduler, request, at(0ms));
  }
  EXPECT_FALSE(scheduler.dispatch_next(at(199ms)).has_value());

  const auto at_200 = require_value(scheduler.dispatch_next(at(200ms)));
  EXPECT_EQ(std::get<protocol::ReadRequest>(at_200.request).slave_id, 2U);
  complete(scheduler, at_200, at(200ms));

  const auto overdue_at_500 = require_value(scheduler.dispatch_next(at(500ms)));
  EXPECT_EQ(std::get<protocol::ReadRequest>(overdue_at_500.request).slave_id, 2U);
  complete(scheduler, overdue_at_500, at(500ms));

  const auto due_at_500 = require_value(scheduler.dispatch_next(at(500ms)));
  EXPECT_EQ(std::get<protocol::ReadRequest>(due_at_500.request).slave_id, 1U);
}

TEST(PollSchedulerTest, CoalescesMissedPeriodsWithoutBurstReplay) {
  PollScheduler scheduler({make_job(1U, 1U, 1000ms)}, at(0ms));

  const auto request = require_value(scheduler.dispatch_next(at(2500ms)));
  EXPECT_EQ(request.due_at, at(0ms));
  EXPECT_EQ(scheduler.slave_statistics(1U).poll_periods_coalesced, 2U);
  EXPECT_EQ(scheduler.slave_statistics(1U).maximum_dispatch_lateness, 2500ms);
  complete(scheduler, request, at(2500ms));

  EXPECT_FALSE(scheduler.dispatch_next(at(2999ms)).has_value());
  EXPECT_EQ(scheduler.next_due_time(), at(3000ms));
  EXPECT_TRUE(scheduler.dispatch_next(at(3000ms)).has_value());
}

TEST(PollSchedulerTest, RecordsIndependentSlaveStatisticsAndTiming) {
  PollScheduler scheduler({make_job(1U, 1U), make_job(2U, 2U), make_job(3U, 3U)}, at(0ms));

  for (std::uint8_t slave = 1U; slave <= 3U; ++slave) {
    const auto request = require_value(scheduler.dispatch_next(at(0ms)));
    ASSERT_EQ(std::get<protocol::ReadRequest>(request.request).slave_id, slave);
    complete(scheduler, request, at(10ms + std::chrono::milliseconds{slave}), slave != 2U);
  }

  EXPECT_EQ(scheduler.slave_statistics(1U).requests_completed, 1U);
  EXPECT_EQ(scheduler.slave_statistics(1U).requests_failed, 0U);
  EXPECT_EQ(scheduler.slave_statistics(2U).requests_completed, 1U);
  EXPECT_EQ(scheduler.slave_statistics(2U).requests_failed, 1U);
  EXPECT_EQ(scheduler.slave_statistics(3U).requests_completed, 1U);
  EXPECT_EQ(scheduler.slave_statistics(3U).requests_failed, 0U);
  EXPECT_EQ(require_value(scheduler.slave_statistics(2U).last_timing).completed_at, at(12ms));
}

TEST(PollSchedulerTest, RejectsInvalidCompletionWithoutReleasingSlot) {
  PollScheduler scheduler({make_job(1U, 1U)}, at(0ms));
  const auto request = require_value(scheduler.dispatch_next(at(0ms)));

  auto invalid = observation_for(request, at(10ms));
  invalid.sent_at = at(8ms);
  invalid.first_byte_at = at(7ms);
  EXPECT_EQ(scheduler.complete_request(invalid).category,
            SchedulerErrorCategory::invalid_observation_times);
  EXPECT_TRUE(scheduler.has_in_flight_request());

  auto wrong_job = observation_for(request, at(10ms));
  wrong_job.poll_job_id = 99U;
  EXPECT_EQ(scheduler.complete_request(wrong_job).category,
            SchedulerErrorCategory::poll_job_id_mismatch);
  EXPECT_TRUE(scheduler.has_in_flight_request());
}

TEST(PollSchedulerTest, TerminalFailureReleasesBusForAnotherSlave) {
  PollScheduler scheduler({make_job(1U, 1U), make_job(2U, 2U), make_job(3U, 3U)}, at(0ms));
  const auto failed = require_value(scheduler.dispatch_next(at(0ms)));
  complete(scheduler, failed, at(1ms), false);

  const auto next = require_value(scheduler.dispatch_next(at(1ms)));
  EXPECT_EQ(std::get<protocol::ReadRequest>(next.request).slave_id, 2U);
}

TEST(PollSchedulerTest, FrozenRegisterMapProducesSixteenValidPollJobs) {
  PollScheduler scheduler(frozen_three_slave_plan(), at(0ms));
  ASSERT_TRUE(scheduler.valid());
  EXPECT_EQ(scheduler.poll_job_count(), 16U);

  for (std::size_t index = 0U; index < 16U; ++index) {
    const auto request = require_value(scheduler.dispatch_next(at(0ms)));
    complete(scheduler, request, at(0ms));
  }
  EXPECT_FALSE(scheduler.dispatch_next(at(0ms)).has_value());
  EXPECT_GT(scheduler.slave_statistics(1U).requests_dispatched, 0U);
  EXPECT_GT(scheduler.slave_statistics(2U).requests_dispatched, 0U);
  EXPECT_GT(scheduler.slave_statistics(3U).requests_dispatched, 0U);
}

} // namespace
} // namespace industrial_iot_gateway::scheduler
