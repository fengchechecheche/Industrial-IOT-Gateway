#include "industrial_iot_gateway/config/runtime_config.hpp"
#include "industrial_iot_gateway/lifecycle/shutdown_coordinator.hpp"
#include "industrial_iot_gateway/mqtt/mqtt_config.hpp"
#include "industrial_iot_gateway/mqtt/mqtt_publish_sink.hpp"
#include "industrial_iot_gateway/runtime/gateway_runtime.hpp"
#include "industrial_iot_gateway/simulation/pty_bus_harness.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <exception>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

using Clock = std::chrono::steady_clock;
using industrial_iot_gateway::pty_slave::FaultMode;
using industrial_iot_gateway::pty_slave::FaultPlan;
using industrial_iot_gateway::runtime::GatewayRuntimeStatistics;
using Json = nlohmann::json;

std::atomic<bool> stop_requested{};

void on_signal(int) noexcept { stop_requested.store(true); }

struct Arguments {
  std::string profile_path{};
  std::string run_id{};
  std::string gateway_events_path{};
};

[[nodiscard]] std::optional<Arguments> parse_arguments(int argc, char **argv) {
  Arguments arguments{};
  for (int index = 1; index + 1 < argc; index += 2) {
    const std::string option = argv[index];
    if (option == "--profile") {
      arguments.profile_path = argv[index + 1];
    } else if (option == "--run-id") {
      arguments.run_id = argv[index + 1];
    } else if (option == "--gateway-events") {
      arguments.gateway_events_path = argv[index + 1];
    } else {
      return std::nullopt;
    }
  }
  if (arguments.profile_path.empty() || arguments.run_id.empty() ||
      arguments.gateway_events_path.empty()) {
    return std::nullopt;
  }
  return arguments;
}

void emit(Json event) {
  event["monotonic_ms"] =
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch())
          .count();
  std::cout << event.dump() << '\n';
  std::cout.flush();
}

[[nodiscard]] Json
queue_json(const industrial_iot_gateway::concurrency::QueueStatistics &statistics) {
  return {{"capacity", statistics.capacity},
          {"current_depth", statistics.current_depth},
          {"high_watermark", statistics.high_watermark},
          {"maximum_depth", statistics.maximum_depth},
          {"accepted", statistics.accepted},
          {"popped", statistics.popped},
          {"full", statistics.full},
          {"coalesced", statistics.coalesced},
          {"rejected_closed", statistics.rejected_closed},
          {"timed_out", statistics.timed_out}};
}

[[nodiscard]] Json statistics_json(const GatewayRuntimeStatistics &statistics) {
  Json slaves = Json::array();
  for (std::uint8_t slave_id = 1U; slave_id <= 3U; ++slave_id) {
    const auto &slave = statistics.slaves[slave_id];
    slaves.push_back({{"slave_id", slave_id},
                      {"attempts_sent", slave.attempts_sent},
                      {"requests_succeeded", slave.requests_succeeded},
                      {"requests_failed", slave.requests_failed}});
  }
  const auto &publisher = statistics.publisher;
  return {{"requests_sent", statistics.requests_sent},
          {"requests_succeeded", statistics.requests_succeeded},
          {"requests_failed", statistics.requests_failed},
          {"response_timeouts", statistics.response_timeouts},
          {"late_response_quarantines", statistics.late_response_quarantines},
          {"late_response_bytes_discarded", statistics.late_response_bytes_discarded},
          {"crc_errors", statistics.crc_errors},
          {"truncated_frames", statistics.truncated_frames},
          {"remote_exceptions", statistics.remote_exceptions},
          {"serial_errors", statistics.serial_errors},
          {"serial_open_successes", statistics.serial_open_successes},
          {"measurement_enqueue_failures", statistics.measurement_enqueue_failures},
          {"running", statistics.running},
          {"stopped", statistics.stopped},
          {"request_queue", queue_json(statistics.request_queue)},
          {"measurement_queue", queue_json(statistics.measurement_queue)},
          {"publish_queue", queue_json(statistics.publish_queue)},
          {"publisher",
           {{"connected", publisher.connected},
            {"connect_attempts", publisher.connect_attempts},
            {"connected_events", publisher.connected_events},
            {"disconnected_events", publisher.disconnected_events},
            {"publish_attempts", publisher.publish_attempts},
            {"publish_successes", publisher.publish_successes},
            {"publish_failures", publisher.publish_failures},
            {"coalesced", publisher.coalesced},
            {"dropped", publisher.dropped},
            {"expired_fresh_dropped", publisher.expired_fresh_dropped},
            {"critical_enqueue_failures", publisher.critical_enqueue_failures},
            {"drain_expired", publisher.drain_expired},
            {"unconfirmed_on_close", publisher.unconfirmed_on_close}}},
          {"slaves", std::move(slaves)}};
}

