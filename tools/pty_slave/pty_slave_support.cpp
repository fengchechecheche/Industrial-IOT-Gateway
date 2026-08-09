#include "pty_slave_support.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <poll.h>
#include <pty.h>
#include <string_view>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <variant>

#include <yaml-cpp/yaml.h>

namespace industrial_iot_gateway::pty_slave {
namespace {

constexpr std::size_t kRegisterAddressCount = 65'536U;
constexpr int kPollIntervalMs = 20;

struct WriteRule {
  bool writable{};
  bool has_range{};
  std::uint16_t minimum{};
  std::uint16_t maximum{std::numeric_limits<std::uint16_t>::max()};
  std::array<std::uint16_t, 16U> allowed_values{};
  std::size_t allowed_value_count{};
};

[[nodiscard]] std::optional<std::uint32_t> parse_unsigned(const std::string_view text) noexcept {
  std::uint32_t value{};
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
    return std::nullopt;
  }
  return value;
}

[[nodiscard]] protocol::FunctionCode parse_function(const YAML::Node &node) {
  const auto value = node.as<std::string>();
  if (value == "0x03") {
    return protocol::FunctionCode::read_holding_registers;
  }
  if (value == "0x04") {
    return protocol::FunctionCode::read_input_registers;
  }
  throw std::runtime_error("unsupported register function: " + value);
}

[[nodiscard]] bool is_stop_requested(const StopRequested stop_requested) noexcept {
  return stop_requested != nullptr && stop_requested();
}

[[nodiscard]] std::uint64_t monotonic_time_us() noexcept {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

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

[[nodiscard]] FaultMode parse_fault_mode(const std::string_view value) {
  if (value == "normal") {
    return FaultMode::normal;
  }
  if (value == "exception") {
    return FaultMode::exception_response;
  }
  if (value == "delay") {
    return FaultMode::delayed_response;
  }
  if (value == "silent") {
    return FaultMode::silent;
  }
  if (value == "bad-crc") {
    return FaultMode::bad_crc;
  }
  if (value == "truncated") {
    return FaultMode::truncated_response;
  }
  throw std::runtime_error("unknown fault mode: " + std::string(value));
}

[[nodiscard]] const char *fault_mode_name(const FaultMode mode) noexcept {
  switch (mode) {
  case FaultMode::normal:
    return "normal";
  case FaultMode::exception_response:
    return "exception";
  case FaultMode::delayed_response:
    return "delay";
  case FaultMode::silent:
    return "silent";
  case FaultMode::bad_crc:
    return "bad-crc";
  case FaultMode::truncated_response:
    return "truncated";
  }
  return "unknown";
}

} // namespace

class RegisterBank::Impl {
public:
  std::uint8_t slave_id{};
  std::array<std::uint16_t, kRegisterAddressCount> holding_values{};
  std::array<std::uint16_t, kRegisterAddressCount> input_values{};
  std::array<std::uint8_t, kRegisterAddressCount> holding_present{};
  std::array<std::uint8_t, kRegisterAddressCount> input_present{};
  std::map<std::uint16_t, WriteRule> write_rules{};
};

struct ConfigLoaderAccess {
  static RegisterBank::Impl &impl(RegisterBank &bank) noexcept { return *bank.impl_; }
};

RegisterBank::RegisterBank() : impl_(std::make_unique<Impl>()) {}
RegisterBank::~RegisterBank() = default;
RegisterBank::RegisterBank(RegisterBank &&) noexcept = default;
RegisterBank &RegisterBank::operator=(RegisterBank &&) noexcept = default;

std::uint8_t RegisterBank::slave_id() const noexcept { return impl_->slave_id; }

RegisterOperationResult RegisterBank::read(const protocol::FunctionCode function,
                                           const std::uint16_t start_address,
                                           const std::uint16_t quantity,
                                           protocol::ReadResponse &response) const noexcept {
  if (quantity == 0U || quantity > protocol::kMaxReadRegisterQuantity) {
    return RegisterOperationResult::illegal_value;
  }
  const auto end = static_cast<std::uint32_t>(start_address) + quantity;
  if (end > kRegisterAddressCount) {
    return RegisterOperationResult::illegal_address;
  }

