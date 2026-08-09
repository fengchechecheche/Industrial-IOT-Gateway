#include "industrial_iot_gateway/build_info.hpp"
#include "industrial_iot_gateway/config/runtime_config.hpp"
#include "industrial_iot_gateway/lifecycle/shutdown_coordinator.hpp"
#include "industrial_iot_gateway/runtime/gateway_runtime.hpp"

#ifdef GATEWAY_HAS_MQTT
#include "industrial_iot_gateway/mqtt/mqtt_config.hpp"
#include "industrial_iot_gateway/mqtt/mqtt_publish_sink.hpp"
#endif

#include <array>
#include <charconv>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

struct Options {
  std::string serial_device{};
  std::string register_map{};
  std::optional<std::uint8_t> write_slave{};
  std::optional<std::uint16_t> write_address{};
  std::optional<std::uint16_t> write_value{};
#ifdef GATEWAY_HAS_MQTT
  std::string mqtt_broker_uri{};
  std::string mqtt_client_id{};
  std::string gateway_id{};
#endif
  bool show_version{};
  bool show_help{};
};

template <typename T> [[nodiscard]] bool parse_unsigned(std::string_view text, T &value) {
  std::uint32_t parsed{};
  const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
      parsed > static_cast<std::uint32_t>(std::numeric_limits<T>::max())) {
    return false;
  }
  value = static_cast<T>(parsed);
  return true;
}

[[nodiscard]] std::optional<Options> parse_options(const int argc, char **argv) {
  Options options{};
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument = argv[index];
    if (argument == "--version") {
      options.show_version = true;
    } else if (argument == "--help") {
      options.show_help = true;
    } else if ((argument == "--serial-device" || argument == "--register-map"
#ifdef GATEWAY_HAS_MQTT
                || argument == "--mqtt-broker-uri" || argument == "--mqtt-client-id" ||
                argument == "--gateway-id"
#endif
                ) &&
               index + 1 < argc) {
      const std::string value = argv[++index];
      if (argument == "--serial-device") {
        options.serial_device = value;
      } else if (argument == "--register-map") {
        options.register_map = value;
#ifdef GATEWAY_HAS_MQTT
      } else if (argument == "--mqtt-broker-uri") {
        options.mqtt_broker_uri = value;
      } else if (argument == "--mqtt-client-id") {
        options.mqtt_client_id = value;
      } else {
        options.gateway_id = value;
#endif
      }
    } else if ((argument == "--write-slave" || argument == "--write-address" ||
                argument == "--write-value") &&
               index + 1 < argc) {
      const std::string_view value = argv[++index];
      if (argument == "--write-slave") {
        std::uint8_t parsed{};
        if (!parse_unsigned(value, parsed)) {
          return std::nullopt;
        }
        options.write_slave = parsed;
      } else if (argument == "--write-address") {
        std::uint16_t parsed{};
        if (!parse_unsigned(value, parsed)) {
          return std::nullopt;
        }
        options.write_address = parsed;
      } else {
        std::uint16_t parsed{};
        if (!parse_unsigned(value, parsed)) {
          return std::nullopt;
        }
        options.write_value = parsed;
      }
    } else {
      return std::nullopt;
    }
  }
  return options;
}

void print_usage(std::ostream &output) {
  output << "usage: gateway_app --serial-device PATH --register-map PATH "
            "[--write-slave ID --write-address ADDRESS --write-value VALUE]"
#ifdef GATEWAY_HAS_MQTT
            " [--mqtt-broker-uri URI --mqtt-client-id ID --gateway-id ID]"
#endif
            "\n";
}

[[nodiscard]] bool complete_write_options(const Options &options) noexcept {
  const auto count = static_cast<unsigned int>(options.write_slave.has_value()) +
                     static_cast<unsigned int>(options.write_address.has_value()) +
                     static_cast<unsigned int>(options.write_value.has_value());
  return count == 0U || count == 3U;
}

#ifdef GATEWAY_HAS_MQTT
[[nodiscard]] bool complete_mqtt_options(const Options &options) noexcept {
  const auto count = static_cast<unsigned int>(!options.mqtt_broker_uri.empty()) +
                     static_cast<unsigned int>(!options.mqtt_client_id.empty()) +
                     static_cast<unsigned int>(!options.gateway_id.empty());
  return count == 0U || count == 3U;
}

[[nodiscard]] std::string generate_run_id() {
  std::array<std::uint8_t, 16U> bytes{};
  std::random_device random{};
  for (auto &byte : bytes) {
    byte = static_cast<std::uint8_t>(random());
  }
  bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0FU) | 0x40U);
  bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3FU) | 0x80U);

  std::ostringstream value{};
  value << std::hex << std::setfill('0');
  for (std::size_t index = 0U; index < bytes.size(); ++index) {
    if (index == 4U || index == 6U || index == 8U || index == 10U) {
      value << '-';
    }
    value << std::setw(2) << static_cast<unsigned int>(bytes[index]);
  }
  return value.str();
}
#endif

} // namespace