[[nodiscard]] FaultPlan make_fault(const Json &fault) {
  FaultPlan plan{};
  const auto kind = fault.at("kind").get<std::string>();
  if (kind == "silent") {
    plan.mode = FaultMode::silent;
  } else if (kind == "delayed_response") {
    plan.mode = FaultMode::delayed_response;
    plan.delay_ms = fault.value("delay_ms", 650U);
  } else if (kind == "bad_crc") {
    plan.mode = FaultMode::bad_crc;
  } else if (kind == "truncated_response") {
    plan.mode = FaultMode::truncated_response;
    plan.truncate_bytes = fault.value("truncate_bytes", 2U);
  } else if (kind == "exception_response") {
    plan.mode = FaultMode::exception_response;
    plan.exception_code = fault.value("exception_code", 2U);
  }
  return plan;
}

struct FaultState {
  Json definition{};
  std::size_t last_cycle{static_cast<std::size_t>(-1)};
  bool active{};
  bool awaiting_recovery{};
  Clock::time_point clear_at{};
  Clock::time_point recovery_deadline{};
  std::uint64_t success_at_clear{};
  std::array<std::uint64_t, 4U> slave_successes_at_clear{};
  std::array<bool, 4U> slave_recovered{};
  std::array<std::uint64_t, 4U> slave_recovery_ms{};
};

[[nodiscard]] std::array<std::uint64_t, 4U>
slave_success_snapshot(const GatewayRuntimeStatistics &statistics) {
  std::array<std::uint64_t, 4U> values{};
  for (std::uint8_t slave_id = 1U; slave_id <= 3U; ++slave_id) {
    values[slave_id] = statistics.slaves[slave_id].requests_succeeded;
  }
  return values;
}

[[nodiscard]] std::uint64_t slave_successes(const GatewayRuntimeStatistics &statistics,
                                            const Json &fault) {
  return statistics.slaves[fault.value("target_slave", 3U)].requests_succeeded;
}

[[nodiscard]] bool all_slaves_recovered(const FaultState &state) {
  return state.slave_recovered[1] && state.slave_recovered[2] && state.slave_recovered[3];
}

} // namespace

