#include "industrial_iot_gateway/runtime/gateway_runtime.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>

#include "industrial_iot_gateway/observability/structured_log.hpp"
#include "industrial_iot_gateway/pipeline/event_pipeline.hpp"
#include "industrial_iot_gateway/pipeline/request_queue.hpp"
#include "industrial_iot_gateway/protocol/modbus_codec.hpp"
#include "industrial_iot_gateway/protocol/rtu_stream_parser.hpp"
#include "industrial_iot_gateway/publish/jsonl_publish_sink.hpp"
#include "industrial_iot_gateway/quality/freshness_tracker.hpp"

namespace industrial_iot_gateway::runtime {
namespace {

using RuntimeClock = std::chrono::steady_clock;
using RuntimeTimePoint = RuntimeClock::time_point;
constexpr auto kSchedulerFeedbackDeliveryTimeout = std::chrono::milliseconds(100);
constexpr auto kMaximumLateResponseTransfer = std::chrono::milliseconds(150);
constexpr auto kSchedulerFeedbackJitterGrace = std::chrono::milliseconds(50);

struct AttemptSentFeedback {
  std::uint64_t request_id{};
  std::uint32_t poll_job_id{};
  std::uint32_t attempt{};
  RuntimeTimePoint sent_at{};
};

using SchedulerFeedback = std::variant<AttemptSentFeedback, scheduler::AttemptResult>;

struct QualityControlEvent {
  std::uint32_t poll_job_id{};
  std::uint8_t slave_id{};
  scheduler::RequestResultCategory result{scheduler::RequestResultCategory::success};
  RuntimeTimePoint observed_at{};
  bool terminal{};
  bool device_state_changed{};
  scheduler::DeviceHealthState device_state{scheduler::DeviceHealthState::online};
};

struct AttemptOutcome {
  scheduler::RequestResultCategory category{scheduler::RequestResultCategory::response_timeout};
  std::optional<protocol::Response> response{};
  std::optional<std::uint8_t> exception_code{};
  bool requires_serial_reopen{};
};

struct LateResponseQuarantineOutcome {
  std::size_t bytes_discarded{};
  bool requires_serial_reopen{};
};

[[nodiscard]] std::uint8_t request_slave_id(const protocol::Request &request) noexcept {
  if (const auto *read = std::get_if<protocol::ReadRequest>(&request)) {
    return read->slave_id;
  }
  const auto *write = std::get_if<protocol::WriteSingleRegisterRequest>(&request);
  return write == nullptr ? 0U : write->slave_id;
}

[[nodiscard]] std::uint8_t request_function(const protocol::Request &request) noexcept {
  if (const auto *read = std::get_if<protocol::ReadRequest>(&request)) {
    return static_cast<std::uint8_t>(read->function);
  }
  return static_cast<std::uint8_t>(protocol::FunctionCode::write_single_register);
}

[[nodiscard]] std::uint16_t request_address(const protocol::Request &request) noexcept {
  if (const auto *read = std::get_if<protocol::ReadRequest>(&request)) {
    return read->start_address;
  }
  const auto *write = std::get_if<protocol::WriteSingleRegisterRequest>(&request);
  return write == nullptr ? 0U : write->register_address;
}

[[nodiscard]] std::uint64_t monotonic_microseconds(RuntimeTimePoint now) noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count());
}

[[nodiscard]] scheduler::RequestResultCategory
map_parser_error(protocol::ParserErrorCategory error) noexcept {
  switch (error) {
  case protocol::ParserErrorCategory::crc_mismatch:
    return scheduler::RequestResultCategory::crc_mismatch;
  case protocol::ParserErrorCategory::inter_character_timeout:
  case protocol::ParserErrorCategory::frame_too_short:
    return scheduler::RequestResultCategory::truncated_frame;
  default:
    return scheduler::RequestResultCategory::serial_io_transient;
  }
}

[[nodiscard]] std::string result_name(scheduler::RequestResultCategory result) {
  switch (result) {
  case scheduler::RequestResultCategory::success:
    return "success";
  case scheduler::RequestResultCategory::response_timeout:
    return "response_timeout";
  case scheduler::RequestResultCategory::crc_mismatch:
    return "crc_mismatch";
  case scheduler::RequestResultCategory::truncated_frame:
    return "truncated_frame";
  case scheduler::RequestResultCategory::serial_io_transient:
    return "serial_io_transient";
  case scheduler::RequestResultCategory::remote_exception:
    return "remote_exception";
  case scheduler::RequestResultCategory::invalid_configuration:
    return "invalid_configuration";
  case scheduler::RequestResultCategory::broadcast_unsupported:
    return "broadcast_unsupported";
  case scheduler::RequestResultCategory::deadline_exceeded:
    return "deadline_exceeded";
  case scheduler::RequestResultCategory::shutdown_cancelled:
    return "shutdown_cancelled";
  }
  return "unknown";
}

[[nodiscard]] std::string scheduler_error_name(scheduler::SchedulerErrorCategory error) {
  switch (error) {
  case scheduler::SchedulerErrorCategory::none:
    return "none";
  case scheduler::SchedulerErrorCategory::empty_poll_plan:
    return "empty_poll_plan";
  case scheduler::SchedulerErrorCategory::duplicate_poll_job_id:
    return "duplicate_poll_job_id";
  case scheduler::SchedulerErrorCategory::invalid_slave_address:
    return "invalid_slave_address";
  case scheduler::SchedulerErrorCategory::unsupported_poll_function:
    return "unsupported_poll_function";
  case scheduler::SchedulerErrorCategory::invalid_poll_quantity:
    return "invalid_poll_quantity";
  case scheduler::SchedulerErrorCategory::address_range_overflow:
    return "address_range_overflow";
  case scheduler::SchedulerErrorCategory::invalid_poll_period:
    return "invalid_poll_period";
  case scheduler::SchedulerErrorCategory::invalid_policy_configuration:
    return "invalid_policy_configuration";
  case scheduler::SchedulerErrorCategory::invalid_device_health_configuration:
    return "invalid_device_health_configuration";
  case scheduler::SchedulerErrorCategory::no_in_flight_request:
    return "no_in_flight_request";
  case scheduler::SchedulerErrorCategory::request_id_mismatch:
    return "request_id_mismatch";
  case scheduler::SchedulerErrorCategory::poll_job_id_mismatch:
    return "poll_job_id_mismatch";
  case scheduler::SchedulerErrorCategory::attempt_mismatch:
    return "attempt_mismatch";
  case scheduler::SchedulerErrorCategory::invalid_request_state:
    return "invalid_request_state";
  case scheduler::SchedulerErrorCategory::invalid_observation_times:
    return "invalid_observation_times";
  case scheduler::SchedulerErrorCategory::request_id_exhausted:
    return "request_id_exhausted";
  }
  return "unknown";
}

