#include "industrial_iot_gateway/mqtt/mqtt_publish_sink.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <mqtt/async_client.h>

#include "industrial_iot_gateway/mqtt/payload_serializer.hpp"
#include "industrial_iot_gateway/mqtt/reconnect_policy.hpp"
#include "industrial_iot_gateway/pipeline/event_pipeline.hpp"

namespace industrial_iot_gateway::mqtt {
namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] bool valid_identifier(const std::string &value, std::size_t maximum,
                                    bool allow_underscore) {
  if (value.empty() || value.size() > maximum) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [allow_underscore](unsigned char character) {
    return std::isalnum(character) != 0 || (allow_underscore && character == '_');
  });
}

[[nodiscard]] bool is_critical(const pipeline::PublishMessage &message) noexcept {
  const auto *telemetry = std::get_if<pipeline::TelemetryEvent>(&message);
  return telemetry == nullptr || telemetry->quality != quality::RegisterQuality::fresh;
}

[[nodiscard]] bool is_expired_fresh(const pipeline::PublishMessage &message,
                                    Clock::time_point now) noexcept {
  const auto *telemetry = std::get_if<pipeline::TelemetryEvent>(&message);
  return telemetry != nullptr && telemetry->quality == quality::RegisterQuality::fresh &&
         telemetry->freshness_deadline.has_value() && now > *telemetry->freshness_deadline;
}

template <typename Event>
void ensure_identity(Event &event, const std::string &run_id, std::uint64_t sequence,
                     std::chrono::system_clock::time_point now) {
  if (event.run_id.empty()) {
    event.run_id = run_id;
  }
  if (event.sequence == 0U) {
    event.sequence = sequence;
  }
  if (event.source_timestamp.time_since_epoch().count() == 0) {
    event.source_timestamp = now;
  }
  if (event.gateway_timestamp.time_since_epoch().count() == 0) {
    event.gateway_timestamp = now;
  }
}

} // namespace

class MqttPublishSink::Impl {
public:
  Impl(MqttConfig supplied, std::shared_ptr<observability::JsonlLogWriter> supplied_logger)
      : config_(std::move(supplied)), logger_(std::move(supplied_logger)),
        serializer_(config_.gateway_id), queue_(config_.queue),
        random_(static_cast<std::mt19937::result_type>(std::hash<std::string>{}(config_.run_id))),
        jitter_(-1.0, 1.0) {
    valid_ = validate();
  }

  ~Impl() {
    request_stop(Clock::now());
    join();
  }

  [[nodiscard]] bool valid() const noexcept { return valid_; }

  [[nodiscard]] bool start() {
    bool expected = false;
    if (!valid_ || !started_.compare_exchange_strong(expected, true)) {
      return false;
    }
    try {
      worker_ = std::thread([this] { run(); });
      return true;
    } catch (...) {
      started_.store(false);
      return false;
    }
  }

  [[nodiscard]] concurrency::QueuePushStatus submit(pipeline::PublishMessage message) {
    if (stopping_.load()) {
      return concurrency::QueuePushStatus::closed;
    }
    enrich(message);
    remember_snapshot(message);
    const bool critical = is_critical(message);
    const auto status = queue_.push(std::move(message));
    {
      const std::lock_guard<std::mutex> lock(statistics_mutex_);
      if (status == concurrency::QueuePushStatus::coalesced) {
        ++statistics_.coalesced;
      } else if (status == concurrency::QueuePushStatus::full ||
                 status == concurrency::QueuePushStatus::closed ||
                 status == concurrency::QueuePushStatus::timed_out) {
        ++statistics_.dropped;
        if (critical) {
          ++statistics_.critical_enqueue_failures;
        }
      }
    }
    wake_.notify_one();
    return status;
  }

  void request_stop(Clock::time_point drain_deadline) noexcept {
    bool initiate_shutdown = false;
    {
      const std::lock_guard<std::mutex> lock(shutdown_mutex_);
      if (!stopping_.load()) {
        // Publish the deadline before the worker can observe stopping=true.
        // finish_shutdown() takes the same mutex before reading it.
        drain_deadline_ = drain_deadline;
        stopping_.store(true);
        initiate_shutdown = true;
      }
    }
    if (initiate_shutdown) {
      queue_.close();
      wake_.notify_all();
    }
  }

