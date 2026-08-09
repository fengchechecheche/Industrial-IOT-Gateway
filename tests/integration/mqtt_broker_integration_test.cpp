#include "industrial_iot_gateway/mqtt/mqtt_config.hpp"
#include "industrial_iot_gateway/mqtt/mqtt_publish_sink.hpp"

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

#include <gtest/gtest.h>
#include <mqtt/async_client.h>

namespace {

using industrial_iot_gateway::mqtt::MqttConfig;
using industrial_iot_gateway::mqtt::MqttPublishSink;
using industrial_iot_gateway::pipeline::DeviceStatusEvent;
using industrial_iot_gateway::pipeline::PublishMessage;
using industrial_iot_gateway::pipeline::TelemetryEvent;
using industrial_iot_gateway::quality::RegisterQuality;
using namespace std::chrono_literals;

const std::string kMosquitto = GATEWAY_MOSQUITTO_EXECUTABLE;

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

[[nodiscard]] ::mqtt::connect_options clean_session_options() {
  ::mqtt::connect_options options{};
  options.set_mqtt_version(MQTTVERSION_3_1_1);
  options.set_clean_session(true);
  options.set_connect_timeout(1);
  return options;
}

[[nodiscard]] bool connect_subscriber(::mqtt::async_client &client,
                                      std::chrono::milliseconds timeout) {
  return wait_until(
      [&client] {
        try {
          auto token = client.connect(clean_session_options());
          return token->wait_for(1s) && client.is_connected();
        } catch (const ::mqtt::exception &) {
          return false;
        }
      },
      timeout);
}

[[nodiscard]] bool consume_matching(::mqtt::async_client &client, const std::string &topic,
                                    const std::string &payload_marker,
                                    std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto message = client.try_consume_message_for(100ms);
    if (message != nullptr && message->get_topic() == topic &&
        message->to_string().find(payload_marker) != std::string::npos) {
      return true;
    }
  }
  return false;
}

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

  ::mqtt::token_ptr first_disconnect{};
  ::mqtt::async_client first_subscriber(broker_uri, "observera" + std::to_string(::getpid()));
  first_subscriber.start_consuming();
  ASSERT_TRUE(connect_subscriber(first_subscriber, 2s));
  ASSERT_TRUE(first_subscriber.subscribe("industrial_iot_gateway/#", 1)->wait_for(1s));
  const auto initial =
      telemetry(RegisterQuality::fresh, "valid_sample", std::chrono::steady_clock::now() + 2s);
  const auto topic = initial.topic;
  EXPECT_EQ(sink.submit(PublishMessage{initial}),
            industrial_iot_gateway::concurrency::QueuePushStatus::accepted);
  EXPECT_TRUE(consume_matching(first_subscriber, topic, "\"quality\":\"fresh\"", 2s));
  first_disconnect = first_subscriber.disconnect();
  static_cast<void>(first_disconnect->wait_for(1s));
  first_subscriber.stop_consuming();

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
  ::mqtt::token_ptr second_disconnect{};
  ::mqtt::async_client second_subscriber(broker_uri, "observerb" + std::to_string(::getpid()));
  second_subscriber.start_consuming();
  ASSERT_TRUE(connect_subscriber(second_subscriber, 2s));
  ASSERT_TRUE(second_subscriber.subscribe("industrial_iot_gateway/#", 1)->wait_for(1s));
  ASSERT_TRUE(wait_until([&sink] { return sink.statistics().connected_events >= 2U; }, 4s))
      << events.str();
  EXPECT_TRUE(consume_matching(second_subscriber, topic, "\"quality\":\"stale\"", 2s));
  EXPECT_TRUE(consume_matching(second_subscriber,
                               "industrial_iot_gateway/devices/environment_sensor/status",
                               "\"state\":\"offline\"", 2s));

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
  second_disconnect = second_subscriber.disconnect();
  static_cast<void>(second_disconnect->wait_for(1s));
  second_subscriber.stop_consuming();
}

} // namespace