[[nodiscard]] std::string queue_push_status_name(concurrency::QueuePushStatus status) {
  switch (status) {
  case concurrency::QueuePushStatus::accepted:
    return "accepted";
  case concurrency::QueuePushStatus::coalesced:
    return "coalesced";
  case concurrency::QueuePushStatus::full:
    return "full";
  case concurrency::QueuePushStatus::closed:
    return "closed";
  case concurrency::QueuePushStatus::timed_out:
    return "timed_out";
  }
  return "unknown";
}

} // namespace

class GatewayRuntime::Impl {
public:
  Impl(GatewayRuntimeConfig supplied,
       std::shared_ptr<observability::JsonlLogWriter> supplied_logger,
       std::unique_ptr<publish::PublishSink> supplied_sink)
      : config(std::move(supplied)), log_writer(std::move(supplied_logger)),
        publish_sink(std::move(supplied_sink)), request_queue(config.request_queue),
        measurement_queue(config.measurement_queue), scheduler_feedback({256U, 205U}),
        quality_control({256U, 205U}) {
    if (publish_sink == nullptr && log_writer != nullptr) {
      publish_sink = std::make_unique<publish::JsonlPublishSink>(log_writer);
    }
    validate();
  }

  ~Impl() {
    request_stop(lifecycle::ShutdownReason::service_stop);
    join();
  }

  void validate() {
    if (log_writer == nullptr || publish_sink == nullptr || !publish_sink->valid()) {
      error = RuntimeErrorCategory::invalid_publish_sink;
      return;
    }
    if (config.serial.device_path.empty()) {
      error = RuntimeErrorCategory::invalid_serial_configuration;
      return;
    }
    if (config.registers.device_count == 0U || config.registers.registers.empty() ||
        config.registers.poll_jobs.empty() || config.registers.freshness.empty() ||
        config.registers.registers.size() != config.registers.poll_jobs.size() ||
        config.registers.registers.size() != config.registers.freshness.size()) {
      error = RuntimeErrorCategory::invalid_register_configuration;
      return;
    }
    if (config.response_timeout.count() <= 0 || config.late_response_guard.count() <= 0 ||
        config.late_response_guard > std::chrono::seconds(1) ||
        config.minimum_request_interval.count() < 0 ||
        config.minimum_request_interval > std::chrono::seconds(10) ||
        config.serial_reopen_backoff.count() <= 0) {
      error = RuntimeErrorCategory::invalid_timing;
      return;
    }
    if (!request_queue.valid()) {
      error = RuntimeErrorCategory::invalid_request_queue_configuration;
      return;
    }
    if (!measurement_queue.valid()) {
      error = RuntimeErrorCategory::invalid_measurement_queue_configuration;
      return;
    }
    scheduler::PollScheduler scheduler_probe(config.registers.poll_jobs, RuntimeClock::now(),
                                             config.scheduler_policy);
    if (!scheduler_probe.valid()) {
      error = RuntimeErrorCategory::invalid_scheduler_configuration;
    }
  }

  [[nodiscard]] bool start() {
    if (error != RuntimeErrorCategory::none) {
      return false;
    }
    bool expected = false;
    if (!started.compare_exchange_strong(expected, true)) {
      error = RuntimeErrorCategory::already_started;
      return false;
    }
    stop_requested.store(false);
    {
      const std::lock_guard<std::mutex> lock(statistics_mutex);
      statistics_value.running = true;
      statistics_value.stopped = false;
    }
    try {
      if (!publish_sink->start()) {
        error = RuntimeErrorCategory::thread_start_failed;
        return false;
      }
      quality_thread = std::thread([this] { quality_loop(); });
      serial_thread = std::thread([this] { serial_loop(); });
      scheduler_thread = std::thread([this] { scheduler_loop(); });
      log("runtime_started", "lifecycle", observability::LogSeverity::info);
      return true;
    } catch (...) {
      error = RuntimeErrorCategory::thread_start_failed;
      request_stop(lifecycle::ShutdownReason::fatal_component_error);
      join();
      return false;
    }
  }

  [[nodiscard]] concurrency::QueuePushStatus
  submit_write(std::uint8_t slave_id, std::uint16_t address, std::uint16_t value) {
    if (!started.load() || stop_requested.load()) {
      return concurrency::QueuePushStatus::closed;
    }
    const auto now = RuntimeClock::now();
    scheduler::ScheduledRequest request{};
    request.request_id = (std::uint64_t{1U} << 63U) | next_explicit_request_id.fetch_add(1U);
    request.request_kind = scheduler::RequestKind::explicit_write;
    request.request = protocol::WriteSingleRegisterRequest{slave_id, address, value};
    request.due_at = now;
    request.created_at = now;
    request.enqueued_at = now;
    request.deadline = now + config.scheduler_policy.reliability.request_deadline;
    request.attempt_eligible_at = now;
    request.attempt = 1U;
    request.max_attempts = 1U;
    request.source = scheduler::RequestSource::local_test_runner;
    return request_queue.push(request);
  }

