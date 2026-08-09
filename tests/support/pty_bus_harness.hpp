#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "pty_slave_support.hpp"

namespace industrial_iot_gateway::test_support {

class PtyBusHarness {
public:
  PtyBusHarness(std::string register_map_path, std::string scenario_path,
                std::array<pty_slave::FaultPlan, 3U> faults = {});
  ~PtyBusHarness();
  PtyBusHarness(const PtyBusHarness &) = delete;
  PtyBusHarness &operator=(const PtyBusHarness &) = delete;

  [[nodiscard]] bool start();
  [[nodiscard]] bool disconnect_and_reconnect(std::chrono::milliseconds outage);
  [[nodiscard]] bool set_fault(std::uint8_t slave_id, pty_slave::FaultPlan fault) noexcept;
  void stop() noexcept;
  [[nodiscard]] const std::string &gateway_path() const noexcept;
  [[nodiscard]] const std::string &last_error() const noexcept;
  [[nodiscard]] std::array<std::size_t, 3U> handled_requests() const noexcept;

private:
  void relay_loop() noexcept;
  [[nodiscard]] bool create_gateway_pty();
  [[nodiscard]] bool update_gateway_alias();
  void close_descriptors() noexcept;

  std::string register_map_path_;
  std::string scenario_path_;
  std::array<pty_slave::FaultPlan, 3U> faults_{};
  std::array<std::unique_ptr<pty_slave::PtySlaveServer>, 3U> servers_{};
  std::array<int, 3U> slave_fds_{{-1, -1, -1}};
  std::array<pty_slave::ServerRunResult, 3U> server_results_{};
  std::array<std::thread, 3U> server_threads_{};
  int gateway_master_fd_{-1};
  std::string gateway_actual_path_{};
  std::string gateway_path_{};
  std::string last_error_{};
  std::atomic<bool> stop_requested_{};
  std::atomic<bool> relay_stop_requested_{};
  std::thread relay_thread_{};
};

} // namespace industrial_iot_gateway::test_support
