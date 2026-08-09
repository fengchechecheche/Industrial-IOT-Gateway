#include "industrial_iot_gateway/config/runtime_config.hpp"
#include "industrial_iot_gateway/pipeline/event_pipeline.hpp"
#include "industrial_iot_gateway/pipeline/request_queue.hpp"
#include "industrial_iot_gateway/runtime/gateway_runtime.hpp"

#include <chrono>
#include <cstdint>
#include <sstream>
#include <utility>

#include <gtest/gtest.h>

namespace {

using industrial_iot_gateway::concurrency::QueuePushStatus;
using industrial_iot_gateway::pipeline::MeasurementQueue;
using industrial_iot_gateway::pipeline::MeasurementRaw;
using industrial_iot_gateway::pipeline::RequestQueue;
using industrial_iot_gateway::pipeline::RequestQueueConfig;
using industrial_iot_gateway::runtime::GatewayRuntime;
using industrial_iot_gateway::runtime::GatewayRuntimeConfig;
using industrial_iot_gateway::runtime::RuntimeErrorCategory;
using industrial_iot_gateway::scheduler::RequestKind;
using industrial_iot_gateway::scheduler::ScheduledRequest;
using namespace std::chrono_literals;

[[nodiscard]] ScheduledRequest explicit_request(std::uint64_t request_id) {
  const auto now = std::chrono::steady_clock::now();
  ScheduledRequest request{};
  request.request_id = request_id;
  request.request_kind = RequestKind::explicit_write;
  request.request = industrial_iot_gateway::protocol::WriteSingleRegisterRequest{
      1U, static_cast<std::uint16_t>(100U + request_id), 1U};
  request.created_at = now;
  request.enqueued_at = now;
  request.due_at = now;
  request.deadline = now + 1s;
  request.attempt_eligible_at = now;
  request.attempt = 1U;
  request.max_attempts = 1U;
  return request;
}

struct MeasurementIdentity {
  std::uint64_t request_id{};
  std::uint16_t address{};
};

[[nodiscard]] MeasurementRaw measurement(MeasurementIdentity identity) {
  MeasurementRaw value{};
  value.request_id = identity.request_id;
  value.slave_id = 1U;
  value.function = industrial_iot_gateway::protocol::FunctionCode::read_holding_registers;
  value.start_address = identity.address;
  value.registers = {1U};
  value.received_at = std::chrono::steady_clock::now();
  value.received_at_system = std::chrono::system_clock::now();
  return value;
}

TEST(FaultMatrixQueueTest, F07ExplicitRequestQueueFullIsVisibleAndBounded) {
  RequestQueue queue(RequestQueueConfig{{4U, 3U}, 2U});
  ASSERT_TRUE(queue.valid());
  for (std::uint64_t request_id = 1U; request_id <= 4U; ++request_id) {
    EXPECT_EQ(queue.push(explicit_request(request_id)), QueuePushStatus::accepted);
  }
  EXPECT_EQ(queue.push(explicit_request(5U)), QueuePushStatus::full);
  const auto statistics = queue.statistics();
  EXPECT_EQ(statistics.capacity, 4U);
  EXPECT_EQ(statistics.maximum_depth, 4U);
  EXPECT_EQ(statistics.full, 1U);
}

TEST(FaultMatrixQueueTest, F08PausedMeasurementConsumerProducesVisibleBoundedFull) {
  MeasurementQueue queue({2U, 1U});
  ASSERT_TRUE(queue.valid());
  EXPECT_EQ(queue.push(measurement({1U, 100U})), QueuePushStatus::accepted);
  EXPECT_EQ(queue.push(measurement({2U, 101U})), QueuePushStatus::accepted);
  EXPECT_EQ(queue.push(measurement({3U, 102U})), QueuePushStatus::full);
  const auto statistics = queue.statistics();
  EXPECT_EQ(statistics.capacity, 2U);
  EXPECT_EQ(statistics.maximum_depth, 2U);
  EXPECT_EQ(statistics.full, 1U);
}

TEST(FaultMatrixConfigurationTest, F15RejectsInvalidRequestQueueBeforeStartingThreads) {
  auto loaded =
      industrial_iot_gateway::config::load_runtime_configuration(GATEWAY_REGISTER_MAP_PATH);
  ASSERT_TRUE(loaded) << loaded.detail;
  GatewayRuntimeConfig config{};
  config.serial.device_path = "/dev/null";
  config.registers = std::move(*loaded.configuration);
  config.request_queue.queue = {0U, 0U};
  std::ostringstream output;
  GatewayRuntime runtime(std::move(config), output);
  EXPECT_FALSE(runtime.valid());
  EXPECT_EQ(runtime.validation_error(), RuntimeErrorCategory::invalid_request_queue_configuration);
  EXPECT_FALSE(runtime.start());
  EXPECT_FALSE(runtime.statistics().running);
}

} // namespace