  void join() noexcept {
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  [[nodiscard]] publish::PublishSinkStatistics statistics() const {
    const std::lock_guard<std::mutex> lock(statistics_mutex_);
    auto copy = statistics_;
    copy.queue = queue_.statistics();
    return copy;
  }

private:
  [[nodiscard]] bool validate() const noexcept {
    return logger_ != nullptr && queue_.valid() && !config_.broker_uri.empty() &&
           valid_identifier(config_.client_id, 23U, false) &&
           valid_identifier(config_.gateway_id, 64U, true) && !config_.run_id.empty() &&
           config_.keep_alive.count() > 0 && config_.connect_timeout.count() > 0 &&
           config_.publish_ack_timeout.count() > 0 && config_.drain_timeout.count() > 0;
  }

  void emit(const std::string &event, observability::LogSeverity severity,
            const std::string &reason = {}) const {
    observability::StructuredEvent value{};
    value.event = event;
    value.component = "mqtt";
    value.severity = severity;
    value.reason = reason;
    value.queue_depth = queue_.statistics().current_depth;
    static_cast<void>(logger_->write(value));
  }

  [[nodiscard]] std::uint64_t next_sequence() noexcept { return next_sequence_.fetch_add(1U); }

  void enrich(pipeline::PublishMessage &message) {
    const auto now = std::chrono::system_clock::now();
    std::visit(
        [&](auto &event) {
          using Event = std::decay_t<decltype(event)>;
          if constexpr (std::is_same_v<Event, pipeline::TelemetryEvent> ||
                        std::is_same_v<Event, pipeline::DeviceStatusEvent> ||
                        std::is_same_v<Event, pipeline::GatewayHealthEvent>) {
            ensure_identity(event, config_.run_id, next_sequence(), now);
          }
        },
        message);
  }

  void remember_snapshot(const pipeline::PublishMessage &message) {
    const std::lock_guard<std::mutex> lock(snapshot_mutex_);
    if (const auto *telemetry = std::get_if<pipeline::TelemetryEvent>(&message)) {
      telemetry_snapshots_[telemetry->topic] = *telemetry;
    } else if (const auto *status = std::get_if<pipeline::DeviceStatusEvent>(&message)) {
      device_snapshots_[status->device_name] = *status;
    }
  }

  [[nodiscard]] pipeline::GatewayHealthEvent health(std::string state, std::string reason,
                                                    bool lwt) {
    pipeline::GatewayHealthEvent event{};
    const auto sequence = next_sequence();
    event.event_id = sequence;
    event.state = std::move(state);
    event.reason = std::move(reason);
    event.lwt = lwt;
    ensure_identity(event, config_.run_id, sequence, std::chrono::system_clock::now());
    return event;
  }

  [[nodiscard]] ::mqtt::connect_options connect_options() {
    const auto lwt = serializer_.serialize(
        pipeline::PublishMessage{health("offline", "unexpected_disconnect", true)});
    ::mqtt::connect_options options{};
    options.set_mqtt_version(MQTTVERSION_3_1_1);
    options.set_clean_session(true);
    options.set_automatic_reconnect(false);
    options.set_keep_alive_interval(config_.keep_alive);
    options.set_connect_timeout(config_.connect_timeout);
    options.set_max_inflight(1);
    if (lwt.has_value()) {
      options.set_will(::mqtt::will_options{lwt->topic, lwt->payload, lwt->qos, lwt->retain});
    }
    return options;
  }

  [[nodiscard]] bool announce_connected() {
    {
      const std::lock_guard<std::mutex> lock(statistics_mutex_);
      if (statistics_.connected) {
        return true;
      }
      statistics_.connected = true;
      ++statistics_.connected_events;
    }
    reconnect_failures_ = 0U;
    emit("mqtt_connected", observability::LogSeverity::info);
    if (!publish_direct(pipeline::PublishMessage{health("online", "started", false)},
                        Clock::now() + config_.publish_ack_timeout)) {
      return false;
    }
    publish_snapshots();
    return client_->is_connected();
  }

  [[nodiscard]] bool connect() {
    {
      const std::lock_guard<std::mutex> lock(statistics_mutex_);
      ++statistics_.connect_attempts;
    }
    emit("mqtt_connect_attempt", observability::LogSeverity::info);
    try {
      auto token = client_->connect(connect_options());
      const auto deadline = Clock::now() + config_.connect_timeout;
      while (!stopping_.load() && Clock::now() < deadline) {
        if (token->wait_for(std::chrono::milliseconds{25})) {
          break;
        }
      }
      if (!client_->is_connected()) {
        emit("mqtt_connect_failed", observability::LogSeverity::warning,
             stopping_.load() ? "shutdown" : "timeout");
        return false;
      }
      return announce_connected();
    } catch (const ::mqtt::exception &error) {
      emit("mqtt_connect_failed", observability::LogSeverity::warning, error.what());
      return false;
    }
  }

  void mark_disconnected(const std::string &reason) {
    bool was_connected{};
    {
      const std::lock_guard<std::mutex> lock(statistics_mutex_);
      was_connected = statistics_.connected;
      statistics_.connected = false;
      if (was_connected) {
        ++statistics_.disconnected_events;
      }
    }
    if (was_connected) {
      emit("mqtt_disconnected", observability::LogSeverity::warning, reason);
    }
  }

  void schedule_reconnect() {
    const auto delay =
        reconnect_policy_.jittered_delay(reconnect_failures_, JitterSample{jitter_(random_)});
    ++reconnect_failures_;
    next_connect_at_ = Clock::now() + delay;
    {
      const std::lock_guard<std::mutex> lock(statistics_mutex_);
      ++statistics_.reconnect_scheduled;
    }
    emit("mqtt_reconnect_scheduled", observability::LogSeverity::info,
         std::to_string(delay.count()));
  }

  [[nodiscard]] bool publish_direct(const pipeline::PublishMessage &message,
                                    Clock::time_point deadline) {
    const auto publication = serializer_.serialize(message);
    if (!publication.has_value()) {
      return true;
    }
    {
      const std::lock_guard<std::mutex> lock(statistics_mutex_);
      ++statistics_.publish_attempts;
    }
    emit("mqtt_publish_attempt", observability::LogSeverity::debug);
    try {
      // Paho signals token completion from its receive thread. Keep two generations alive so the
      // callback that signalled the previous token has returned before that token is destroyed.
      // This is bounded because max_inflight is fixed to one.
      previous_publish_token_ = std::move(active_publish_token_);
      active_publish_token_ =
          client_->publish(publication->topic, publication->payload.data(),
                           publication->payload.size(), publication->qos, publication->retain);
      while (Clock::now() < deadline) {
        if (active_publish_token_->wait_for(std::chrono::milliseconds{25})) {
          const std::lock_guard<std::mutex> lock(statistics_mutex_);
          ++statistics_.publish_successes;
          return true;
        }
        if (!client_->is_connected()) {
          break;
        }
      }
    } catch (const ::mqtt::exception &error) {
      emit("mqtt_publish_failed", observability::LogSeverity::warning, error.what());
    }
    {
      const std::lock_guard<std::mutex> lock(statistics_mutex_);
      ++statistics_.publish_failures;
    }
    mark_disconnected("publish_failed");
    return false;
  }

  void publish_snapshots() {
    std::vector<pipeline::PublishMessage> snapshots{};
    {
      const std::lock_guard<std::mutex> lock(snapshot_mutex_);
      snapshots.reserve(device_snapshots_.size() + telemetry_snapshots_.size());
      for (auto &[name, status] : device_snapshots_) {
        static_cast<void>(name);
        auto copy = status;
        copy.sequence = 0U;
        copy.gateway_timestamp = std::chrono::system_clock::now();
        snapshots.emplace_back(std::move(copy));
      }
      for (auto &[topic, telemetry] : telemetry_snapshots_) {
        static_cast<void>(topic);
        if (telemetry.quality != quality::RegisterQuality::fresh) {
          auto copy = telemetry;
          copy.sequence = 0U;
          copy.gateway_timestamp = std::chrono::system_clock::now();
          snapshots.emplace_back(std::move(copy));
        }
      }
    }
    for (auto &snapshot : snapshots) {
      enrich(snapshot);
      if (!publish_direct(snapshot, Clock::now() + config_.publish_ack_timeout)) {
        return;
      }
    }
  }

  void process_messages() {
    if (!pending_.has_value()) {
      auto result = queue_.wait_pop_until(Clock::now() + std::chrono::milliseconds{25});
      if (result.value.has_value()) {
        pending_ = std::move(*result.value);
      }
    }
    if (!pending_.has_value()) {
      return;
    }
    if (is_expired_fresh(*pending_, Clock::now())) {
      pending_.reset();
      const std::lock_guard<std::mutex> lock(statistics_mutex_);
      ++statistics_.expired_fresh_dropped;
      ++statistics_.dropped;
      return;
    }
    if (publish_direct(*pending_, Clock::now() + config_.publish_ack_timeout)) {
      pending_.reset();
    }
  }

  void finish_shutdown() {
    Clock::time_point deadline{};
    {
      const std::lock_guard<std::mutex> lock(shutdown_mutex_);
      deadline = drain_deadline_.value_or(Clock::now());
    }
    if (statistics().connected) {
      static_cast<void>(publish_direct(
          pipeline::PublishMessage{health("stopping", "graceful_shutdown", false)}, deadline));
      while (Clock::now() < deadline) {
        process_messages();
        if (!pending_.has_value() && queue_.statistics().current_depth == 0U) {
          break;
        }
      }
      static_cast<void>(publish_direct(
          pipeline::PublishMessage{health("offline", "graceful_shutdown", false)}, deadline));
    }
    const auto remaining = queue_.statistics().current_depth + (pending_.has_value() ? 1U : 0U);
    if (remaining > 0U || Clock::now() >= deadline) {
      const std::lock_guard<std::mutex> lock(statistics_mutex_);
      ++statistics_.drain_expired;
      statistics_.unconfirmed_on_close += remaining;
      statistics_.dropped += remaining;
      emit("mqtt_drain_expired", observability::LogSeverity::warning, std::to_string(remaining));
    }
    try {
      if (client_ != nullptr && client_->is_connected()) {
        disconnect_token_ = client_->disconnect();
        static_cast<void>(disconnect_token_->wait_for(std::chrono::milliseconds{100}));
      }
    } catch (const ::mqtt::exception &error) {
      emit("mqtt_disconnect_failed", observability::LogSeverity::warning, error.what());
    }
    mark_disconnected("closed");
  }

  void run() noexcept {
    try {
      client_ = std::make_unique<::mqtt::async_client>(config_.broker_uri, config_.client_id);
      next_connect_at_ = Clock::now();
      while (!stopping_.load()) {
        if (!client_->is_connected()) {
          mark_disconnected("connection_lost");
          const auto now = Clock::now();
          if (now >= next_connect_at_) {
            if (!connect()) {
              schedule_reconnect();
            }
          }
          std::unique_lock<std::mutex> lock(wake_mutex_);
          wake_.wait_until(lock,
                           std::min(next_connect_at_, Clock::now() + std::chrono::milliseconds{50}),
                           [this] { return stopping_.load(); });
          continue;
        }
        if (!statistics().connected && !announce_connected()) {
          continue;
        }
        process_messages();
      }
      finish_shutdown();
    } catch (const std::exception &error) {
      emit("mqtt_worker_failed", observability::LogSeverity::critical, error.what());
      const std::lock_guard<std::mutex> lock(statistics_mutex_);
      ++statistics_.publish_failures;
    } catch (...) {
      emit("mqtt_worker_failed", observability::LogSeverity::critical, "unknown");
      const std::lock_guard<std::mutex> lock(statistics_mutex_);
      ++statistics_.publish_failures;
    }
  }

  MqttConfig config_{};
  std::shared_ptr<observability::JsonlLogWriter> logger_{};
  PayloadSerializer serializer_;
  pipeline::PublishQueue queue_;
  ReconnectPolicy reconnect_policy_{};
  ::mqtt::token_ptr disconnect_token_{};
  ::mqtt::delivery_token_ptr previous_publish_token_{};
  ::mqtt::delivery_token_ptr active_publish_token_{};
  std::unique_ptr<::mqtt::async_client> client_{};
  std::thread worker_{};
  std::atomic<bool> started_{};
  std::atomic<bool> stopping_{};
  std::atomic<std::uint64_t> next_sequence_{1U};
  bool valid_{};
  mutable std::mutex shutdown_mutex_{};
  std::optional<Clock::time_point> drain_deadline_{};
  Clock::time_point next_connect_at_{};
  std::size_t reconnect_failures_{};
  std::optional<pipeline::PublishMessage> pending_{};
  mutable std::mutex statistics_mutex_{};
  publish::PublishSinkStatistics statistics_{};
  mutable std::mutex snapshot_mutex_{};
  std::unordered_map<std::string, pipeline::TelemetryEvent> telemetry_snapshots_{};
  std::unordered_map<std::string, pipeline::DeviceStatusEvent> device_snapshots_{};
  std::mutex wake_mutex_{};
  std::condition_variable wake_{};
  std::mt19937 random_;
  std::uniform_real_distribution<double> jitter_;
};

MqttPublishSink::MqttPublishSink(MqttConfig config,
                                 std::shared_ptr<observability::JsonlLogWriter> logger)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(logger))) {}

MqttPublishSink::~MqttPublishSink() = default;

bool MqttPublishSink::valid() const noexcept { return impl_->valid(); }

bool MqttPublishSink::start() { return impl_->start(); }

concurrency::QueuePushStatus MqttPublishSink::submit(pipeline::PublishMessage message) {
  return impl_->submit(std::move(message));
}

void MqttPublishSink::request_stop(Clock::time_point drain_deadline) noexcept {
  impl_->request_stop(drain_deadline);
}

void MqttPublishSink::join() noexcept { impl_->join(); }

publish::PublishSinkStatistics MqttPublishSink::statistics() const { return impl_->statistics(); }

} // namespace industrial_iot_gateway::mqtt
