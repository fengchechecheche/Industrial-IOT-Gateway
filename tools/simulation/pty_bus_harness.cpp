#include "industrial_iot_gateway/simulation/pty_bus_harness.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <poll.h>
#include <pty.h>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace industrial_iot_gateway::simulation {
namespace {

constexpr protocol::ParserTiming kParserTiming{860U, 2'006U};
std::atomic<bool> *active_stop_flag = nullptr;
std::atomic<std::uint64_t> next_alias_id{1U};

[[nodiscard]] bool stop_requested() noexcept {
  return active_stop_flag != nullptr && active_stop_flag->load();
}

[[nodiscard]] bool set_nonblocking(const int descriptor) noexcept {
  const auto flags = ::fcntl(descriptor, F_GETFL, 0);
  return flags >= 0 && ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == 0 &&
         ::fcntl(descriptor, F_SETFD, FD_CLOEXEC) == 0;
}

[[nodiscard]] bool configure_raw(const int descriptor) noexcept {
  termios settings{};
  if (::tcgetattr(descriptor, &settings) != 0) {
    return false;
  }
  ::cfmakeraw(&settings);
  settings.c_cflag |= CLOCAL | CREAD | CS8;
  settings.c_cflag &= static_cast<tcflag_t>(~(PARENB | PARODD));
  return ::cfsetispeed(&settings, B19200) == 0 && ::cfsetospeed(&settings, B19200) == 0 &&
         ::tcsetattr(descriptor, TCSANOW, &settings) == 0;
}

void write_best_effort(const int descriptor, const std::uint8_t *data,
                       const std::size_t size) noexcept {
  std::size_t offset = 0U;
  while (offset < size && !stop_requested()) {
    const auto count = ::write(descriptor, data + offset, size - offset);
    if (count > 0) {
      offset += static_cast<std::size_t>(count);
      continue;
    }
    if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
      return;
    }
    pollfd value{descriptor, POLLOUT, 0};
    static_cast<void>(::poll(&value, 1U, 10));
  }
}

} // namespace

PtyBusHarness::PtyBusHarness(std::string register_map_path, std::string scenario_path,
                             std::array<pty_slave::FaultPlan, 3U> faults)
    : register_map_path_(std::move(register_map_path)), scenario_path_(std::move(scenario_path)),
      faults_(faults) {}

PtyBusHarness::~PtyBusHarness() { stop(); }