  void request_stop(lifecycle::ShutdownReason reason) noexcept {
    bool expected = false;
    if (!stop_requested.compare_exchange_strong(expected, true)) {
      return;
    }
    static_cast<void>(shutdown.request_stop(reason, RuntimeClock::now()));
    request_queue.close();
    scheduler_feedback.close();
    log("stop_requested", "lifecycle", observability::LogSeverity::info);
  }

  void join() noexcept {
    if (scheduler_thread.joinable()) {
      scheduler_thread.join();
    }
    if (serial_thread.joinable()) {
      serial_thread.join();
    }
    if (quality_thread.joinable()) {
      quality_thread.join();
    }
    publish_sink->request_stop(RuntimeClock::now() + std::chrono::milliseconds{2000});
    publish_sink->join();
    if (started.load()) {
      const std::lock_guard<std::mutex> lock(statistics_mutex);
      statistics_value.running = false;
      statistics_value.stopped = true;
    }
  }

  [[nodiscard]] GatewayRuntimeStatistics statistics() const {
    GatewayRuntimeStatistics copy{};
    {
      const std::lock_guard<std::mutex> lock(statistics_mutex);
      copy = statistics_value;
    }
    copy.request_queue = request_queue.statistics();
    copy.measurement_queue = measurement_queue.statistics();
    copy.scheduler_feedback_queue = scheduler_feedback.statistics();
    copy.publisher = publish_sink->statistics();
    copy.publish_queue = copy.publisher.queue;
    return copy;
  }

