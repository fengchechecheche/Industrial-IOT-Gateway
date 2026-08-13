#include "industrial_iot_gateway/config/runtime_config.hpp"
#include "industrial_iot_gateway/runtime/gateway_runtime.hpp"
#include "industrial_iot_gateway/simulation/pty_bus_harness.hpp"

#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

using industrial_iot_gateway::config::load_runtime_configuration;
using industrial_iot_gateway::lifecycle::ShutdownReason;
using industrial_iot_gateway::pty_slave::FaultMode;
using industrial_iot_gateway::pty_slave::FaultPlan;
using industrial_iot_gateway::runtime::GatewayRuntime;
using industrial_iot_gateway::runtime::GatewayRuntimeConfig;
using industrial_iot_gateway::test_support::PtyBusHarness;

struct RecordingState {
  mutable std::mutex mutex{};
  std::vector<industrial_iot_gateway::pipeline::PublishMessage> messages{};
};

class RecordingPublishSink final : public industrial_iot_gateway::publish::PublishSink {
public:
  explicit RecordingPublishSink(std::shared_ptr<RecordingState> state) : state_(std::move(state)) {}

  [[nodiscard]] bool valid() const noexcept override { return state_ != nullptr; }
  [[nodiscard]] bool start() override {
    started_ = true;
    return valid();
  }
  [[nodiscard]] industrial_iot_gateway::concurrency::QueuePushStatus
  submit(industrial_iot_gateway::pipeline::PublishMessage message) override {
    if (!started_ || stopped_) {
      return industrial_iot_gateway::concurrency::QueuePushStatus::closed;
    }
    const std::lock_guard<std::mutex> lock(state_->mutex);
    state_->messages.push_back(std::move(message));
    ++statistics_.publish_attempts;
    ++statistics_.publish_successes;
    return industrial_iot_gateway::concurrency::QueuePushStatus::accepted;
  }
  void request_stop(std::chrono::steady_clock::time_point) noexcept override { stopped_ = true; }
  void join() noexcept override {}
  [[nodiscard]] industrial_iot_gateway::publish::PublishSinkStatistics statistics() const override {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    return statistics_;
  }

private:
  std::shared_ptr<RecordingState> state_{};
  industrial_iot_gateway::publish::PublishSinkStatistics statistics_{};
  bool started_{};
  bool stopped_{};
};

[[nodiscard]] bool
has_retained_telemetry(const RecordingState &state,
                       industrial_iot_gateway::quality::RegisterQuality quality) {
  const std::lock_guard<std::mutex> lock(state.mutex);
  for (const auto &message : state.messages) {
    const auto *telemetry = std::get_if<industrial_iot_gateway::pipeline::TelemetryEvent>(&message);
    if (telemetry != nullptr && telemetry->quality == quality && telemetry->value_is_retained &&
        telemetry->value.has_value() && telemetry->raw_value.has_value()) {
      return true;
    }
  }
  return false;
}

const std::string kRegisterMap = GATEWAY_REGISTER_MAP_PATH;
const std::string kScenarioMap = GATEWAY_PTY_SCENARIO_PATH;

template <typename Predicate>
[[nodiscard]] bool wait_until(Predicate predicate, const std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return predicate();
}

[[nodiscard]] GatewayRuntimeConfig runtime_config_for(const std::string &device_path) {
  auto loaded = load_runtime_configuration(kRegisterMap);
  if (!loaded) {
    return {};
  }
  GatewayRuntimeConfig config{};
  config.serial.device_path = device_path;
  config.registers = std::move(*loaded.configuration);
  config.response_timeout = std::chrono::milliseconds(180);
  config.serial_reopen_backoff = std::chrono::milliseconds(20);
  return config;
}

