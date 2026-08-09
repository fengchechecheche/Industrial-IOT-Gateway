#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>

#include <poll.h>
#include <sys/types.h>
#include <termios.h>

namespace industrial_iot_gateway::transport::detail {

class PosixSerialApi {
public:
  virtual ~PosixSerialApi() = default;

  [[nodiscard]] virtual int open_device(const char *path, int flags) noexcept = 0;
  [[nodiscard]] virtual int close_fd(int fd) noexcept = 0;
  [[nodiscard]] virtual int get_attributes(int fd, termios *settings) noexcept = 0;
  [[nodiscard]] virtual int set_attributes(int fd, int action,
                                           const termios *settings) noexcept = 0;
  [[nodiscard]] virtual int flush(int fd, int queue_selector) noexcept = 0;
  [[nodiscard]] virtual ssize_t read_fd(int fd, void *buffer, std::size_t size) noexcept = 0;
  [[nodiscard]] virtual ssize_t write_fd(int fd, const void *data, std::size_t size) noexcept = 0;
  [[nodiscard]] virtual int poll_fds(pollfd *descriptors, nfds_t count,
                                     std::chrono::milliseconds timeout) noexcept = 0;
};

[[nodiscard]] std::unique_ptr<PosixSerialApi> make_posix_serial_api();

} // namespace industrial_iot_gateway::transport::detail