  const auto holding = function == protocol::FunctionCode::read_holding_registers;
  if (!holding && function != protocol::FunctionCode::read_input_registers) {
    return RegisterOperationResult::illegal_address;
  }
  const auto &present = holding ? impl_->holding_present : impl_->input_present;
  const auto &values = holding ? impl_->holding_values : impl_->input_values;
  for (std::uint32_t index = start_address; index < end; ++index) {
    if (present[index] == 0U) {
      return RegisterOperationResult::illegal_address;
    }
  }

  response = {};
  response.slave_id = impl_->slave_id;
  response.function = function;
  response.value_count = quantity;
  for (std::size_t index = 0U; index < quantity; ++index) {
    response.values[index] = values[static_cast<std::size_t>(start_address) + index];
  }
  return RegisterOperationResult::success;
}

RegisterOperationResult RegisterBank::write(const std::uint16_t address,
                                            const std::uint16_t value) noexcept {
  const auto rule_iterator = impl_->write_rules.find(address);
  if (rule_iterator == impl_->write_rules.end() || !rule_iterator->second.writable) {
    return RegisterOperationResult::illegal_address;
  }
  const auto &rule = rule_iterator->second;
  if (rule.allowed_value_count > 0U) {
    const auto begin = rule.allowed_values.begin();
    const auto end = begin + static_cast<std::ptrdiff_t>(rule.allowed_value_count);
    if (std::find(begin, end, value) == end) {
      return RegisterOperationResult::illegal_value;
    }
  } else if (rule.has_range && (value < rule.minimum || value > rule.maximum)) {
    return RegisterOperationResult::illegal_value;
  }
  impl_->holding_values[address] = value;
  return RegisterOperationResult::success;
}

ConfigLoadResult::operator bool() const noexcept { return configuration != nullptr; }

ConfigLoadResult load_runtime_configuration(const std::string &register_map_path,
                                            const std::string &scenario_path,
                                            const std::uint8_t slave_id) noexcept {
  try {
    auto configuration = std::make_unique<RuntimeConfiguration>();
    auto &bank = ConfigLoaderAccess::impl(configuration->registers);
    bank.slave_id = slave_id;

    const auto register_map = YAML::LoadFile(register_map_path);
    const auto devices = register_map["devices"];
    if (!devices || !devices.IsSequence()) {
      throw std::runtime_error("register_map.yaml has no devices sequence");
    }

    YAML::Node selected_device;
    for (const auto &device : devices) {
      if (device["slave_id"].as<unsigned int>() == slave_id) {
        selected_device = YAML::Clone(device);
        break;
      }
    }
    if (!selected_device) {
      throw std::runtime_error("slave_id is not present in register_map.yaml");
    }

    const auto registers = selected_device["registers"];
    if (!registers || !registers.IsSequence()) {
      throw std::runtime_error("selected device has no registers sequence");
    }
    for (const auto &entry : registers) {
      const auto function = parse_function(entry["function"]);
      const auto address = entry["address"].as<std::uint32_t>();
      const auto count = entry["register_count"].as<std::uint32_t>();
      if (count == 0U || address + count > kRegisterAddressCount) {
        throw std::runtime_error("register span exceeds Modbus address space");
      }
      auto &present = function == protocol::FunctionCode::read_holding_registers
                          ? bank.holding_present
                          : bank.input_present;
      for (std::uint32_t offset = 0U; offset < count; ++offset) {
        const auto index = address + offset;
        if (present[index] != 0U) {
          throw std::runtime_error("overlapping register span in selected device");
        }
        present[index] = 1U;
      }

      const auto access = entry["access"].as<std::string>();
      const auto write_function = entry["write_function"];
      if (function == protocol::FunctionCode::read_holding_registers && access == "read_write" &&
          write_function && !write_function.IsNull()) {
        if (count != 1U || write_function.as<std::string>() != "0x06") {
          throw std::runtime_error("writable register must be one word with write function 0x06");
        }
        WriteRule rule{};
        rule.writable = true;
        const auto limits = entry["write_limits"];
        if (!limits) {
          throw std::runtime_error("writable register has no write_limits");
        }
        const auto allowed = limits["allowed_values"];
        if (allowed) {
          if (!allowed.IsSequence() || allowed.size() > rule.allowed_values.size()) {
            throw std::runtime_error("write allowed_values is invalid or too large");
          }
          for (const auto &allowed_value : allowed) {
            rule.allowed_values[rule.allowed_value_count] = allowed_value.as<std::uint16_t>();
            ++rule.allowed_value_count;
          }
        } else {
          rule.has_range = true;
          rule.minimum = limits["min"].as<std::uint16_t>();
          rule.maximum = limits["max"].as<std::uint16_t>();
          if (rule.minimum > rule.maximum) {
            throw std::runtime_error("write limit minimum exceeds maximum");
          }
        }
        bank.write_rules.emplace(static_cast<std::uint16_t>(address), rule);
      }
    }

    const auto scenarios = YAML::LoadFile(scenario_path);
    const auto defaults = scenarios["defaults"];
    if (!defaults) {
      throw std::runtime_error("scenario file has no defaults");
    }
    configuration->fault.delay_ms = defaults["delay_ms"].as<std::uint32_t>();
    configuration->fault.exception_code = defaults["exception_code"].as<std::uint8_t>();
    configuration->fault.truncate_bytes = defaults["truncate_bytes"].as<std::size_t>();

    const auto slave_values = scenarios["initial_values"][std::to_string(slave_id)];
    if (!slave_values) {
      throw std::runtime_error("scenario file has no initial values for selected slave");
    }
    const auto apply_values = [&bank](const YAML::Node &values, const bool holding) {
      if (!values || !values.IsMap()) {
        throw std::runtime_error("initial register values must be a map");
      }
      auto &storage = holding ? bank.holding_values : bank.input_values;
      const auto &present = holding ? bank.holding_present : bank.input_present;
      for (const auto &entry : values) {
        const auto address_text = entry.first.as<std::string>();
        const auto parsed_address = parse_unsigned(address_text);
        if (!parsed_address.has_value() || *parsed_address >= kRegisterAddressCount ||
            present[*parsed_address] == 0U) {
          throw std::runtime_error("initial value addresses an undefined register: " +
                                   address_text);
        }
        const auto value = entry.second.as<std::uint32_t>();
        if (value > std::numeric_limits<std::uint16_t>::max()) {
          throw std::runtime_error("initial register value exceeds uint16");
        }
        storage[*parsed_address] = static_cast<std::uint16_t>(value);
      }
    };
    apply_values(slave_values["holding"], true);
    apply_values(slave_values["input"], false);

    return ConfigLoadResult{std::move(configuration), {}};
  } catch (const std::exception &error) {
    return ConfigLoadResult{nullptr, error.what()};
  } catch (...) {
    return ConfigLoadResult{nullptr, "unknown configuration error"};
  }
}

class PtySlaveServer::Impl {
public:
  Impl(RuntimeConfiguration runtime_configuration, const protocol::ParserTiming parser_timing)
      : configuration(std::move(runtime_configuration)), parser(parser_timing) {}