TEST(GatewayRuntimePtyTest, PollsThreeSlavesWritesAndStopsInOrder) {
  PtyBusHarness bus(kRegisterMap, kScenarioMap);
  ASSERT_TRUE(bus.start()) << bus.last_error();

  auto loaded = load_runtime_configuration(kRegisterMap);
  ASSERT_TRUE(loaded) << loaded.detail;
  GatewayRuntimeConfig config{};
  config.serial.device_path = bus.gateway_path();
  config.registers = std::move(*loaded.configuration);
  config.response_timeout = std::chrono::milliseconds(250);
  config.serial_reopen_backoff = std::chrono::milliseconds(20);
  std::ostringstream evidence;
  GatewayRuntime runtime(std::move(config), evidence);
  ASSERT_TRUE(runtime.valid());
  ASSERT_TRUE(runtime.start());

  ASSERT_TRUE(wait_until(
      [&runtime] {
        const auto stats = runtime.statistics();
        return stats.slaves[1].requests_succeeded >= 1U &&
               stats.slaves[2].requests_succeeded >= 1U &&
               stats.slaves[3].requests_succeeded >= 1U && stats.requests_succeeded >= 16U;
      },
      std::chrono::seconds(6)));

  EXPECT_EQ(runtime.submit_write(2U, 0U, 1300U),
            industrial_iot_gateway::concurrency::QueuePushStatus::accepted);
  ASSERT_TRUE(wait_until([&runtime] { return runtime.statistics().explicit_write_successes >= 1U; },
                         std::chrono::seconds(2)));

  runtime.request_stop(ShutdownReason::service_stop);
  runtime.join();
  const auto statistics = runtime.statistics();
  EXPECT_TRUE(statistics.stopped);
  EXPECT_FALSE(statistics.running);
  EXPECT_EQ(statistics.maximum_in_flight_requests, 1U);
  EXPECT_EQ(statistics.in_flight_requests, 0U);
  EXPECT_EQ(statistics.serial_open_successes, 1U);
  EXPECT_EQ(statistics.serial_close_count, 1U);
  EXPECT_GE(statistics.telemetry_events, 16U);
  EXPECT_EQ(statistics.requests_failed, 0U);
  EXPECT_NE(evidence.str().find("runtime_started"), std::string::npos);
  EXPECT_NE(evidence.str().find("telemetry"), std::string::npos);
  EXPECT_NE(evidence.str().find("stop_requested"), std::string::npos);

  bus.stop();
  const auto handled = bus.handled_requests();
  EXPECT_GT(handled[0], 0U);
  EXPECT_GT(handled[1], 0U);
  EXPECT_GT(handled[2], 0U);
}

struct FaultCase {
  const char *name;
  FaultPlan plan;
  std::function<bool(const industrial_iot_gateway::runtime::GatewayRuntimeStatistics &)> observed;
};

class GatewayRuntimeFaultPtyTest : public testing::TestWithParam<FaultCase> {};

TEST_P(GatewayRuntimeFaultPtyTest, ClassifiesFaultAndKeepsOtherSlavesPolling) {
  std::array<FaultPlan, 3U> faults{};
  faults[2] = GetParam().plan;
  PtyBusHarness bus(kRegisterMap, kScenarioMap, faults);
  ASSERT_TRUE(bus.start()) << bus.last_error();
  std::ostringstream evidence;
  GatewayRuntime runtime(runtime_config_for(bus.gateway_path()), evidence);
  ASSERT_TRUE(runtime.valid());
  ASSERT_TRUE(runtime.start());

  ASSERT_TRUE(wait_until(
      [&] {
        const auto statistics = runtime.statistics();
        return GetParam().observed(statistics) && statistics.slaves[1].requests_succeeded > 0U &&
               statistics.slaves[2].requests_succeeded > 0U;
      },
      std::chrono::seconds(5)));
  runtime.request_stop(ShutdownReason::service_stop);
  runtime.join();
  EXPECT_TRUE(runtime.statistics().stopped);
  EXPECT_EQ(runtime.statistics().maximum_in_flight_requests, 1U);
}

INSTANTIATE_TEST_SUITE_P(
    FaultMatrix, GatewayRuntimeFaultPtyTest,
    testing::Values(FaultCase{"delayed", FaultPlan{FaultMode::delayed_response, 60U, 2U, 2U},
                              [](const auto &value) {
                                return value.slaves[3].requests_succeeded > 0U;
                              }},
                    FaultCase{"silent", FaultPlan{FaultMode::silent, 40U, 2U, 2U},
                              [](const auto &value) { return value.response_timeouts > 0U; }},
                    FaultCase{"bad_crc", FaultPlan{FaultMode::bad_crc, 40U, 2U, 2U},
                              [](const auto &value) { return value.crc_errors > 0U; }},
                    FaultCase{"truncated", FaultPlan{FaultMode::truncated_response, 40U, 2U, 2U},
                              [](const auto &value) { return value.truncated_frames > 0U; }},
                    FaultCase{"exception_01", FaultPlan{FaultMode::exception_response, 40U, 1U, 2U},
                              [](const auto &value) { return value.remote_exceptions > 0U; }},
                    FaultCase{"exception_02", FaultPlan{FaultMode::exception_response, 40U, 2U, 2U},
                              [](const auto &value) { return value.remote_exceptions > 0U; }},
                    FaultCase{"exception_03", FaultPlan{FaultMode::exception_response, 40U, 3U, 2U},
                              [](const auto &value) { return value.remote_exceptions > 0U; }},
                    FaultCase{"exception_04", FaultPlan{FaultMode::exception_response, 40U, 4U, 2U},
                              [](const auto &value) { return value.remote_exceptions > 0U; }}),
    [](const testing::TestParamInfo<FaultCase> &info) { return info.param.name; });