  [[nodiscard]] RuntimeErrorCategory validation_error() const noexcept { return error; }

private:
  void log(const std::string &event, const std::string &component,
           observability::LogSeverity severity,
           const scheduler::ScheduledRequest *request = nullptr,
           scheduler::RequestResultCategory result = scheduler::RequestResultCategory::success,
           std::optional<std::uint64_t> duration_ms = std::nullopt) {
    observability::StructuredEvent value{};
    value.event = event;
    value.component = component;
    value.severity = severity;
    value.result = result_name(result);
    value.monotonic_ms =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       RuntimeClock::now().time_since_epoch())
                                       .count());
    value.duration_ms = duration_ms;
    if (request != nullptr) {
      value.request_id = request->request_id;
      value.attempt = request->attempt;
      value.slave_id = request_slave_id(request->request);
      value.function = request_function(request->request);
      value.address = request_address(request->request);
    }
    static_cast<void>(log_writer->write(value));
  }

  [[nodiscard]] concurrency::QueuePushStatus publish_message(pipeline::PublishMessage message) {
    const bool telemetry = std::holds_alternative<pipeline::TelemetryEvent>(message);
    const bool transition = std::holds_alternative<pipeline::QualityTransitionEvent>(message);
    const auto status = publish_sink->submit(std::move(message));
    if (status == concurrency::QueuePushStatus::accepted ||
        status == concurrency::QueuePushStatus::coalesced) {
      const std::lock_guard<std::mutex> lock(statistics_mutex);
      if (telemetry) {
        ++statistics_value.telemetry_events;
      }
      if (transition) {
        ++statistics_value.quality_transitions;
      }
    }
    return status;
  }

  void scheduler_loop() noexcept {
    try {
      scheduler::PollScheduler poll_scheduler(config.registers.poll_jobs, RuntimeClock::now(),
                                              config.scheduler_policy);
      while (!stop_requested.load()) {
        auto feedback = scheduler_feedback.try_pop();
        if (feedback.status == concurrency::QueuePopStatus::item && feedback.value.has_value()) {
          process_scheduler_feedback(poll_scheduler, *feedback.value);
          continue;
        }

        const auto now = RuntimeClock::now();
        const auto next_wake = poll_scheduler.next_wake_time();
        const auto feedback_grace = config.late_response_guard + kMaximumLateResponseTransfer +
                                    kSchedulerFeedbackJitterGrace;
        if (poll_scheduler.has_in_flight_request() && next_wake.has_value() &&
            now >= *next_wake + feedback_grace) {
          auto advanced = poll_scheduler.on_time_advanced(now);
          if (!advanced.has_value()) {
            handle_scheduler_error(
                "scheduler_deadline_recovery_rejected",
                {scheduler::SchedulerErrorCategory::invalid_request_state, 0U, 0U}, 0U);
            continue;
          }
          {
            const std::lock_guard<std::mutex> lock(statistics_mutex);
            ++statistics_value.scheduler_deadline_recoveries;
          }
          log_scheduler_transition("scheduler_deadline_recovery", *advanced,
                                   observability::LogSeverity::warning);
          process_scheduler_transition(*advanced, now);
          continue;
        }
        if (auto request = poll_scheduler.dispatch_next(now); request.has_value()) {
          const auto status = request_queue.push(*request);
          if (status == concurrency::QueuePushStatus::accepted ||
              status == concurrency::QueuePushStatus::coalesced) {
            log("request_enqueued", "scheduler", observability::LogSeverity::debug, &*request);
          } else if (status != concurrency::QueuePushStatus::closed) {
            log("request_queue_rejected", "scheduler", observability::LogSeverity::error, &*request,
                scheduler::RequestResultCategory::serial_io_transient);
          }
          continue;
        }

        auto wake_at = now + std::chrono::milliseconds(50);
        if (next_wake.has_value()) {
          const auto scheduled_wake =
              poll_scheduler.has_in_flight_request() ? *next_wake + feedback_grace : *next_wake;
          wake_at = std::min(wake_at, scheduled_wake);
        }
        feedback = scheduler_feedback.wait_pop_until(wake_at);
        if (feedback.status == concurrency::QueuePopStatus::item && feedback.value.has_value()) {
          process_scheduler_feedback(poll_scheduler, *feedback.value);
        }
      }
    } catch (...) {
      request_stop(lifecycle::ShutdownReason::fatal_component_error);
    }
    request_queue.close();
    quality_control.close();
  }

  void process_scheduler_feedback(scheduler::PollScheduler &poll_scheduler,
                                  const SchedulerFeedback &feedback) {
    if (const auto *sent = std::get_if<AttemptSentFeedback>(&feedback)) {
      const auto error =
          poll_scheduler.mark_attempt_sent(sent->request_id, sent->attempt, sent->sent_at);
      if (error.category != scheduler::SchedulerErrorCategory::none) {
        handle_scheduler_error("scheduler_mark_attempt_rejected", error, sent->attempt);
      }
      return;
    }
    const auto &result = std::get<scheduler::AttemptResult>(feedback);
    const auto transition = poll_scheduler.record_attempt_result(result);
    process_scheduler_transition(transition, result.observed_at);
  }

  void process_scheduler_transition(const scheduler::SchedulerTransition &transition,
                                    RuntimeTimePoint observed_at) {
    if (transition.error.category != scheduler::SchedulerErrorCategory::none) {
      handle_scheduler_error("scheduler_result_rejected", transition.error, transition.attempt);
      return;
    }
    const auto *definition = config.registers.find_register(transition.poll_job_id);
    if (definition != nullptr && (transition.terminal || transition.device_state_changed)) {
      static_cast<void>(quality_control.try_push(
          {transition.poll_job_id, definition->slave_id, transition.result, observed_at,
           transition.terminal, transition.device_state_changed, transition.device_state}));
    }
    if (transition.terminal) {
      const std::lock_guard<std::mutex> lock(statistics_mutex);
      auto &slave = statistics_value.slaves[definition == nullptr ? 0U : definition->slave_id];
      if (transition.result == scheduler::RequestResultCategory::success) {
        ++statistics_value.requests_succeeded;
        ++slave.requests_succeeded;
      } else {
        ++statistics_value.requests_failed;
        ++slave.requests_failed;
      }
    }
  }

  void log_scheduler_transition(const std::string &event,
                                const scheduler::SchedulerTransition &transition,
                                observability::LogSeverity severity) {
    observability::StructuredEvent value{};
    value.event = event;
    value.component = "scheduler";
    value.severity = severity;
    value.request_id = transition.request_id;
    value.attempt = transition.attempt;
    value.monotonic_ms =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       RuntimeClock::now().time_since_epoch())
                                       .count());
    value.result = result_name(transition.result);
    value.reason =
        transition.terminal ? "terminal_deadline_transition" : "retry_deadline_transition";
    static_cast<void>(log_writer->write(value));
  }

  void handle_scheduler_error(const std::string &event,
                              const scheduler::SchedulerError &error_value, std::uint32_t attempt) {
    observability::StructuredEvent value{};
    value.event = event;
    value.component = "scheduler";
    value.severity = observability::LogSeverity::critical;
    value.request_id = error_value.request_id;
    value.attempt = attempt;
    value.monotonic_ms =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       RuntimeClock::now().time_since_epoch())
                                       .count());
    value.result = "scheduler_transition_error";
    value.reason = scheduler_error_name(error_value.category);
    value.queue_depth = scheduler_feedback.statistics().current_depth;
    static_cast<void>(log_writer->write(value));
    {
      const std::lock_guard<std::mutex> lock(statistics_mutex);
      ++statistics_value.scheduler_transition_errors;
    }
    request_stop(lifecycle::ShutdownReason::fatal_component_error);
  }

  [[nodiscard]] bool ensure_serial_open(transport::SerialPort &serial) {
    if (serial.is_open()) {
      return true;
    }
    transport::SerialError error_value{};
    if (serial.open(config.serial, error_value)) {
      const std::lock_guard<std::mutex> lock(statistics_mutex);
      ++statistics_value.serial_open_successes;
      return true;
    }
    {
      const std::lock_guard<std::mutex> lock(statistics_mutex);
      ++statistics_value.serial_errors;
    }
    std::this_thread::sleep_for(config.serial_reopen_backoff);
    return false;
  }

  [[nodiscard]] bool write_request(transport::SerialPort &serial, const protocol::Adu &adu,
                                   RuntimeTimePoint deadline) {
    std::size_t offset{};
    while (offset < adu.size && !stop_requested.load()) {
      if (RuntimeClock::now() >= deadline) {
        return false;
      }
      const auto written = serial.write_some(adu.bytes.data() + offset, adu.size - offset);
      if (written.status == transport::IoStatus::completed) {
        if (written.bytes_transferred == 0U) {
          return false;
        }
        offset += written.bytes_transferred;
        continue;
      }
      if (written.status == transport::IoStatus::would_block) {
        const auto ready = serial.wait({false, true}, deadline);
        if (ready.status == transport::WaitStatus::ready && !ready.peer_closed &&
            RuntimeClock::now() < deadline) {
          continue;
        }
      }
      return false;
    }
    return offset == adu.size;
  }

  void discard_late_bytes(transport::SerialPort &serial) {
    std::array<std::uint8_t, protocol::kMaxModbusAduSize> bytes{};
    while (serial.is_open()) {
      const auto read = serial.read_some(bytes.data(), bytes.size());
      if (read.status != transport::IoStatus::completed || read.bytes_transferred == 0U) {
        break;
      }
    }
  }

  [[nodiscard]] LateResponseQuarantineOutcome
  quarantine_late_response(transport::SerialPort &serial,
                           const scheduler::ScheduledRequest &request,
                           scheduler::RequestResultCategory original_result) {
    constexpr auto frame_boundary = std::chrono::microseconds(2'006);
    const auto started_at = RuntimeClock::now();
    const auto listen_deadline = started_at + config.late_response_guard;
    const auto overall_deadline = listen_deadline + kMaximumLateResponseTransfer + frame_boundary;
    auto last_byte_at = started_at;
    bool received_any{};
    LateResponseQuarantineOutcome outcome{};
    std::array<std::uint8_t, protocol::kMaxModbusAduSize> bytes{};

    log("late_response_quarantine_started", "serial", observability::LogSeverity::warning, &request,
        original_result);
    while (serial.is_open() && !stop_requested.load()) {
      const auto deadline = received_any ? std::min(overall_deadline, last_byte_at + frame_boundary)
                                         : listen_deadline;
      const auto ready = serial.wait({true, false}, deadline);
      if (ready.status == transport::WaitStatus::timeout) {
        break;
      }
      if (ready.peer_closed || ready.status != transport::WaitStatus::ready) {
        outcome.requires_serial_reopen = true;
        break;
      }
      const auto read = serial.read_some(bytes.data(), bytes.size());
      if (read.status == transport::IoStatus::would_block) {
        continue;
      }
      if (read.status != transport::IoStatus::completed || read.bytes_transferred == 0U) {
        outcome.requires_serial_reopen = true;
        break;
      }
      outcome.bytes_discarded += read.bytes_transferred;
      received_any = true;
      last_byte_at = RuntimeClock::now();
    }

    {
      const std::lock_guard<std::mutex> lock(statistics_mutex);
      ++statistics_value.late_response_quarantines;
      statistics_value.late_response_bytes_discarded += outcome.bytes_discarded;
    }
    if (outcome.bytes_discarded > 0U) {
      const auto duration_ms = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(RuntimeClock::now() - started_at)
              .count());
      log("late_response_discarded", "serial", observability::LogSeverity::warning, &request,
          original_result, duration_ms);
    } else {
      log("late_response_quarantine_completed", "serial", observability::LogSeverity::debug,
          &request, original_result);
    }
    return outcome;
  }

  [[nodiscard]] AttemptOutcome receive_response(transport::SerialPort &serial,
                                                const scheduler::ScheduledRequest &request,
                                                RuntimeTimePoint deadline) {
    constexpr protocol::ParserTiming timing{860U, 2'006U};
    protocol::RtuStreamParser parser(timing);
    if (!parser.begin_response_stream(request.request_id, request.request)) {
      return {scheduler::RequestResultCategory::invalid_configuration, std::nullopt, std::nullopt};
    }
    std::array<std::uint8_t, protocol::kMaxModbusAduSize> input{};
    std::array<protocol::ParserEvent, 4U> events{};
    bool received_any{};
    while (!stop_requested.load() && RuntimeClock::now() < deadline) {
      const auto ready = serial.wait({true, false}, deadline);
      if (ready.status == transport::WaitStatus::timeout) {
        break;
      }
      if (ready.peer_closed) {
        return {scheduler::RequestResultCategory::serial_io_transient, std::nullopt, std::nullopt,
                true};
      }
      if (ready.status != transport::WaitStatus::ready) {
        return {scheduler::RequestResultCategory::serial_io_transient, std::nullopt, std::nullopt,
                true};
      }
      const auto read = serial.read_some(input.data(), input.size());
      if (read.status == transport::IoStatus::would_block) {
        continue;
      }
      if (read.status != transport::IoStatus::completed || read.bytes_transferred == 0U) {
        return {scheduler::RequestResultCategory::serial_io_transient, std::nullopt, std::nullopt,
                true};
      }
      received_any = true;
      const auto now = RuntimeClock::now();
      const auto batch = parser.ingest(input.data(), read.bytes_transferred,
                                       {monotonic_microseconds(now)}, events.data(), events.size());
      for (std::size_t index = 0U; index < batch.events_written; ++index) {
        if (const auto *response =
                std::get_if<protocol::ParsedResponseEvent>(&events[index].payload)) {
          if (const auto *exception =
                  std::get_if<protocol::ExceptionResponse>(&response->response)) {
            return {scheduler::RequestResultCategory::remote_exception, response->response,
                    exception->exception_code};
          }
          return {scheduler::RequestResultCategory::success, response->response, std::nullopt};
        }
        if (const auto *error = std::get_if<protocol::ParserError>(&events[index].payload)) {
          return {map_parser_error(error->category), std::nullopt, std::nullopt};
        }
      }
    }

    const auto now = RuntimeClock::now() + std::chrono::milliseconds(3);
    const auto batch =
        parser.on_time_advanced({monotonic_microseconds(now)}, events.data(), events.size());
    for (std::size_t index = 0U; index < batch.events_written; ++index) {
      if (const auto *error = std::get_if<protocol::ParserError>(&events[index].payload)) {
        return {map_parser_error(error->category), std::nullopt, std::nullopt};
      }
    }
    return {received_any ? scheduler::RequestResultCategory::truncated_frame
                         : scheduler::RequestResultCategory::response_timeout,
            std::nullopt, std::nullopt};
  }

  [[nodiscard]] AttemptOutcome execute_request(transport::SerialPort &serial,
                                               const scheduler::ScheduledRequest &request) {
    if (!ensure_serial_open(serial)) {
      return {scheduler::RequestResultCategory::serial_io_transient, std::nullopt, std::nullopt};
    }
    discard_late_bytes(serial);
    const auto encoded = protocol::encode_request(request.request);
    if (const auto *error = std::get_if<protocol::CodecError>(&encoded)) {
      const auto result = error->category == protocol::CodecErrorCategory::broadcast_unsupported
                              ? scheduler::RequestResultCategory::broadcast_unsupported
                              : scheduler::RequestResultCategory::invalid_configuration;
      return {result, std::nullopt, std::nullopt};
    }
    const auto &adu = std::get<protocol::Adu>(encoded);
    const auto sent_at = RuntimeClock::now();
    const auto deadline = std::min(request.deadline, sent_at + config.response_timeout);
    if (!write_request(serial, adu, deadline)) {
      if (stop_requested.load()) {
        return {scheduler::RequestResultCategory::shutdown_cancelled, std::nullopt, std::nullopt};
      }
      bool closed_serial{};
      if (serial.is_open()) {
        serial.close();
        closed_serial = true;
      }
      {
        const std::lock_guard<std::mutex> lock(statistics_mutex);
        if (closed_serial) {
          ++statistics_value.serial_close_count;
        }
        ++statistics_value.serial_errors;
      }
      return {scheduler::RequestResultCategory::serial_io_transient, std::nullopt, std::nullopt};
    }
    {
      const std::lock_guard<std::mutex> lock(statistics_mutex);
      ++statistics_value.requests_sent;
      ++statistics_value.slaves[request_slave_id(request.request)].attempts_sent;
      statistics_value.in_flight_requests = 1U;
      statistics_value.maximum_in_flight_requests = std::max(
          statistics_value.maximum_in_flight_requests, statistics_value.in_flight_requests);
    }
    auto outcome = receive_response(serial, request, deadline);
    if (stop_requested.load()) {
      outcome = {scheduler::RequestResultCategory::shutdown_cancelled, std::nullopt, std::nullopt};
    }
    if (!outcome.requires_serial_reopen &&
        (outcome.category == scheduler::RequestResultCategory::response_timeout ||
         outcome.category == scheduler::RequestResultCategory::truncated_frame)) {
      const auto quarantine = quarantine_late_response(serial, request, outcome.category);
      outcome.requires_serial_reopen = quarantine.requires_serial_reopen;
    }
    if (outcome.requires_serial_reopen && serial.is_open()) {
      serial.close();
      const std::lock_guard<std::mutex> lock(statistics_mutex);
      ++statistics_value.serial_close_count;
    }
    {
      const std::lock_guard<std::mutex> lock(statistics_mutex);
      statistics_value.in_flight_requests = 0U;
      switch (outcome.category) {
      case scheduler::RequestResultCategory::response_timeout:
        ++statistics_value.response_timeouts;
        break;
      case scheduler::RequestResultCategory::crc_mismatch:
        ++statistics_value.crc_errors;
        break;
      case scheduler::RequestResultCategory::truncated_frame:
        ++statistics_value.truncated_frames;
        break;
      case scheduler::RequestResultCategory::remote_exception:
        ++statistics_value.remote_exceptions;
        break;
      case scheduler::RequestResultCategory::serial_io_transient:
        ++statistics_value.serial_errors;
        break;
      default:
        break;
      }
    }
    return outcome;
  }

  [[nodiscard]] bool submit_scheduler_feedback(SchedulerFeedback feedback,
                                               const scheduler::ScheduledRequest &request) {
    const auto status = scheduler_feedback.push_until(
        feedback, RuntimeClock::now() + kSchedulerFeedbackDeliveryTimeout);
    if (status == concurrency::QueuePushStatus::accepted) {
      return true;
    }
    if (status == concurrency::QueuePushStatus::closed && stop_requested.load()) {
      return false;
    }

    observability::StructuredEvent event{};
    event.event = "scheduler_feedback_delivery_failed";
    event.component = "serial";
    event.severity = observability::LogSeverity::critical;
    event.request_id = request.request_id;
    event.attempt = request.attempt;
    event.slave_id = request_slave_id(request.request);
    event.function = request_function(request.request);
    event.address = request_address(request.request);
    event.monotonic_ms =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       RuntimeClock::now().time_since_epoch())
                                       .count());
    event.result = "scheduler_feedback_error";
    event.reason = queue_push_status_name(status);
    event.queue_depth = scheduler_feedback.statistics().current_depth;
    static_cast<void>(log_writer->write(event));
    {
      const std::lock_guard<std::mutex> lock(statistics_mutex);
      ++statistics_value.scheduler_feedback_delivery_failures;
    }
    request_stop(lifecycle::ShutdownReason::fatal_component_error);
    return false;
  }

  void serial_loop() noexcept {
    transport::SerialPort serial;
    try {
      std::optional<RuntimeTimePoint> previous_request_started_at{};
      while (true) {
        auto popped =
            request_queue.wait_pop_until(RuntimeClock::now() + std::chrono::milliseconds(50));
        if (popped.status == concurrency::QueuePopStatus::closed) {
          break;
        }
        if (popped.status != concurrency::QueuePopStatus::item || !popped.value.has_value()) {
          if (stop_requested.load() && request_queue.closed()) {
            break;
          }
          continue;
        }
        auto request = *popped.value;
        if (stop_requested.load()) {
          continue;
        }
        if (previous_request_started_at.has_value() &&
            config.minimum_request_interval.count() > 0) {
          const auto earliest_start =
              *previous_request_started_at + config.minimum_request_interval;
          while (!stop_requested.load()) {
            const auto now = RuntimeClock::now();
            if (now >= earliest_start) {
              break;
            }
            const auto remaining = earliest_start - now;
            std::this_thread::sleep_for(std::min(
                remaining,
                std::chrono::duration_cast<RuntimeClock::duration>(std::chrono::milliseconds(10))));
          }
        }
        if (stop_requested.load()) {
          continue;
        }
        const auto attempt_started = RuntimeClock::now();
        previous_request_started_at = attempt_started;
        if (request.request_kind == scheduler::RequestKind::poll_read) {
          if (!submit_scheduler_feedback(AttemptSentFeedback{request.request_id,
                                                             request.poll_job_id, request.attempt,
                                                             attempt_started},
                                         request)) {
            continue;
          }
        }
        const auto outcome = execute_request(serial, request);
        const auto observed_at = RuntimeClock::now();
        const auto duration_ms = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(observed_at - attempt_started)
                .count());
        log("request_completed", "serial", observability::LogSeverity::info, &request,
            outcome.category, duration_ms);

        if (outcome.category == scheduler::RequestResultCategory::success &&
            outcome.response.has_value()) {
          if (const auto *read = std::get_if<protocol::ReadResponse>(&*outcome.response)) {
            pipeline::MeasurementRaw measurement{};
            measurement.request_id = request.request_id;
            measurement.slave_id = read->slave_id;
            measurement.function = read->function;
            measurement.start_address = request_address(request.request);
            measurement.received_at = observed_at;
            measurement.received_at_system = std::chrono::system_clock::now();
            measurement.registers.assign(read->values.begin(),
                                         read->values.begin() + read->value_count);
            const auto measurement_status = measurement_queue.push(std::move(measurement));
            if (measurement_status != concurrency::QueuePushStatus::accepted &&
                measurement_status != concurrency::QueuePushStatus::closed) {
              const auto queue_statistics = measurement_queue.statistics();
              observability::StructuredEvent event{};
              event.event = "measurement_queue_rejected";
              event.component = "serial";
              event.severity = observability::LogSeverity::error;
              event.request_id = request.request_id;
              event.slave_id = read->slave_id;
              event.function = static_cast<std::uint8_t>(read->function);
              event.address = request_address(request.request);
              event.result = "queue_full";
              event.reason = "measurement_consumer_not_keeping_up";
              event.queue_depth = queue_statistics.current_depth;
              static_cast<void>(log_writer->write(event));
              const std::lock_guard<std::mutex> lock(statistics_mutex);
              ++statistics_value.measurement_enqueue_failures;
            }
          } else if (const auto *write =
                         std::get_if<protocol::WriteSingleRegisterResponse>(&*outcome.response)) {
            pipeline::WriteAuditEvent audit{};
            audit.event_id = next_event_id.fetch_add(1U);
            audit.request_id = request.request_id;
            audit.slave_id = write->slave_id;
            audit.address = write->register_address;
            audit.result = outcome.category;
            static_cast<void>(publish_message(audit));
            const std::lock_guard<std::mutex> lock(statistics_mutex);
            ++statistics_value.explicit_write_successes;
          }
        }

        if (request.request_kind == scheduler::RequestKind::poll_read) {
          scheduler::AttemptResult result{};
          result.request_id = request.request_id;
          result.poll_job_id = request.poll_job_id;
          result.attempt = request.attempt;
          result.category = outcome.category;
          result.observed_at = observed_at;
          result.remote_exception_code = outcome.exception_code;
          static_cast<void>(submit_scheduler_feedback(result, request));
        } else {
          const std::lock_guard<std::mutex> lock(statistics_mutex);
          auto &slave = statistics_value.slaves[request_slave_id(request.request)];
          if (outcome.category == scheduler::RequestResultCategory::success) {
            ++statistics_value.requests_succeeded;
            ++slave.requests_succeeded;
          } else {
            ++statistics_value.requests_failed;
            ++slave.requests_failed;
          }
        }
      }
    } catch (...) {
      request_stop(lifecycle::ShutdownReason::fatal_component_error);
    }
    if (serial.is_open()) {
      serial.close();
      const std::lock_guard<std::mutex> lock(statistics_mutex);
      ++statistics_value.serial_close_count;
    }
    measurement_queue.close();
  }

  [[nodiscard]] const config::RuntimeRegisterDefinition *
  find_register(std::uint32_t poll_job_id) const noexcept {
    for (const auto &candidate : config.registers.registers) {
      if (candidate.poll_job_id == poll_job_id) {
        return &candidate;
      }
    }
    return nullptr;
  }

  void publish_quality_snapshot(const quality::QualityTransition &transition,
                                const std::string &reason) {
    if (transition.current != quality::RegisterQuality::stale &&
        transition.current != quality::RegisterQuality::offline) {
      return;
    }
    const auto *definition = find_register(transition.poll_job_id);
    if (definition == nullptr) {
      return;
    }

    pipeline::TelemetryEvent telemetry{};
    const auto latest = latest_valid_telemetry.find(transition.poll_job_id);
    if (latest != latest_valid_telemetry.end()) {
      telemetry = latest->second;
      telemetry.value_is_retained = true;
    } else {
      telemetry.topic = definition->topic;
      telemetry.device_name = definition->device_name;
      telemetry.slave_id = definition->slave_id;
      telemetry.register_name = definition->register_name;
      telemetry.unit = definition->unit;
      telemetry.source_timestamp = std::chrono::system_clock::now();
    }
    telemetry.event_id = next_event_id.fetch_add(1U);
    telemetry.quality = transition.current;
    telemetry.quality_reason = reason;
    telemetry.run_id.clear();
    telemetry.sequence = 0U;
    telemetry.gateway_timestamp = std::chrono::system_clock::now();
    static_cast<void>(publish_message(std::move(telemetry)));
  }

  void publish_quality_transitions(const quality::QualityUpdateBatch &batch,
                                   const std::string &reason) {
    for (const auto &transition : batch.transitions) {
      static_cast<void>(publish_message(pipeline::QualityTransitionEvent{transition}));
      publish_quality_snapshot(transition, reason);
    }
  }

  void handle_measurement(quality::FreshnessTracker &freshness,
                          const pipeline::MeasurementRaw &measurement) {
    const config::RuntimeRegisterDefinition *definition = nullptr;
    for (const auto &candidate : config.registers.registers) {
      if (candidate.slave_id == measurement.slave_id &&
          candidate.function == measurement.function &&
          candidate.address == measurement.start_address) {
        definition = &candidate;
        break;
      }
    }
    if (definition == nullptr) {
      return;
    }
    const auto decoded = config::decode_engineering_value(*definition, measurement.registers);
    const auto transitions =
        decoded ? freshness.record_valid_sample(definition->poll_job_id, measurement.received_at)
                : freshness.record_invalid_sample(definition->poll_job_id, measurement.received_at);
    publish_quality_transitions(transitions, decoded ? "valid_sample" : "decode_error");
    pipeline::TelemetryEvent telemetry{};
    telemetry.event_id = next_event_id.fetch_add(1U);
    telemetry.request_id = measurement.request_id;
    telemetry.topic = definition->topic;
    telemetry.quality = freshness.quality(definition->poll_job_id);
    telemetry.quality_reason = decoded ? "valid_sample"
                               : decoded.error == config::DecodeErrorCategory::invalid_raw_value
                                   ? "invalid_raw_value"
                                   : "decode_error";
    telemetry.value = decoded.value;
    telemetry.device_name = definition->device_name;
    telemetry.slave_id = definition->slave_id;
    telemetry.register_name = definition->register_name;
    telemetry.raw_value = decoded.raw_value;
    telemetry.unit = definition->unit;
    telemetry.source_timestamp = measurement.received_at_system;
    telemetry.gateway_timestamp = std::chrono::system_clock::now();
    telemetry.freshness_deadline = measurement.received_at + definition->freshness;
    if (decoded) {
      latest_valid_telemetry[definition->poll_job_id] = telemetry;
    }
    static_cast<void>(publish_message(std::move(telemetry)));
  }

  void handle_quality_control(quality::FreshnessTracker &freshness,
                              const QualityControlEvent &control) {
    if (control.terminal && control.result != scheduler::RequestResultCategory::success) {
      publish_quality_transitions(
          freshness.record_poll_failure(control.poll_job_id, control.observed_at), "poll_failure");
    }
    if (control.device_state_changed) {
      const bool offline = control.device_state != scheduler::DeviceHealthState::online;
      publish_quality_transitions(
          freshness.set_device_offline(control.slave_id, offline, control.observed_at),
          offline ? "device_offline" : "device_recovered");
      const config::RuntimeRegisterDefinition *device = nullptr;
      for (const auto &candidate : config.registers.registers) {
        if (candidate.slave_id == control.slave_id) {
          device = &candidate;
          break;
        }
      }
      if (device != nullptr) {
        pipeline::DeviceStatusEvent status{};
        status.event_id = next_event_id.fetch_add(1U);
        status.device_name = device->device_name;
        status.slave_id = control.slave_id;
        if (control.device_state == scheduler::DeviceHealthState::online) {
          status.state = "online";
          status.reason = "device_recovered";
        } else if (control.device_state == scheduler::DeviceHealthState::probing) {
          status.state = "probing";
          status.reason = "recovery_probe";
        } else {
          status.state = "offline";
          status.reason = "consecutive_final_failures";
        }
        status.source_timestamp = std::chrono::system_clock::now();
        status.gateway_timestamp = status.source_timestamp;
        static_cast<void>(publish_message(std::move(status)));
      }
    }
  }

  void quality_loop() noexcept {
    quality::FreshnessTracker freshness(config.registers.freshness);
    bool measurement_closed{};
    bool control_closed{};
    try {
      while (!measurement_closed || !control_closed) {
        auto measurement =
            measurement_queue.wait_pop_until(RuntimeClock::now() + std::chrono::milliseconds(25));
        if (measurement.status == concurrency::QueuePopStatus::closed) {
          measurement_closed = true;
        } else if (measurement.status == concurrency::QueuePopStatus::item &&
                   measurement.value.has_value()) {
          if (const auto *raw = std::get_if<pipeline::MeasurementRaw>(&*measurement.value)) {
            handle_measurement(freshness, *raw);
          }
        }

        auto control = quality_control.try_pop();
        if (control.status == concurrency::QueuePopStatus::closed) {
          control_closed = true;
        } else if (control.status == concurrency::QueuePopStatus::item &&
                   control.value.has_value()) {
          handle_quality_control(freshness, *control.value);
        }
        publish_quality_transitions(freshness.advance_time(RuntimeClock::now()),
                                    "freshness_expired");
      }
    } catch (...) {
      request_stop(lifecycle::ShutdownReason::fatal_component_error);
    }
  }

  GatewayRuntimeConfig config;
  RuntimeErrorCategory error{RuntimeErrorCategory::none};
  std::shared_ptr<observability::JsonlLogWriter> log_writer;
  std::unique_ptr<publish::PublishSink> publish_sink;
  pipeline::RequestQueue request_queue{};
  pipeline::MeasurementQueue measurement_queue{};
  concurrency::BoundedQueue<SchedulerFeedback> scheduler_feedback;
  concurrency::BoundedQueue<QualityControlEvent> quality_control;
  lifecycle::ShutdownCoordinator shutdown{};
  std::atomic<bool> started{};
  std::atomic<bool> stop_requested{};
  std::atomic<std::uint64_t> next_explicit_request_id{1U};
  std::atomic<std::uint64_t> next_event_id{1U};
  std::unordered_map<std::uint32_t, pipeline::TelemetryEvent> latest_valid_telemetry{};
  std::thread scheduler_thread{};
  std::thread serial_thread{};
  std::thread quality_thread{};
  mutable std::mutex statistics_mutex{};
  GatewayRuntimeStatistics statistics_value{};
};