int main(const int argc, char **argv) {
  const auto options = parse_options(argc, argv);
  if (!options.has_value()) {
    print_usage(std::cerr);
    return 2;
  }
  if (options->show_help) {
    print_usage(std::cout);
    return 0;
  }
  if (options->show_version) {
    std::cout << industrial_iot_gateway::project_name() << ' '
              << industrial_iot_gateway::project_version() << '\n';
    return 0;
  }
  if (options->serial_device.empty() || options->register_map.empty() ||
      !complete_write_options(*options)
#ifdef GATEWAY_HAS_MQTT
      || !complete_mqtt_options(*options)
#endif
  ) {
    print_usage(std::cerr);
    return 2;
  }
  if (!industrial_iot_gateway::lifecycle::SynchronousSignalWaiter::
          block_shutdown_signals_for_current_thread()) {
    std::cerr << "failed to block SIGINT/SIGTERM\n";
    return 3;
  }

  auto loaded = industrial_iot_gateway::config::load_runtime_configuration(options->register_map);
  if (!loaded) {
    std::cerr << "register map error: " << loaded.detail << '\n';
    return 4;
  }
  industrial_iot_gateway::runtime::GatewayRuntimeConfig config{};
  config.serial.device_path = options->serial_device;
  config.registers = std::move(*loaded.configuration);
  std::unique_ptr<industrial_iot_gateway::runtime::GatewayRuntime> runtime{};
#ifdef GATEWAY_HAS_MQTT
  if (!options->mqtt_broker_uri.empty()) {
    auto logger =
        std::make_shared<industrial_iot_gateway::observability::JsonlLogWriter>(std::cout);
    industrial_iot_gateway::mqtt::MqttConfig mqtt_config{};
    mqtt_config.broker_uri = options->mqtt_broker_uri;
    mqtt_config.client_id = options->mqtt_client_id;
    mqtt_config.gateway_id = options->gateway_id;
    mqtt_config.run_id = generate_run_id();
    auto sink = std::make_unique<industrial_iot_gateway::mqtt::MqttPublishSink>(
        std::move(mqtt_config), logger);
    runtime = std::make_unique<industrial_iot_gateway::runtime::GatewayRuntime>(
        std::move(config), std::move(logger), std::move(sink));
  } else
#endif
  {
    runtime = std::make_unique<industrial_iot_gateway::runtime::GatewayRuntime>(std::move(config),
                                                                                std::cout);
  }
  if (!runtime->valid() || !runtime->start()) {
    std::cerr << "gateway runtime failed to start\n";
    return 5;
  }

  if (options->write_slave.has_value()) {
    const auto status =
        runtime->submit_write(*options->write_slave, options->write_address.value_or(0U),
                              options->write_value.value_or(0U));
    if (status != industrial_iot_gateway::concurrency::QueuePushStatus::accepted) {
      runtime->request_stop(industrial_iot_gateway::lifecycle::ShutdownReason::service_stop);
      runtime->join();
      std::cerr << "explicit write request was rejected\n";
      return 6;
    }
  }

  std::cout << "{\"event\":\"gateway_ready\",\"component\":\"app\"}\n" << std::flush;
  const auto reason = industrial_iot_gateway::lifecycle::SynchronousSignalWaiter::wait();
  runtime->request_stop(reason);
  runtime->join();
  const auto statistics = runtime->statistics();
  std::cout << "{\"event\":\"gateway_summary\",\"component\":\"app\","
               "\"requests_succeeded\":"
            << statistics.requests_succeeded
            << ",\"requests_failed\":" << statistics.requests_failed
            << ",\"serial_open_successes\":" << statistics.serial_open_successes
            << ",\"mqtt_connected_events\":" << statistics.publisher.connected_events
            << ",\"mqtt_publish_successes\":" << statistics.publisher.publish_successes
            << ",\"mqtt_publish_failures\":" << statistics.publisher.publish_failures
            << ",\"mqtt_queue_high_water\":" << statistics.publisher.queue.maximum_depth
            << ",\"mqtt_coalesced\":" << statistics.publisher.coalesced
            << ",\"mqtt_dropped\":" << statistics.publisher.dropped
            << ",\"mqtt_drain_expired\":" << statistics.publisher.drain_expired
            << ",\"stopped\":" << (statistics.stopped ? "true" : "false") << "}\n"
            << std::flush;
  return statistics.stopped ? 0 : 7;
}