TEST(GatewayRuntimePtyTest, BoundsExplicitRequestPressureAndStopsWithoutDeadlock) {
  PtyBusHarness bus(kRegisterMap, kScenarioMap);
  ASSERT_TRUE(bus.start()) << bus.last_error();
  std::ostringstream evidence;
  GatewayRuntime runtime(runtime_config_for(bus.gateway_path()), evidence);
  ASSERT_TRUE(runtime.start());
  std::size_t rejected{};
  for (std::size_t index = 0U; index < 2'000U; ++index) {
    if (runtime.submit_write(2U, 0U, static_cast<std::uint16_t>(index % 3'001U)) ==
        industrial_iot_gateway::concurrency::QueuePushStatus::full) {
      ++rejected;
    }
  }
  EXPECT_GT(rejected, 0U);
  runtime.request_stop(ShutdownReason::service_stop);
  runtime.join();
  const auto statistics = runtime.statistics();
  EXPECT_TRUE(statistics.stopped);
  EXPECT_GT(statistics.request_queue.full, 0U);
  EXPECT_EQ(statistics.in_flight_requests, 0U);
}

TEST(GatewayRuntimePtyTest, ReopensStableDevicePathAfterPtyDisconnect) {
  PtyBusHarness bus(kRegisterMap, kScenarioMap);
  ASSERT_TRUE(bus.start()) << bus.last_error();
  std::ostringstream evidence;
  GatewayRuntime runtime(runtime_config_for(bus.gateway_path()), evidence);
  ASSERT_TRUE(runtime.start());
  ASSERT_TRUE(wait_until([&runtime] { return runtime.statistics().requests_succeeded >= 20U; },
                         std::chrono::seconds(3)));
  const auto successes_before = runtime.statistics().requests_succeeded;
  ASSERT_TRUE(bus.disconnect_and_reconnect(std::chrono::milliseconds(80))) << bus.last_error();
  const auto recovery_started = std::chrono::steady_clock::now();
  const auto recovered = wait_until(
      [&runtime, successes_before] {
        const auto statistics = runtime.statistics();
        return statistics.serial_open_successes >= 2U &&
               statistics.requests_succeeded > successes_before;
      },
      std::chrono::seconds(4));
  const auto recovery_statistics = runtime.statistics();
  ASSERT_TRUE(recovered) << " opens=" << recovery_statistics.serial_open_successes
                         << " serial_errors=" << recovery_statistics.serial_errors
                         << " successes=" << recovery_statistics.requests_succeeded
                         << " before=" << successes_before << "\n"
                         << evidence.str();
  std::cout << "FAULT_RECOVERY_TIME_MS="
            << std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - recovery_started)
                   .count()
            << '\n';
  runtime.request_stop(ShutdownReason::service_stop);
  runtime.join();
  EXPECT_GE(runtime.statistics().serial_open_successes, 2U);
  EXPECT_GT(runtime.statistics().serial_errors, 0U);
}

TEST(GatewayRuntimePtyTest, QuarantinesLateResponseWithoutPollutingNextTransaction) {
  PtyBusHarness bus(kRegisterMap, kScenarioMap);
  ASSERT_TRUE(bus.start()) << bus.last_error();
  std::ostringstream evidence;
  auto config = runtime_config_for(bus.gateway_path());
  config.response_timeout = std::chrono::milliseconds(500);
  config.late_response_guard = std::chrono::milliseconds(200);
  GatewayRuntime runtime(std::move(config), evidence);
  ASSERT_TRUE(runtime.start());
  ASSERT_TRUE(wait_until([&runtime] { return runtime.statistics().requests_succeeded >= 20U; },
                         std::chrono::seconds(3)));
  const auto before = runtime.statistics();
  ASSERT_TRUE(bus.set_fault(3U, FaultPlan{FaultMode::delayed_response, 650U, 2U, 2U}));
  ASSERT_TRUE(wait_until(
      [&runtime, &before] {
        const auto statistics = runtime.statistics();
        return statistics.response_timeouts > before.response_timeouts &&
               statistics.late_response_quarantines > before.late_response_quarantines &&
               statistics.late_response_bytes_discarded >=
                   before.late_response_bytes_discarded + 5U;
      },
      std::chrono::seconds(4)))
      << evidence.str();
  ASSERT_TRUE(bus.set_fault(3U, FaultPlan{}));
  ASSERT_TRUE(wait_until(
      [&runtime, &before] {
        const auto statistics = runtime.statistics();
        return statistics.slaves[1].requests_succeeded > before.slaves[1].requests_succeeded &&
               statistics.slaves[2].requests_succeeded > before.slaves[2].requests_succeeded &&
               statistics.slaves[3].requests_succeeded > before.slaves[3].requests_succeeded;
      },
      std::chrono::seconds(10)))
      << evidence.str();
  runtime.request_stop(ShutdownReason::service_stop);
  runtime.join();
  const auto after = runtime.statistics();
  EXPECT_TRUE(after.stopped);
  EXPECT_EQ(after.serial_open_successes, 1U);
  EXPECT_EQ(after.serial_errors, before.serial_errors) << evidence.str();
  EXPECT_GT(after.late_response_quarantines, before.late_response_quarantines);
  EXPECT_GT(after.late_response_bytes_discarded, before.late_response_bytes_discarded);
  EXPECT_NE(evidence.str().find("late_response_quarantine_started"), std::string::npos);
  EXPECT_NE(evidence.str().find("late_response_discarded"), std::string::npos);
}

TEST(GatewayRuntimePtyTest, StopDuringInFlightRequestIsCancellationNotSerialFailure) {
  PtyBusHarness bus(kRegisterMap, kScenarioMap);
  ASSERT_TRUE(bus.start()) << bus.last_error();
  ASSERT_TRUE(bus.set_fault(1U, FaultPlan{FaultMode::silent, 0U, 0U, 0U}));
  std::ostringstream evidence;
  auto config = runtime_config_for(bus.gateway_path());
  config.response_timeout = std::chrono::milliseconds(500);
  GatewayRuntime runtime(std::move(config), evidence);
  ASSERT_TRUE(runtime.start());
  ASSERT_TRUE(wait_until([&runtime] { return runtime.statistics().in_flight_requests == 1U; },
                         std::chrono::seconds(2)))
      << evidence.str();
  const auto errors_before = runtime.statistics().serial_errors;
  runtime.request_stop(ShutdownReason::service_stop);
  runtime.join();
  const auto after = runtime.statistics();
  EXPECT_TRUE(after.stopped);
  EXPECT_EQ(after.serial_errors, errors_before) << evidence.str();
  EXPECT_NE(evidence.str().find("shutdown_cancelled"), std::string::npos);
}

TEST(GatewayRuntimePtyTest, EmitsStaleOfflineAndRecoversPolling) {
  PtyBusHarness bus(kRegisterMap, kScenarioMap);
  ASSERT_TRUE(bus.start()) << bus.last_error();
  auto config = runtime_config_for(bus.gateway_path());
  config.response_timeout = std::chrono::milliseconds(80);
  config.scheduler_policy.reliability.response_timeout = std::chrono::milliseconds(80);
  config.scheduler_policy.reliability.max_attempts = 1U;
  config.scheduler_policy.reliability.backoff_initial = std::chrono::milliseconds(20);
  config.scheduler_policy.reliability.backoff_max = std::chrono::milliseconds(20);
  config.scheduler_policy.reliability.request_deadline = std::chrono::milliseconds(500);
  config.scheduler_policy.consecutive_final_failures_to_offline = 1U;
  config.scheduler_policy.offline_probe_interval = std::chrono::milliseconds(500);
  config.scheduler_policy.consecutive_probe_successes_to_recover = 1U;
  std::ostringstream evidence;
  auto logger = std::make_shared<industrial_iot_gateway::observability::JsonlLogWriter>(evidence);
  auto recorded = std::make_shared<RecordingState>();
  GatewayRuntime runtime(std::move(config), logger,
                         std::make_unique<RecordingPublishSink>(recorded));
  ASSERT_TRUE(runtime.valid());
  ASSERT_TRUE(runtime.start());
  ASSERT_TRUE(wait_until([&runtime] { return runtime.statistics().requests_succeeded >= 20U; },
                         std::chrono::seconds(3)));
  const auto successes_before = runtime.statistics().requests_succeeded;

  ASSERT_TRUE(bus.disconnect_and_reconnect(std::chrono::milliseconds(650))) << bus.last_error();
  const auto recovery_started = std::chrono::steady_clock::now();
  const auto recovered = wait_until(
      [&runtime, successes_before] {
        return runtime.statistics().requests_succeeded > successes_before;
      },
      std::chrono::seconds(5));
  const auto recovery_finished = std::chrono::steady_clock::now();
  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  runtime.request_stop(ShutdownReason::service_stop);
  runtime.join();
  ASSERT_TRUE(recovered) << evidence.str();
  std::cout << "FAULT_RECOVERY_TIME_MS="
            << std::chrono::duration_cast<std::chrono::milliseconds>(recovery_finished -
                                                                     recovery_started)
                   .count()
            << '\n';
  EXPECT_TRUE(
      has_retained_telemetry(*recorded, industrial_iot_gateway::quality::RegisterQuality::stale));
  EXPECT_TRUE(
      has_retained_telemetry(*recorded, industrial_iot_gateway::quality::RegisterQuality::offline));
  EXPECT_GT(runtime.statistics().requests_succeeded, successes_before);
}

} // namespace