int run(int argc, char **argv) {
  const auto arguments = parse_arguments(argc, argv);
  if (!arguments.has_value()) {
    std::cerr << "usage: gateway_soak_driver --profile FILE --run-id ID --gateway-events FILE\n";
    return 2;
  }
  std::signal(SIGTERM, on_signal);
  std::signal(SIGINT, on_signal);

  Json profile{};
  try {
    std::ifstream input(arguments->profile_path);
    input >> profile;
  } catch (const std::exception &error) {
    std::cerr << "cannot read resolved profile: " << error.what() << '\n';
    return 2;
  }
  std::unique_ptr<std::ofstream> gateway_event_file{};
  std::ostream *gateway_events = &std::cerr;
  if (arguments->gateway_events_path != "-") {
    gateway_event_file =
        std::make_unique<std::ofstream>(arguments->gateway_events_path, std::ios::app);
    gateway_events = gateway_event_file.get();
  }
  if (!gateway_events->good()) {
    std::cerr << "cannot open gateway event output\n";
    return 2;
  }

  const auto &load = profile.at("load");
  industrial_iot_gateway::simulation::PtyBusHarness bus(load.at("register_map").get<std::string>(),
                                                        load.at("scenario_map").get<std::string>());
  if (!bus.start()) {
    std::cerr << bus.last_error() << '\n';
    return 3;
  }
  auto loaded = industrial_iot_gateway::config::load_runtime_configuration(
      load.at("register_map").get<std::string>());
  if (!loaded) {
    std::cerr << loaded.detail << '\n';
    return 3;
  }
  industrial_iot_gateway::runtime::GatewayRuntimeConfig runtime_config{};
  runtime_config.serial.device_path = bus.gateway_path();
  runtime_config.registers = std::move(*loaded.configuration);
  runtime_config.response_timeout = std::chrono::milliseconds(500);
  runtime_config.late_response_guard = std::chrono::milliseconds(200);
  runtime_config.serial_reopen_backoff = std::chrono::milliseconds(100);

  auto logger =
      std::make_shared<industrial_iot_gateway::observability::JsonlLogWriter>(*gateway_events);
  industrial_iot_gateway::mqtt::MqttConfig mqtt_config{};
  const auto &mqtt = load.at("mqtt");
  mqtt_config.broker_uri = "tcp://" + mqtt.at("broker_host").get<std::string>() + ":" +
                           std::to_string(mqtt.at("broker_port").get<unsigned int>());
  const auto client_digits = std::to_string(std::hash<std::string>{}(arguments->run_id));
  mqtt_config.client_id = "iiotgw" + client_digits.substr(0U, 17U);
  mqtt_config.gateway_id = "lab_gateway_01";
  mqtt_config.run_id = arguments->run_id;
  mqtt_config.keep_alive = std::chrono::seconds(2);
  mqtt_config.connect_timeout = std::chrono::seconds(1);
  mqtt_config.publish_ack_timeout = std::chrono::seconds(2);
  auto sink = std::make_unique<industrial_iot_gateway::mqtt::MqttPublishSink>(mqtt_config, logger);
  industrial_iot_gateway::runtime::GatewayRuntime runtime(std::move(runtime_config), logger,
                                                          std::move(sink));
  if (!runtime.valid() || !runtime.start()) {
    std::cerr << "gateway runtime failed to start\n";
    bus.stop();
    return 3;
  }

  std::vector<FaultState> faults{};
  for (const auto &fault : profile.at("faults")) {
    if (fault.at("actor") == "driver") {
      faults.push_back(FaultState{fault});
    }
  }
  const auto started = Clock::now();
  auto next_heartbeat = started;
  const auto duration = std::chrono::seconds(profile.at("duration_seconds").get<int>());
  const auto cycle_seconds = profile.at("fault_cycle_seconds").get<std::size_t>();
  bool internal_failure{};
  emit({{"event", "soak_driver_started"}, {"run_id", arguments->run_id}});

  while (!stop_requested.load() && Clock::now() - started < duration) {
    const auto now = Clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - started);
    const auto elapsed_ms = static_cast<std::size_t>(elapsed.count());
    const auto cycle = elapsed_ms / (cycle_seconds * 1000U);
    const auto cycle_offset_ms = elapsed_ms % (cycle_seconds * 1000U);
    const auto statistics = runtime.statistics();

    for (auto &state : faults) {
      const auto offset_ms = state.definition.at("offset_seconds").get<std::size_t>() * 1000U;
      if (!state.active && !state.awaiting_recovery && state.last_cycle != cycle &&
          cycle_offset_ms >= offset_ms) {
        state.last_cycle = cycle;
        state.active = true;
        emit({{"event", "fault_started"},
              {"fault_id", state.definition.at("fault_id")},
              {"cycle", cycle}});
        const auto fault_duration = std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(state.definition.at("duration_seconds").get<double>()));
        state.clear_at = now + fault_duration;
        if (state.definition.at("kind") == "pty_disconnect") {
          if (!bus.disconnect_and_reconnect(
                  std::chrono::duration_cast<std::chrono::milliseconds>(fault_duration))) {
            emit({{"event", "driver_error"}, {"reason", bus.last_error()}});
            internal_failure = true;
          }
          state.clear_at = Clock::now();
        } else if (!bus.set_fault(state.definition.value("target_slave", 3U),
                                  make_fault(state.definition))) {
          emit({{"event", "driver_error"}, {"reason", "set_fault failed"}});
          internal_failure = true;
        }
      }
      if (state.active && Clock::now() >= state.clear_at) {
        if (state.definition.at("kind") != "pty_disconnect") {
          static_cast<void>(bus.set_fault(state.definition.value("target_slave", 3U), FaultPlan{}));
        }
        state.active = false;
        state.awaiting_recovery = true;
        const auto statistics_at_clear = runtime.statistics();
        state.success_at_clear = slave_successes(statistics_at_clear, state.definition);
        state.slave_successes_at_clear = slave_success_snapshot(statistics_at_clear);
        state.slave_recovered.fill(false);
        state.slave_recovery_ms.fill(0U);
        state.recovery_deadline =
            Clock::now() +
            std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(
                state.definition.at("recovery_timeout_seconds").get<double>()));
        emit({{"event", "fault_cleared"}, {"fault_id", state.definition.at("fault_id")}});
      }
      if (state.awaiting_recovery) {
        bool recovered{};
        Json recovery_details = Json::object();
        if (state.definition.at("kind") == "pty_disconnect") {
          const auto current = runtime.statistics();
          Json slaves = Json::array();
          for (std::uint8_t slave_id = 1U; slave_id <= 3U; ++slave_id) {
            if (!state.slave_recovered[slave_id] && current.slaves[slave_id].requests_succeeded >
                                                        state.slave_successes_at_clear[slave_id]) {
              state.slave_recovered[slave_id] = true;
              state.slave_recovery_ms[slave_id] =
                  static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                 Clock::now() - state.clear_at)
                                                 .count());
            }
            slaves.push_back({{"slave_id", slave_id},
                              {"recovered", state.slave_recovered[slave_id]},
                              {"recovery_ms", state.slave_recovery_ms[slave_id]}});
          }
          recovered = all_slaves_recovered(state);
          recovery_details["slaves"] = std::move(slaves);
          recovery_details["all_slaves_recovered"] = recovered;
          recovery_details["recovery_ms"] = std::max(
              {state.slave_recovery_ms[1], state.slave_recovery_ms[2], state.slave_recovery_ms[3]});
        } else {
          recovered =
              slave_successes(runtime.statistics(), state.definition) > state.success_at_clear;
        }
        if (recovered) {
          state.awaiting_recovery = false;
          Json event{{"event", "fault_recovered"}, {"fault_id", state.definition.at("fault_id")}};
          event.update(recovery_details);
          emit(std::move(event));
        } else if (Clock::now() > state.recovery_deadline) {
          state.awaiting_recovery = false;
          internal_failure = true;
          emit(
              {{"event", "fault_recovery_timeout"}, {"fault_id", state.definition.at("fault_id")}});
        }
      }
    }

    if (now >= next_heartbeat) {
      emit({{"event", "soak_heartbeat"},
            {"elapsed_ms", elapsed.count()},
            {"statistics", statistics_json(statistics)}});
      next_heartbeat =
          now + std::chrono::seconds(profile.at("heartbeat_interval_seconds").get<int>());
    }
    if (internal_failure) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  const auto shutdown_started = Clock::now();
  runtime.request_stop(industrial_iot_gateway::lifecycle::ShutdownReason::service_stop);
  runtime.join();
  bus.stop();
  const auto final_statistics = runtime.statistics();
  emit({{"event", "soak_driver_stopped"},
        {"shutdown_duration_ms",
         std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - shutdown_started)
             .count()},
        {"statistics", statistics_json(final_statistics)},
        {"internal_failure", internal_failure}});
  return internal_failure ? 4 : 0;
}

int main(int argc, char **argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception &error) {
    std::cerr << "unhandled soak driver error: " << error.what() << '\n';
    return 5;
  } catch (...) {
    std::cerr << "unhandled non-standard soak driver error\n";
    return 5;
  }
}
