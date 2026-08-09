#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace industrial_iot_gateway::transport {

enum class BaudRate : std::uint32_t {
  baud_9600 = 9600U,
  baud_19200 = 19200U,
  baud_38400 = 38400U,
  baud_115200 = 115200U,
};

enum class Parity {
  none,
  even,
  odd,
};

enum class StopBits {
  one,
  two,
};

struct SerialConfig {
  std::string device_path{};
  BaudRate baud_rate{BaudRate::baud_19200};
  std::uint8_t data_bits{8U};
  Parity parity{Parity::even};
  StopBits stop_bits{StopBits::one};
};

enum class SerialErrorCategory {
  none,
  invalid_configuration,
  already_open,
  not_open,
  open_failed,
  configure_failed,
  wait_failed,
  read_failed,
  write_failed,
  disconnected,
};

struct SerialError {
  SerialErrorCategory category{SerialErrorCategory::none};
  int system_error{};
};

enum class IoStatus {
  completed,
  would_block,
  disconnected,
  error,
};

struct IoResult {
  IoStatus status{IoStatus::error};
  std::size_t bytes_transferred{};
  SerialError error{};
};

struct WaitInterest {
  bool readable{};
  bool writable{};
};

enum class WaitStatus {
  ready,
  timeout,
  disconnected,
  error,
};

struct WaitResult {
  WaitStatus status{WaitStatus::error};
  bool readable{};
  bool writable{};
  bool peer_closed{};
  SerialError error{};
};

namespace detail {
class PosixSerialApi;
} // namespace detail

struct SerialPortTestAccess;

class SerialPort {
public:
  SerialPort();
  ~SerialPort();
  SerialPort(SerialPort &&) noexcept;
  SerialPort &operator=(SerialPort &&) noexcept;
  SerialPort(const SerialPort &) = delete;
  SerialPort &operator=(const SerialPort &) = delete;

  [[nodiscard]] bool open(const SerialConfig &config, SerialError &error) noexcept;
  [[nodiscard]] bool reopen(const SerialConfig &config, SerialError &error) noexcept;
  void close() noexcept;

  [[nodiscard]] bool is_open() const noexcept;
  [[nodiscard]] IoResult read_some(std::uint8_t *buffer, std::size_t capacity) noexcept;
  [[nodiscard]] IoResult write_some(const std::uint8_t *data, std::size_t size) noexcept;
  [[nodiscard]] WaitResult wait(WaitInterest interest,
                                std::chrono::steady_clock::time_point deadline) noexcept;

private:
  explicit SerialPort(std::unique_ptr<detail::PosixSerialApi> api);
  friend struct SerialPortTestAccess;

  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace industrial_iot_gateway::transport