  RuntimeConfiguration configuration;
  protocol::RtuStreamParser parser;
  int master_fd{-1};
  std::string slave_path{};
  std::string last_error{};

  [[nodiscard]] std::optional<protocol::Adu>
  response_for_request(const protocol::Request &request) {
    if (request_slave_id(request) != configuration.registers.slave_id()) {
      return std::nullopt;
    }

    protocol::Response response{};
    if (configuration.fault.mode == FaultMode::exception_response) {
      response = protocol::ExceptionResponse{
          configuration.registers.slave_id(), request_function(request),
          configuration.fault.exception_code,
          configuration.fault.exception_code >= 1U && configuration.fault.exception_code <= 4U};
    } else if (const auto *read = std::get_if<protocol::ReadRequest>(&request)) {
      protocol::ReadResponse read_response{};
      const auto result = configuration.registers.read(read->function, read->start_address,
                                                       read->quantity, read_response);
      if (result == RegisterOperationResult::success) {
        response = read_response;
      } else {
        const auto exception_code = result == RegisterOperationResult::illegal_value ? 3U : 2U;
        response =
            protocol::ExceptionResponse{read->slave_id, static_cast<std::uint8_t>(read->function),
                                        static_cast<std::uint8_t>(exception_code), true};
      }
    } else if (const auto *write = std::get_if<protocol::WriteSingleRegisterRequest>(&request)) {
      const auto result =
          configuration.registers.write(write->register_address, write->register_value);
      if (result == RegisterOperationResult::success) {
        response = protocol::WriteSingleRegisterResponse{write->slave_id, write->register_address,
                                                         write->register_value};
      } else {
        const auto exception_code = result == RegisterOperationResult::illegal_value ? 3U : 2U;
        response = protocol::ExceptionResponse{
            write->slave_id,
            static_cast<std::uint8_t>(protocol::FunctionCode::write_single_register),
            static_cast<std::uint8_t>(exception_code), true};
      }
    } else {
      return std::nullopt;
    }

    const auto encoded = protocol::encode_response(response);
    const auto *adu = std::get_if<protocol::Adu>(&encoded);
    if (adu == nullptr) {
      last_error = "failed to encode simulator response";
      return std::nullopt;
    }
    auto output = *adu;
    if (configuration.fault.mode == FaultMode::bad_crc && output.size >= 2U) {
      output.bytes[output.size - 2U] ^= 0x01U;
    }
    if (configuration.fault.mode == FaultMode::truncated_response) {
      const auto remove = std::min(configuration.fault.truncate_bytes, output.size);
      output.size -= remove;
    }
    return output;
  }