bool PtyBusHarness::start() {
  if (gateway_master_fd_ >= 0 || relay_thread_.joinable()) {
    last_error_ = "PTY bus is already started";
    return false;
  }
  stop_requested_.store(false);
  relay_stop_requested_.store(false);
  active_stop_flag = &stop_requested_;

  gateway_path_ = "/tmp/industrial_iot_gateway_t05_bus_" + std::to_string(::getpid()) + "_" +
                  std::to_string(next_alias_id.fetch_add(1U));
  if (!create_gateway_pty() || !update_gateway_alias()) {
    active_stop_flag = nullptr;
    close_descriptors();
    return false;
  }

  for (std::size_t index = 0U; index < servers_.size(); ++index) {
    const auto slave_id = static_cast<std::uint8_t>(index + 1U);
    auto loaded =
        pty_slave::load_runtime_configuration(register_map_path_, scenario_path_, slave_id);
    if (!loaded) {
      last_error_ = loaded.error;
      stop();
      return false;
    }
    loaded.configuration->fault = faults_[index];
    servers_[index] = std::make_unique<pty_slave::PtySlaveServer>(std::move(*loaded.configuration),
                                                                  kParserTiming, false);
    if (!servers_[index]->open()) {
      last_error_ = servers_[index]->last_error();
      stop();
      return false;
    }
    slave_fds_[index] =
        ::open(servers_[index]->slave_path().c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (slave_fds_[index] < 0 || !configure_raw(slave_fds_[index])) {
      last_error_ = std::string("failed to open simulator PTY: ") + std::strerror(errno);
      stop();
      return false;
    }
  }

  for (std::size_t index = 0U; index < servers_.size(); ++index) {
    server_threads_[index] = std::thread(
        [this, index] { server_results_[index] = servers_[index]->run(0U, stop_requested); });
  }
  relay_thread_ = std::thread([this] { relay_loop(); });
  return true;
}

bool PtyBusHarness::disconnect_and_reconnect(const std::chrono::milliseconds outage) {
  if (!relay_thread_.joinable() || gateway_master_fd_ < 0) {
    last_error_ = "PTY bus is not running";
    return false;
  }
  relay_stop_requested_.store(true);
  relay_thread_.join();
  const auto disconnected_master_fd = gateway_master_fd_;
  gateway_master_fd_ = -1;
  if (!create_gateway_pty() || !update_gateway_alias()) {
    static_cast<void>(::close(disconnected_master_fd));
    return false;
  }
  static_cast<void>(::close(disconnected_master_fd));
  std::this_thread::sleep_for(outage);
  std::array<std::uint8_t, 256U> stale_requests{};
  while (::read(gateway_master_fd_, stale_requests.data(), stale_requests.size()) > 0) {
  }
  relay_stop_requested_.store(false);
  relay_thread_ = std::thread([this] { relay_loop(); });
  return true;
}

bool PtyBusHarness::set_fault(const std::uint8_t slave_id, pty_slave::FaultPlan fault) noexcept {
  if (slave_id == 0U || slave_id > servers_.size()) {
    return false;
  }
  auto &server = servers_[slave_id - 1U];
  if (server == nullptr) {
    return false;
  }
  server->set_fault_plan(fault);
  return true;
}

void PtyBusHarness::stop() noexcept {
  stop_requested_.store(true);
  relay_stop_requested_.store(true);
  if (relay_thread_.joinable()) {
    relay_thread_.join();
  }
  for (auto &thread : server_threads_) {
    if (thread.joinable()) {
      thread.join();
    }
  }
  close_descriptors();
  if (!gateway_path_.empty()) {
    static_cast<void>(::unlink(gateway_path_.c_str()));
  }
  active_stop_flag = nullptr;
}

const std::string &PtyBusHarness::gateway_path() const noexcept { return gateway_path_; }
const std::string &PtyBusHarness::last_error() const noexcept { return last_error_; }

std::array<std::size_t, 3U> PtyBusHarness::handled_requests() const noexcept {
  return {server_results_[0].requests_handled, server_results_[1].requests_handled,
          server_results_[2].requests_handled};
}

void PtyBusHarness::relay_loop() noexcept {
  std::array<std::uint8_t, 8U> request{};
  std::size_t request_size = 0U;
  std::optional<std::size_t> active_slave{};
  auto active_since = std::chrono::steady_clock::now();
  auto last_response_byte = active_since;
  std::size_t expected_response_size = 0U;
  std::size_t response_size = 0U;
  std::array<std::uint8_t, 3U> response_prefix{};

  while (!stop_requested_.load() && !relay_stop_requested_.load()) {
    std::array<pollfd, 4U> descriptors{};
    descriptors[0] = pollfd{gateway_master_fd_, POLLIN, 0};
    for (std::size_t index = 0U; index < slave_fds_.size(); ++index) {
      const auto events = static_cast<short>(active_slave == index ? POLLIN : 0);
      descriptors[index + 1U] = pollfd{slave_fds_[index], events, 0};
    }
    const auto polled = ::poll(descriptors.data(), descriptors.size(), 5);
    if (polled < 0 && errno == EINTR) {
      continue;
    }
    const auto now = std::chrono::steady_clock::now();

    if ((descriptors[0].revents & POLLIN) != 0 && !active_slave.has_value()) {
      const auto count =
          ::read(gateway_master_fd_, request.data() + request_size, request.size() - request_size);
      if (count > 0) {
        request_size += static_cast<std::size_t>(count);
        if (request_size == request.size()) {
          const auto slave = request[0];
          if (slave >= 1U && slave <= slave_fds_.size()) {
            active_slave = static_cast<std::size_t>(slave - 1U);
            active_since = now;
            last_response_byte = now;
            expected_response_size = 0U;
            response_size = 0U;
            response_prefix = {};
            write_best_effort(slave_fds_[*active_slave], request.data(), request.size());
          }
          request_size = 0U;
        }
      }
    }

    if (active_slave.has_value()) {
      const auto descriptor_index = *active_slave + 1U;
      if ((descriptors[descriptor_index].revents & POLLIN) != 0) {
        std::array<std::uint8_t, 256U> response{};
        const auto count = ::read(slave_fds_[*active_slave], response.data(), response.size());
        if (count > 0) {
          const auto size = static_cast<std::size_t>(count);
          write_best_effort(gateway_master_fd_, response.data(), size);
          if (expected_response_size == 0U) {
            for (std::size_t index = 0U; index < size && response_size + index < 3U; ++index) {
              response_prefix[response_size + index] = response[index];
            }
            if (response_size + size >= 3U) {
              expected_response_size =
                  (response_prefix[1] & 0x80U) != 0U
                      ? 5U
                      : (response_prefix[1] == 0x06U ? 8U : 5U + response_prefix[2]);
            }
          }
          response_size += size;
          last_response_byte = now;
        }
      }
      const auto complete = expected_response_size > 0U && response_size >= expected_response_size;
      const auto truncated_quiet =
          response_size > 0U && now - last_response_byte > std::chrono::milliseconds(10);
      const auto silent_timeout = now - active_since > std::chrono::milliseconds(700);
      if (complete || truncated_quiet || silent_timeout) {
        active_slave.reset();
      }
    }
  }
}

bool PtyBusHarness::create_gateway_pty() {
  termios settings{};
  ::cfmakeraw(&settings);
  settings.c_cflag |= CLOCAL | CREAD | CS8;
  settings.c_cflag &= static_cast<tcflag_t>(~(PARENB | PARODD));
  static_cast<void>(::cfsetispeed(&settings, B19200));
  static_cast<void>(::cfsetospeed(&settings, B19200));
  int gateway_slave_fd = -1;
  std::array<char, 128U> path{};
  if (::openpty(&gateway_master_fd_, &gateway_slave_fd, path.data(), &settings, nullptr) != 0) {
    last_error_ = std::string("gateway openpty failed: ") + std::strerror(errno);
    return false;
  }
  gateway_actual_path_ = path.data();
  static_cast<void>(::close(gateway_slave_fd));
  if (!set_nonblocking(gateway_master_fd_)) {
    last_error_ = "failed to configure gateway PTY master";
    static_cast<void>(::close(gateway_master_fd_));
    gateway_master_fd_ = -1;
    return false;
  }
  return true;
}

bool PtyBusHarness::update_gateway_alias() {
  const auto pending = gateway_path_ + ".next";
  static_cast<void>(::unlink(pending.c_str()));
  if (::symlink(gateway_actual_path_.c_str(), pending.c_str()) != 0 ||
      ::rename(pending.c_str(), gateway_path_.c_str()) != 0) {
    last_error_ = std::string("failed to update gateway PTY alias: ") + std::strerror(errno);
    static_cast<void>(::unlink(pending.c_str()));
    return false;
  }
  return true;
}

void PtyBusHarness::close_descriptors() noexcept {
  for (auto &descriptor : slave_fds_) {
    if (descriptor >= 0) {
      static_cast<void>(::close(descriptor));
      descriptor = -1;
    }
  }
  if (gateway_master_fd_ >= 0) {
    static_cast<void>(::close(gateway_master_fd_));
    gateway_master_fd_ = -1;
  }
}

} // namespace industrial_iot_gateway::simulation
