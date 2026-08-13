#include "industrial_iot_gateway/mqtt/mqtt_config.hpp"
#include "industrial_iot_gateway/mqtt/mqtt_publish_sink.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <deque>
#include <fcntl.h>
#include <memory>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include <gtest/gtest.h>
namespace {

using industrial_iot_gateway::mqtt::MqttConfig;
using industrial_iot_gateway::mqtt::MqttPublishSink;
using industrial_iot_gateway::pipeline::DeviceStatusEvent;
using industrial_iot_gateway::pipeline::PublishMessage;
using industrial_iot_gateway::pipeline::TelemetryEvent;
using industrial_iot_gateway::quality::RegisterQuality;
using namespace std::chrono_literals;

const std::string kMosquitto = GATEWAY_MOSQUITTO_EXECUTABLE;
const std::string kMosquittoSub = GATEWAY_MOSQUITTO_SUB_EXECUTABLE;

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

class SubscriberProcess {
public:
  SubscriberProcess(std::uint16_t port, std::string client_id)
      : port_(port), client_id_(std::move(client_id)) {}
  ~SubscriberProcess() { stop(); }
  SubscriberProcess(const SubscriberProcess &) = delete;
  SubscriberProcess &operator=(const SubscriberProcess &) = delete;

  [[nodiscard]] bool start() {
    if (child_ > 0 || read_fd_ >= 0) {
      return false;
    }
    int pipe_fds[2]{};
    if (::pipe2(pipe_fds, O_CLOEXEC) != 0) {
      return false;
    }
    child_ = ::fork();
    if (child_ < 0) {
      static_cast<void>(::close(pipe_fds[0]));
      static_cast<void>(::close(pipe_fds[1]));
      return false;
    }
    if (child_ == 0) {
      static_cast<void>(::close(pipe_fds[0]));
      static_cast<void>(::dup2(pipe_fds[1], STDOUT_FILENO));
      static_cast<void>(::close(pipe_fds[1]));
      const auto null_fd = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
      if (null_fd >= 0) {
        static_cast<void>(::dup2(null_fd, STDERR_FILENO));
        static_cast<void>(::close(null_fd));
      }
      const auto port = std::to_string(port_);
      ::execl(kMosquittoSub.c_str(), kMosquittoSub.c_str(), "-h", "127.0.0.1", "-p", port.c_str(),
              "-t", "industrial_iot_gateway/#", "-q", "1", "-v", "-i", client_id_.c_str(),
              static_cast<char *>(nullptr));
      ::_exit(127);
    }
    static_cast<void>(::close(pipe_fds[1]));
    read_fd_ = pipe_fds[0];
    const auto flags = ::fcntl(read_fd_, F_GETFL, 0);
    if (flags < 0 || ::fcntl(read_fd_, F_SETFL, flags | O_NONBLOCK) != 0) {
      stop();
      return false;
    }
    std::this_thread::sleep_for(150ms);
    int status{};
    if (::waitpid(child_, &status, WNOHANG) == child_) {
      child_ = -1;
      static_cast<void>(::close(read_fd_));
      read_fd_ = -1;
      return false;
    }
    return true;
  }

  [[nodiscard]] bool consume_matching(const std::string &topic, const std::string &payload_marker,
                                      std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      drain_messages();
      const auto match = std::find_if(messages_.begin(), messages_.end(), [&](const auto &message) {
        return message.first == topic && message.second.find(payload_marker) != std::string::npos;
      });
      if (match != messages_.end()) {
        messages_.erase(match);
        return true;
      }
      std::this_thread::sleep_for(20ms);
    }
    drain_messages();
    return std::any_of(messages_.begin(), messages_.end(), [&](const auto &message) {
      return message.first == topic && message.second.find(payload_marker) != std::string::npos;
    });
  }

  void stop() noexcept {
    if (child_ > 0) {
      static_cast<void>(::kill(child_, SIGTERM));
      const auto deadline = std::chrono::steady_clock::now() + 2s;
      int status{};
      while (std::chrono::steady_clock::now() < deadline) {
        if (::waitpid(child_, &status, WNOHANG) == child_) {
          child_ = -1;
          break;
        }
        std::this_thread::sleep_for(20ms);
      }
      if (child_ > 0) {
        static_cast<void>(::kill(child_, SIGKILL));
        static_cast<void>(::waitpid(child_, &status, 0));
        child_ = -1;
      }
    }
    if (read_fd_ >= 0) {
      static_cast<void>(::close(read_fd_));
      read_fd_ = -1;
    }
  }

private:
  void drain_messages() {
    char bytes[4096]{};
    while (read_fd_ >= 0) {
      const auto count = ::read(read_fd_, bytes, sizeof(bytes));
      if (count > 0) {
        buffer_.append(bytes, static_cast<std::size_t>(count));
        continue;
      }
      if (count < 0 && errno == EINTR) {
        continue;
      }
      break;
    }
    std::size_t newline{};
    while ((newline = buffer_.find('\n')) != std::string::npos) {
      auto line = buffer_.substr(0U, newline);
      buffer_.erase(0U, newline + 1U);
      const auto separator = line.find(' ');
      if (separator != std::string::npos) {
        messages_.emplace_back(line.substr(0U, separator), line.substr(separator + 1U));
      }
    }
  }