  [[nodiscard]] bool wait_for_delay(const StopRequested stop_requested) const noexcept {
    auto remaining = std::chrono::milliseconds(configuration.fault.delay_ms);
    constexpr auto quantum = std::chrono::milliseconds(10);
    while (remaining.count() > 0 && !is_stop_requested(stop_requested)) {
      const auto current = std::min(remaining, quantum);
      std::this_thread::sleep_for(current);
      remaining -= current;
    }
    return !is_stop_requested(stop_requested);
  }

  [[nodiscard]] bool write_all(const protocol::Adu &adu, const StopRequested stop_requested) {
    std::size_t offset = 0U;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (offset < adu.size && !is_stop_requested(stop_requested)) {
      const auto written = ::write(master_fd, adu.bytes.data() + offset, adu.size - offset);
      if (written > 0) {
        offset += static_cast<std::size_t>(written);
        continue;
      }
      if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        last_error = std::string("PTY write failed: ") + std::strerror(errno);
        return false;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        last_error = "PTY write deadline exceeded";
        return false;
      }
      pollfd descriptor{master_fd, POLLOUT, 0};
      static_cast<void>(::poll(&descriptor, 1U, kPollIntervalMs));
    }
    return offset == adu.size;
  }
};

PtySlaveServer::PtySlaveServer(RuntimeConfiguration configuration,
                               const protocol::ParserTiming timing)
    : impl_(std::make_unique<Impl>(std::move(configuration), timing)) {}
PtySlaveServer::~PtySlaveServer() {
  if (impl_ != nullptr && impl_->master_fd >= 0) {
    static_cast<void>(::close(impl_->master_fd));
    impl_->master_fd = -1;
  }
}
PtySlaveServer::PtySlaveServer(PtySlaveServer &&) noexcept = default;
PtySlaveServer &PtySlaveServer::operator=(PtySlaveServer &&) noexcept = default;

bool PtySlaveServer::open() {
  if (impl_->master_fd >= 0) {
    impl_->last_error = "PTY server is already open";
    return false;
  }

  termios settings{};
  ::cfmakeraw(&settings);
  settings.c_cflag |= CLOCAL | CREAD | CS8;
  settings.c_cflag &= static_cast<tcflag_t>(~(PARENB | PARODD));
  if (::cfsetispeed(&settings, B19200) != 0 || ::cfsetospeed(&settings, B19200) != 0) {
    impl_->last_error = std::string("failed to configure PTY speed: ") + std::strerror(errno);
    return false;
  }

  int slave_fd = -1;
  std::array<char, 128U> path{};
  if (::openpty(&impl_->master_fd, &slave_fd, path.data(), &settings, nullptr) != 0) {
    impl_->master_fd = -1;
    impl_->last_error = std::string("openpty failed: ") + std::strerror(errno);
    return false;
  }
  impl_->slave_path = path.data();
  static_cast<void>(::close(slave_fd));

  const auto flags = ::fcntl(impl_->master_fd, F_GETFL, 0);
  if (flags < 0 || ::fcntl(impl_->master_fd, F_SETFL, flags | O_NONBLOCK) != 0 ||
      ::fcntl(impl_->master_fd, F_SETFD, FD_CLOEXEC) != 0) {
    impl_->last_error = std::string("failed to configure PTY master fd: ") + std::strerror(errno);
    static_cast<void>(::close(impl_->master_fd));
    impl_->master_fd = -1;
    return false;
  }
  return true;
}

const std::string &PtySlaveServer::slave_path() const noexcept { return impl_->slave_path; }
const std::string &PtySlaveServer::last_error() const noexcept { return impl_->last_error; }

