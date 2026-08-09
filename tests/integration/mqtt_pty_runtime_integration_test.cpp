#include "industrial_iot_gateway/config/runtime_config.hpp"
#include "industrial_iot_gateway/mqtt/mqtt_config.hpp"
#include "industrial_iot_gateway/mqtt/mqtt_publish_sink.hpp"
#include "industrial_iot_gateway/runtime/gateway_runtime.hpp"
#include "pty_bus_harness.hpp"

#include <chrono>
#include <csignal>
#include <cstdint>
#include <fcntl.h>
#include <memory>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>

#include <gtest/gtest.h>

namespace {

using industrial_iot_gateway::config::load_runtime_configuration;
using industrial_iot_gateway::lifecycle::ShutdownReason;
using industrial_iot_gateway::mqtt::MqttConfig;
using industrial_iot_gateway::mqtt::MqttPublishSink;
using industrial_iot_gateway::runtime::GatewayRuntime;
using industrial_iot_gateway::runtime::GatewayRuntimeConfig;
using industrial_iot_gateway::test_support::PtyBusHarness;
using namespace std::chrono_literals;

const std::string kMosquitto = GATEWAY_MOSQUITTO_EXECUTABLE;
const std::string kRegisterMap = GATEWAY_REGISTER_MAP_PATH;
const std::string kScenarioMap = GATEWAY_PTY_SCENARIO_PATH;

template <typename Predicate>
[[nodiscard]] bool wait_until(Predicate predicate, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(20ms);
  }
  return predicate();
}

class BrokerProcess {
public:
  explicit BrokerProcess(std::uint16_t port) : port_(port) {}
  ~BrokerProcess() { stop(); }
  BrokerProcess(const BrokerProcess &) = delete;
  BrokerProcess &operator=(const BrokerProcess &) = delete;

  [[nodiscard]] bool start() {
    if (child_ > 0) {
      return false;
    }
    child_ = ::fork();
    if (child_ < 0) {
      return false;
    }
    if (child_ == 0) {
      const auto null_fd = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
      if (null_fd >= 0) {
        static_cast<void>(::dup2(null_fd, STDOUT_FILENO));
        static_cast<void>(::dup2(null_fd, STDERR_FILENO));
        static_cast<void>(::close(null_fd));
      }
      const auto port = std::to_string(port_);
      ::execl(kMosquitto.c_str(), kMosquitto.c_str(), "-p", port.c_str(),
              static_cast<char *>(nullptr));
      ::_exit(127);
    }
    std::this_thread::sleep_for(100ms);
    int status{};
    if (::waitpid(child_, &status, WNOHANG) == child_) {
      child_ = -1;
      return false;
    }
    return true;
  }

  void stop() noexcept {
    if (child_ <= 0) {
      return;
    }
    static_cast<void>(::kill(child_, SIGTERM));
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    int status{};
    while (std::chrono::steady_clock::now() < deadline) {
      if (::waitpid(child_, &status, WNOHANG) == child_) {
        child_ = -1;
        return;
      }
      std::this_thread::sleep_for(20ms);
    }
    static_cast<void>(::kill(child_, SIGKILL));
    static_cast<void>(::waitpid(child_, &status, 0));
    child_ = -1;
  }

private:
  std::uint16_t port_{};
  pid_t child_{-1};
};

TEST(MqttPtyRuntimeIntegrationTest, SerialPollingContinuesWhileBrokerIsUnavailable) {
  const auto port = static_cast<std::uint16_t>(20000U + (::getpid() % 1000));
  BrokerProcess broker(port);
  ASSERT_TRUE(broker.start());
  PtyBusHarness bus(kRegisterMap, kScenarioMap);
  ASSERT_TRUE(bus.start()) << bus.last_error();

  auto loaded = load_runtime_configuration(kRegisterMap);
  ASSERT_TRUE(loaded) << loaded.detail;
  GatewayRuntimeConfig runtime_config{};
  runtime_config.serial.device_path = bus.gateway_path();
  runtime_config.registers = std::move(*loaded.configuration);
  runtime_config.response_timeout = 180ms;
  runtime_config.serial_reopen_backoff = 20ms;

  std::ostringstream events;
  auto logger = std::make_shared<industrial_iot_gateway::observability::JsonlLogWriter>(events);
  MqttConfig mqtt_config{};
  mqtt_config.broker_uri = "tcp://127.0.0.1:" + std::to_string(port);
  mqtt_config.client_id = "iiotgwptytest";
  mqtt_config.gateway_id = "lab_gateway_01";
  mqtt_config.run_id = "8b2144dc-3701-4b80-958f-2bcf918cf307";
  mqtt_config.connect_timeout = 1s;
  mqtt_config.publish_ack_timeout = 1s;
  mqtt_config.queue = {64U, 48U};
  auto sink = std::make_unique<MqttPublishSink>(std::move(mqtt_config), logger);
  GatewayRuntime runtime(std::move(runtime_config), logger, std::move(sink));
  ASSERT_TRUE(runtime.valid());
  ASSERT_TRUE(runtime.start());
  ASSERT_TRUE(wait_until(
      [&runtime] {
        const auto statistics = runtime.statistics();
        return statistics.publisher.connected_events >= 1U && statistics.requests_succeeded >= 15U;
      },
      5s))
      << events.str();

  broker.stop();
  ASSERT_TRUE(wait_until(
      [&runtime] { return runtime.statistics().publisher.disconnected_events >= 1U; }, 3s));
  const auto successes_before = runtime.statistics().requests_succeeded;
  ASSERT_TRUE(wait_until(
      [&runtime, successes_before] {
        return runtime.statistics().requests_succeeded >= successes_before + 10U;
      },
      3s))
      << events.str();

  ASSERT_TRUE(broker.start());
  ASSERT_TRUE(
      wait_until([&runtime] { return runtime.statistics().publisher.connected_events >= 2U; }, 5s))
      << events.str();
  runtime.request_stop(ShutdownReason::service_stop);
  runtime.join();
  const auto statistics = runtime.statistics();
  EXPECT_TRUE(statistics.stopped);
  EXPECT_GE(statistics.requests_succeeded, successes_before + 10U);
  EXPECT_GE(statistics.publisher.connected_events, 2U);
  EXPECT_GE(statistics.publisher.reconnect_scheduled, 1U);
  EXPECT_EQ(statistics.publisher.critical_enqueue_failures, 0U);
  EXPECT_EQ(statistics.publisher.drain_expired, 0U);
}

} // namespace
