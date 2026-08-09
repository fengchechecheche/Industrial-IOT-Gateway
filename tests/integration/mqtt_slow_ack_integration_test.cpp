#include "industrial_iot_gateway/config/runtime_config.hpp"
#include "industrial_iot_gateway/mqtt/mqtt_config.hpp"
#include "industrial_iot_gateway/mqtt/mqtt_publish_sink.hpp"
#include "industrial_iot_gateway/runtime/gateway_runtime.hpp"
#include "pty_bus_harness.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <netinet/in.h>
#include <poll.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

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
    return ::waitpid(child_, &status, WNOHANG) != child_;
  }

  void stop() noexcept {
    if (child_ <= 0) {
      return;
    }
    static_cast<void>(::kill(child_, SIGTERM));
    static_cast<void>(::waitpid(child_, nullptr, 0));
    child_ = -1;
  }

private:
  std::uint16_t port_{};
  pid_t child_{-1};
};

class SlowAckProxy {
public:
  struct Endpoints {
    std::uint16_t listen_port{};
    std::uint16_t upstream_port{};
  };

  explicit SlowAckProxy(Endpoints endpoints)
      : listen_port_(endpoints.listen_port), upstream_port_(endpoints.upstream_port) {}
  ~SlowAckProxy() { stop(); }
  SlowAckProxy(const SlowAckProxy &) = delete;
  SlowAckProxy &operator=(const SlowAckProxy &) = delete;