ServerRunResult PtySlaveServer::run(const std::size_t maximum_requests,
                                    const StopRequested stop_requested) {
  if (impl_->master_fd < 0) {
    return ServerRunResult{false, 0U, "PTY server is not open"};
  }

  std::size_t handled = 0U;
  std::array<std::uint8_t, protocol::kMaxModbusAduSize> input{};
  std::array<protocol::ParserEvent, 8U> events{};

  while (!is_stop_requested(stop_requested) &&
         (maximum_requests == 0U || handled < maximum_requests)) {
    pollfd descriptor{impl_->master_fd, POLLIN, 0};
    const auto poll_result = ::poll(&descriptor, 1U, kPollIntervalMs);
    if (poll_result < 0) {
      if (errno == EINTR) {
        continue;
      }
      return ServerRunResult{false, handled,
                             std::string("PTY poll failed: ") + std::strerror(errno)};
    }
    if (poll_result == 0) {
      const auto timed = impl_->parser.on_time_advanced(
          protocol::MonotonicTimeUs{monotonic_time_us()}, events.data(), events.size());
      for (std::size_t index = 0U; index < timed.events_written; ++index) {
        if (std::holds_alternative<protocol::ParserError>(events[index].payload)) {
          std::cerr << "event=request_parse_error result=discarded\n";
        }
      }
      continue;
    }
    if ((descriptor.revents & POLLIN) == 0) {
      if ((descriptor.revents & (POLLHUP | POLLERR)) != 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      continue;
    }

    const auto count = ::read(impl_->master_fd, input.data(), input.size());
    if (count < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR || errno == EIO) {
        continue;
      }
      return ServerRunResult{false, handled,
                             std::string("PTY read failed: ") + std::strerror(errno)};
    }
    if (count == 0) {
      continue;
    }

    std::size_t consumed = 0U;
    const auto input_size = static_cast<std::size_t>(count);
    while (consumed < input_size && (maximum_requests == 0U || handled < maximum_requests)) {
      const auto parsed = impl_->parser.ingest(input.data() + consumed, input_size - consumed,
                                               protocol::MonotonicTimeUs{monotonic_time_us()},
                                               events.data(), events.size());
      consumed += parsed.bytes_consumed;
      for (std::size_t index = 0U; index < parsed.events_written; ++index) {
        const auto *request_event =
            std::get_if<protocol::ParsedRequestEvent>(&events[index].payload);
        if (request_event == nullptr) {
          std::cerr << "event=request_parse_error result=discarded\n";
          continue;
        }
        if (request_slave_id(request_event->request) != impl_->configuration.registers.slave_id()) {
          std::cerr << "event=request_ignored reason=unexpected_slave\n";
          continue;
        }

        ++handled;
        const auto mode = impl_->configuration.fault.mode;
        if (mode == FaultMode::silent) {
          std::cerr << "event=response request=" << handled << " fault=silent result=dropped\n";
          continue;
        }
        auto response = impl_->response_for_request(request_event->request);
        if (!response.has_value()) {
          if (!impl_->last_error.empty()) {
            return ServerRunResult{false, handled, impl_->last_error};
          }
          continue;
        }
        if (mode == FaultMode::delayed_response && !impl_->wait_for_delay(stop_requested)) {
          break;
        }
        if (!impl_->write_all(*response, stop_requested)) {
          return ServerRunResult{false, handled, impl_->last_error};
        }
        std::cerr << "event=response request=" << handled << " fault=" << fault_mode_name(mode)
                  << " bytes=" << response->size << " result=sent\n";
      }
      if (parsed.bytes_consumed == 0U) {
        break;
      }
    }
  }

  if (handled > 0U && !is_stop_requested(stop_requested)) {
    static_cast<void>(::tcdrain(impl_->master_fd));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  return ServerRunResult{true, handled, {}};
}

namespace {

volatile std::sig_atomic_t g_stop_requested = 0;

void handle_stop_signal(int) noexcept { g_stop_requested = 1; }

[[nodiscard]] bool stop_requested_from_signal() noexcept { return g_stop_requested != 0; }

struct CommandLine {
  std::string register_map_path{};
  std::string scenario_path{};
  std::uint8_t slave_id{1U};
  FaultMode fault_mode{FaultMode::normal};
  std::uint32_t delay_ms{};
  std::uint8_t exception_code{};
  std::size_t truncate_bytes{};
  std::size_t maximum_requests{};
  bool has_delay_override{};
  bool has_exception_override{};
  bool has_truncate_override{};
};

struct OptionValue {
  std::string_view option;
  std::string_view text;
};

[[nodiscard]] std::uint32_t parse_option_value(const OptionValue value,
                                               const std::uint32_t maximum) {
  const auto parsed = parse_unsigned(value.text);
  if (!parsed.has_value() || *parsed > maximum) {
    throw std::runtime_error("invalid value for " + std::string(value.option));
  }
  return *parsed;
}

[[nodiscard]] CommandLine parse_command_line(const int argc, char **argv) {
  CommandLine result{};
  for (int index = 1; index < argc; ++index) {
    const std::string_view option{argv[index]};
    if (option == "--help") {
      std::cout << "usage: pty_slave --register-map PATH --scenario-config PATH "
                   "[--slave-id N] [--fault "
                   "normal|exception|delay|silent|bad-crc|truncated] "
                   "[--delay-ms N] [--exception-code N] [--truncate-bytes N] "
                   "[--max-requests N]\n";
      return result;
    }
    if (index + 1 >= argc) {
      throw std::runtime_error("missing value for " + std::string(option));
    }
    const std::string_view value{argv[++index]};
    if (option == "--register-map") {
      result.register_map_path = value;
    } else if (option == "--scenario-config") {
      result.scenario_path = value;
    } else if (option == "--slave-id") {
      result.slave_id = static_cast<std::uint8_t>(parse_option_value({option, value}, 247U));
      if (result.slave_id == 0U) {
        throw std::runtime_error("slave-id must be in 1..247");
      }
    } else if (option == "--fault") {
      result.fault_mode = parse_fault_mode(value);
    } else if (option == "--delay-ms") {
      result.delay_ms = parse_option_value({option, value}, 5'000U);
      result.has_delay_override = true;
    } else if (option == "--exception-code") {
      result.exception_code = static_cast<std::uint8_t>(parse_option_value({option, value}, 255U));
      result.has_exception_override = true;
    } else if (option == "--truncate-bytes") {
      result.truncate_bytes = parse_option_value({option, value}, protocol::kMaxModbusAduSize);
      result.has_truncate_override = true;
    } else if (option == "--max-requests") {
      result.maximum_requests = parse_option_value({option, value}, 1'000'000U);
    } else {
      throw std::runtime_error("unknown option: " + std::string(option));
    }
  }
  return result;
}

} // namespace

int run_pty_slave_main(const int argc, char **argv) {
  try {
    g_stop_requested = 0;
    auto options = parse_command_line(argc, argv);
    if (options.register_map_path.empty() || options.scenario_path.empty()) {
      std::cerr << "event=startup_failed reason=missing_configuration\n";
      return 2;
    }

    auto loaded = load_runtime_configuration(options.register_map_path, options.scenario_path,
                                             options.slave_id);
    if (!loaded) {
      std::cerr << "event=startup_failed reason=config_error detail=" << loaded.error << '\n';
      return 2;
    }
    loaded.configuration->fault.mode = options.fault_mode;
    if (options.has_delay_override) {
      loaded.configuration->fault.delay_ms = options.delay_ms;
    }
    if (options.has_exception_override) {
      loaded.configuration->fault.exception_code = options.exception_code;
    }
    if (options.has_truncate_override) {
      loaded.configuration->fault.truncate_bytes = options.truncate_bytes;
    }

    std::signal(SIGINT, handle_stop_signal);
    std::signal(SIGTERM, handle_stop_signal);
    constexpr protocol::ParserTiming kDefaultTiming{860U, 2'006U};
    PtySlaveServer server(std::move(*loaded.configuration), kDefaultTiming);
    if (!server.open()) {
      std::cerr << "event=startup_failed reason=pty_error detail=" << server.last_error() << '\n';
      return 3;
    }

    std::cout << "event=pty_ready path=" << server.slave_path()
              << " slave_id=" << static_cast<unsigned int>(options.slave_id) << '\n'
              << std::flush;
    const auto result = server.run(options.maximum_requests, stop_requested_from_signal);
    if (!result.success) {
      std::cerr << "event=shutdown result=failed detail=" << result.error << '\n';
      return 4;
    }
    std::cerr << "event=shutdown result=success requests=" << result.requests_handled << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "event=startup_failed reason=exception detail=" << error.what() << '\n';
    return 2;
  } catch (...) {
    std::cerr << "event=startup_failed reason=unknown_exception\n";
    return 2;
  }
}

} // namespace industrial_iot_gateway::pty_slave
