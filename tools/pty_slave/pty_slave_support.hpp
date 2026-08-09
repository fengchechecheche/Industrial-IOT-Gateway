#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "industrial_iot_gateway/protocol/modbus_codec.hpp"
#include "industrial_iot_gateway/protocol/rtu_stream_parser.hpp"

namespace industrial_iot_gateway::pty_slave {

enum class FaultMode {
  normal,
  exception_response,
  delayed_response,
  silent,
  bad_crc,
  truncated_response,
};

struct FaultPlan {
  FaultMode mode{FaultMode::normal};
  std::uint32_t delay_ms{40U};
  std::uint8_t exception_code{2U};
  std::size_t truncate_bytes{2U};
};

enum class RegisterOperationResult {
  success,
  illegal_address,
  illegal_value,
};

class RegisterBank {
public:
  RegisterBank();
  ~RegisterBank();
  RegisterBank(RegisterBank &&) noexcept;
  RegisterBank &operator=(RegisterBank &&) noexcept;
  RegisterBank(const RegisterBank &) = delete;
  RegisterBank &operator=(const RegisterBank &) = delete;

  [[nodiscard]] std::uint8_t slave_id() const noexcept;
  [[nodiscard]] RegisterOperationResult read(protocol::FunctionCode function,
                                             std::uint16_t start_address, std::uint16_t quantity,
                                             protocol::ReadResponse &response) const noexcept;
  [[nodiscard]] RegisterOperationResult write(std::uint16_t address, std::uint16_t value) noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
  friend struct ConfigLoaderAccess;
};

struct RuntimeConfiguration {
  RegisterBank registers{};
  FaultPlan fault{};
};

struct ConfigLoadResult {
  std::unique_ptr<RuntimeConfiguration> configuration{};
  std::string error{};

  [[nodiscard]] explicit operator bool() const noexcept;
};

[[nodiscard]] ConfigLoadResult load_runtime_configuration(const std::string &register_map_path,
                                                          const std::string &scenario_path,
                                                          std::uint8_t slave_id) noexcept;

struct ServerRunResult {
  bool success{};
  std::size_t requests_handled{};
  std::string error{};
};

using StopRequested = bool (*)() noexcept;

class PtySlaveServer {
public:
  PtySlaveServer(RuntimeConfiguration configuration, protocol::ParserTiming timing);
  ~PtySlaveServer();
  PtySlaveServer(PtySlaveServer &&) noexcept;
  PtySlaveServer &operator=(PtySlaveServer &&) noexcept;
  PtySlaveServer(const PtySlaveServer &) = delete;
  PtySlaveServer &operator=(const PtySlaveServer &) = delete;

  [[nodiscard]] bool open();
  [[nodiscard]] const std::string &slave_path() const noexcept;
  [[nodiscard]] const std::string &last_error() const noexcept;
  void set_fault_plan(FaultPlan fault) noexcept;
  [[nodiscard]] ServerRunResult run(std::size_t maximum_requests, StopRequested stop_requested);

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

[[nodiscard]] int run_pty_slave_main(int argc, char **argv);

} // namespace industrial_iot_gateway::pty_slave