GatewayRuntime::GatewayRuntime(GatewayRuntimeConfig config, std::ostream &event_output)
    : impl_(std::make_unique<Impl>(std::move(config),
                                   std::make_shared<observability::JsonlLogWriter>(event_output),
                                   nullptr)) {}

GatewayRuntime::GatewayRuntime(GatewayRuntimeConfig config,
                               std::shared_ptr<observability::JsonlLogWriter> logger,
                               std::unique_ptr<publish::PublishSink> supplied_sink)
    : impl_(
          std::make_unique<Impl>(std::move(config), std::move(logger), std::move(supplied_sink))) {}

GatewayRuntime::~GatewayRuntime() = default;

bool GatewayRuntime::valid() const noexcept {
  return impl_->validation_error() == RuntimeErrorCategory::none;
}

RuntimeErrorCategory GatewayRuntime::validation_error() const noexcept {
  return impl_->validation_error();
}

bool GatewayRuntime::start() { return impl_->start(); }

concurrency::QueuePushStatus
GatewayRuntime::submit_write(std::uint8_t slave_id, std::uint16_t address, std::uint16_t value) {
  return impl_->submit_write(slave_id, address, value);
}

void GatewayRuntime::request_stop(lifecycle::ShutdownReason reason) noexcept {
  impl_->request_stop(reason);
}

void GatewayRuntime::join() noexcept { impl_->join(); }

GatewayRuntimeStatistics GatewayRuntime::statistics() const { return impl_->statistics(); }

} // namespace industrial_iot_gateway::runtime
