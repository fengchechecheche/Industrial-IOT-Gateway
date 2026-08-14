#include "industrial_iot_gateway/simulation/pty_bus_harness.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace {

std::atomic<bool> stop_requested{};

void on_signal(int) noexcept { stop_requested.store(true); }

struct Options {
  std::string register_map{};
  std::string scenario_config{};
  bool help{};
};

[[nodiscard]] std::optional<Options> parse_options(const int argc, char **argv) {
  Options options{};
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument = argv[index];
    if (argument == "--help") {
      options.help = true;
    } else if ((argument == "--register-map" || argument == "--scenario-config") &&
               index + 1 < argc) {
      const std::string value = argv[++index];
      if (argument == "--register-map") {
        options.register_map = value;
      } else {
        options.scenario_config = value;
      }
    } else {
      return std::nullopt;
    }
  }
  return options;
}

void usage(std::ostream &output) {
  output << "usage: gateway_pty_bus --register-map PATH --scenario-config PATH\n";
}

int run(const int argc, char **argv) {
  const auto options = parse_options(argc, argv);
  if (!options.has_value()) {
    usage(std::cerr);
    return 2;
  }
  if (options->help) {
    usage(std::cout);
    return 0;
  }
  if (options->register_map.empty() || options->scenario_config.empty()) {
    usage(std::cerr);
    return 2;
  }

  std::signal(SIGTERM, on_signal);
  std::signal(SIGINT, on_signal);
  industrial_iot_gateway::simulation::PtyBusHarness bus(options->register_map,
                                                        options->scenario_config);
  if (!bus.start()) {
    std::cerr << "PTY bus failed to start: " << bus.last_error() << '\n';
    return 3;
  }
  std::cout << "{\"event\":\"pty_bus_ready\",\"path\":\"" << bus.gateway_path() << "\"}\n"
            << std::flush;
  while (!stop_requested.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  bus.stop();
  const auto handled = bus.handled_requests();
  std::cout << "{\"event\":\"pty_bus_stopped\",\"handled_requests\":[" << handled[0] << ','
            << handled[1] << ',' << handled[2] << "]}\n"
            << std::flush;
  return 0;
}

} // namespace

int main(const int argc, char **argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception &error) {
    std::cerr << "unhandled PTY bus error: " << error.what() << '\n';
    return 4;
  } catch (...) {
    std::cerr << "unhandled non-standard PTY bus error\n";
    return 4;
  }
}