  [[nodiscard]] bool start() {
    const auto descriptor = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (descriptor < 0) {
      return false;
    }
    int reuse = 1;
    static_cast<void>(::setsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(listen_port_);
    if (::bind(descriptor, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
        ::listen(descriptor, 1) != 0) {
      static_cast<void>(::close(descriptor));
      return false;
    }
    listen_fd_.store(descriptor);
    worker_ = std::thread([this, descriptor] { relay_loop(descriptor); });
    return true;
  }

  void pause_server_to_client(bool paused) noexcept { paused_.store(paused); }

  void stop() noexcept {
    if (stopping_.exchange(true)) {
      return;
    }
    for (const auto descriptor : {listen_fd_.load(), client_fd_.load(), upstream_fd_.load()}) {
      if (descriptor >= 0) {
        static_cast<void>(::shutdown(descriptor, SHUT_RDWR));
      }
    }
    if (worker_.joinable()) {
      worker_.join();
    }
  }

private:
  struct RelayEndpoints {
    int source{-1};
    int destination{-1};
  };

  static bool relay(RelayEndpoints endpoints) {
    std::array<std::uint8_t, 4096U> buffer{};
    const auto count = ::recv(endpoints.source, buffer.data(), buffer.size(), 0);
    if (count <= 0) {
      return false;
    }
    std::size_t offset{};
    while (offset < static_cast<std::size_t>(count)) {
      const auto written = ::send(endpoints.destination, buffer.data() + offset,
                                  static_cast<std::size_t>(count) - offset, MSG_NOSIGNAL);
      if (written <= 0) {
        return false;
      }
      offset += static_cast<std::size_t>(written);
    }
    return true;
  }

  void relay_loop(int listener) noexcept {
    const auto client = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
    client_fd_.store(client);
    if (client < 0 || stopping_.load()) {
      close_all(listener, client, -1);
      return;
    }
    const auto upstream = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    upstream_fd_.store(upstream);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(upstream_port_);
    if (upstream < 0 ||
        ::connect(upstream, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
      close_all(listener, client, upstream);
      return;
    }
    while (!stopping_.load()) {
      const auto upstream_events = static_cast<short>(paused_.load() ? 0 : POLLIN);
      std::array<pollfd, 2U> descriptors{{{client, POLLIN, 0}, {upstream, upstream_events, 0}}};
      const auto ready = ::poll(descriptors.data(), descriptors.size(), 20);
      if (ready < 0) {
        break;
      }
      if ((descriptors[0].revents & POLLIN) != 0 && !relay({client, upstream})) {
        break;
      }
      if ((descriptors[1].revents & POLLIN) != 0 && !relay({upstream, client})) {
        break;
      }
      if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 ||
          (descriptors[1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        break;
      }
    }
    close_all(listener, client, upstream);
  }

  void close_all(int listener, int client, int upstream) noexcept {
    for (const auto descriptor : {upstream, client, listener}) {
      if (descriptor >= 0) {
        static_cast<void>(::close(descriptor));
      }
    }
    listen_fd_.store(-1);
    client_fd_.store(-1);
    upstream_fd_.store(-1);
  }

  std::uint16_t listen_port_{};
  std::uint16_t upstream_port_{};
  std::atomic<bool> paused_{};
  std::atomic<bool> stopping_{};
  std::atomic<int> listen_fd_{-1};
  std::atomic<int> client_fd_{-1};
  std::atomic<int> upstream_fd_{-1};
  std::thread worker_{};
};

TEST(FaultMatrixMqttTest, F11SlowPubackDoesNotBlockSerialPolling) {
  const auto broker_port = static_cast<std::uint16_t>(22000U + (::getpid() % 500));
  const auto proxy_port = static_cast<std::uint16_t>(22500U + (::getpid() % 500));
  BrokerProcess broker(broker_port);
  ASSERT_TRUE(broker.start());
  SlowAckProxy proxy({proxy_port, broker_port});
  ASSERT_TRUE(proxy.start());
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
  mqtt_config.broker_uri = "tcp://127.0.0.1:" + std::to_string(proxy_port);
  mqtt_config.client_id = "iiotgwslowack";
  mqtt_config.gateway_id = "lab_gateway_01";
  mqtt_config.run_id = "8b2144dc-3701-4b80-958f-2bcf918cf307";
  mqtt_config.connect_timeout = 1s;
  mqtt_config.publish_ack_timeout = 2s;
  // Fresh telemetry is capped at six entries while the remaining capacity is
  // reserved for stale/offline/status events that must not be silently lost.
  mqtt_config.queue = {64U, 6U};
  auto sink = std::make_unique<MqttPublishSink>(std::move(mqtt_config), logger);
  GatewayRuntime runtime(std::move(runtime_config), logger, std::move(sink));
  ASSERT_TRUE(runtime.valid());
  ASSERT_TRUE(runtime.start());
  ASSERT_TRUE(wait_until(
      [&runtime] {
        const auto statistics = runtime.statistics();
        return statistics.publisher.connected_events >= 1U && statistics.requests_succeeded >= 10U;
      },
      5s))
      << events.str();

  proxy.pause_server_to_client(true);
  const auto successes_before = runtime.statistics().requests_succeeded;
  ASSERT_TRUE(wait_until(
      [&runtime] { return runtime.statistics().publisher.queue.maximum_depth >= 6U; }, 3s))
      << events.str();
  EXPECT_TRUE(wait_until(
      [&runtime, successes_before] {
        return runtime.statistics().requests_succeeded >= successes_before + 10U;
      },
      3s));

  const auto published_before_recovery = runtime.statistics().publisher.publish_successes;
  proxy.pause_server_to_client(false);
  const auto recovery_started = std::chrono::steady_clock::now();
  const auto drained = wait_until(
      [&runtime, published_before_recovery] {
        const auto statistics = runtime.statistics();
        return statistics.publisher.publish_successes > published_before_recovery &&
               statistics.publisher.queue.current_depth < statistics.publisher.queue.maximum_depth;
      },
      4s);
  ASSERT_TRUE(drained) << events.str();
  std::cout << "FAULT_RECOVERY_TIME_MS="
            << std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - recovery_started)
                   .count()
            << '\n';
  runtime.request_stop(ShutdownReason::service_stop);
  runtime.join();
  const auto statistics = runtime.statistics();
  EXPECT_TRUE(statistics.stopped);
  EXPECT_LE(statistics.publisher.queue.maximum_depth, statistics.publisher.queue.capacity);
  EXPECT_EQ(statistics.publisher.critical_enqueue_failures, 0U);
  EXPECT_EQ(statistics.publisher.drain_expired, 0U);
}

} // namespace