  std::uint16_t port_{};
  std::string client_id_{};
  pid_t child_{-1};
  int read_fd_{-1};
  std::string buffer_{};
  std::deque<std::pair<std::string, std::string>> messages_{};
};

[[nodiscard]] TelemetryEvent telemetry(RegisterQuality quality, std::string reason,
                                       std::chrono::steady_clock::time_point deadline) {
  TelemetryEvent event{};
  event.event_id = 1U;
  event.request_id = 42U;
  event.topic = "industrial_iot_gateway/devices/environment_sensor/registers/ambient_temperature_c";
  event.quality = quality;
  event.quality_reason = std::move(reason);
  event.value_is_retained = quality != RegisterQuality::fresh;
  event.value = 25.3;
  event.device_name = "environment_sensor";
  event.slave_id = 1U;
  event.register_name = "ambient_temperature_c";
  event.raw_value = std::int64_t{253};
  event.unit = "degC";
  event.source_timestamp = std::chrono::system_clock::now();
  event.gateway_timestamp = event.source_timestamp;
  event.freshness_deadline = deadline;
  return event;
}

TEST(MqttBrokerIntegrationTest, ReconnectsDrainsCriticalStateAndDropsExpiredFreshTelemetry) {
  const auto port = static_cast<std::uint16_t>(19000U + (::getpid() % 1000));
  const auto broker_uri = "tcp://127.0.0.1:" + std::to_string(port);
  BrokerProcess broker(port);
  ASSERT_TRUE(broker.start());

  std::ostringstream events;
  auto logger = std::make_shared<industrial_iot_gateway::observability::JsonlLogWriter>(events);
  MqttConfig config{};
  config.broker_uri = broker_uri;
  config.client_id = "iiotgwtest";
  config.gateway_id = "lab_gateway_01";
  config.run_id = "8b2144dc-3701-4b80-958f-2bcf918cf307";
  config.connect_timeout = 1s;
  config.publish_ack_timeout = 1s;
  config.queue = {8U, 6U};
  MqttPublishSink sink(std::move(config), logger);
  ASSERT_TRUE(sink.valid());
  ASSERT_TRUE(sink.start());
  ASSERT_TRUE(wait_until([&sink] { return sink.statistics().connected_events >= 1U; }, 3s))
      << events.str();

  SubscriberProcess first_observer(port, "observera" + std::to_string(::getpid()));
  ASSERT_TRUE(first_observer.start());
  const auto initial =
      telemetry(RegisterQuality::fresh, "valid_sample", std::chrono::steady_clock::now() + 2s);
  const auto topic = initial.topic;
  EXPECT_EQ(sink.submit(PublishMessage{initial}),
            industrial_iot_gateway::concurrency::QueuePushStatus::accepted);
  EXPECT_TRUE(first_observer.consume_matching(topic, "\"quality\":\"fresh\"", 2s));
  first_observer.stop();

  broker.stop();
  ASSERT_TRUE(wait_until([&sink] { return sink.statistics().disconnected_events >= 1U; }, 3s));

  auto expired =
      telemetry(RegisterQuality::fresh, "valid_sample", std::chrono::steady_clock::now() + 100ms);
  EXPECT_EQ(sink.submit(PublishMessage{std::move(expired)}),
            industrial_iot_gateway::concurrency::QueuePushStatus::accepted);
  auto stale =
      telemetry(RegisterQuality::stale, "freshness_expired", std::chrono::steady_clock::now());
  EXPECT_EQ(sink.submit(PublishMessage{stale}),
            industrial_iot_gateway::concurrency::QueuePushStatus::accepted);
  DeviceStatusEvent status{};
  status.event_id = 2U;
  status.device_name = "environment_sensor";
  status.slave_id = 1U;
  status.state = "offline";
  status.reason = "consecutive_final_failures";
  status.source_timestamp = std::chrono::system_clock::now();
  status.gateway_timestamp = status.source_timestamp;
  EXPECT_EQ(sink.submit(PublishMessage{status}),
            industrial_iot_gateway::concurrency::QueuePushStatus::accepted);
  std::this_thread::sleep_for(200ms);

  ASSERT_TRUE(broker.start());
  SubscriberProcess second_observer(port, "observerb" + std::to_string(::getpid()));
  ASSERT_TRUE(second_observer.start());
  ASSERT_TRUE(wait_until([&sink] { return sink.statistics().connected_events >= 2U; }, 4s))
      << events.str();
  EXPECT_TRUE(second_observer.consume_matching(topic, "\"quality\":\"stale\"", 2s));
  EXPECT_TRUE(second_observer.consume_matching(
      "industrial_iot_gateway/devices/environment_sensor/status", "\"state\":\"offline\"", 2s));

  const auto shutdown_started = std::chrono::steady_clock::now();
  sink.request_stop(shutdown_started + 2s);
  sink.join();
  const auto shutdown_elapsed = std::chrono::steady_clock::now() - shutdown_started;
  const auto statistics = sink.statistics();
  EXPECT_LT(shutdown_elapsed, 2500ms);
  EXPECT_GE(statistics.reconnect_scheduled, 1U);
  EXPECT_GE(statistics.connected_events, 2U);
  EXPECT_GE(statistics.publish_successes, 5U);
  EXPECT_GE(statistics.expired_fresh_dropped, 1U);
  EXPECT_EQ(statistics.critical_enqueue_failures, 0U);
  EXPECT_EQ(statistics.drain_expired, 0U);
  second_observer.stop();
}

} // namespace
