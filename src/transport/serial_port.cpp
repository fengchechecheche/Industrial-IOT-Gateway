#include "industrial_iot_gateway/transport/serial_port.hpp"

#include <cerrno>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include "serial_port_detail.hpp"

namespace industrial_iot_gateway::transport {
namespace detail {
namespace {

class SystemPosixSerialApi final : public PosixSerialApi {
public:
  int open_device(const char *path, const int flags) noexcept override {
    return ::open(path, flags);
  }

  int close_fd(const int fd) noexcept override { return ::close(fd); }

  int get_attributes(const int fd, termios *settings) noexcept override {
    return ::tcgetattr(fd, settings);
  }

  int set_attributes(const int fd, const int action, const termios *settings) noexcept override {
    return ::tcsetattr(fd, action, settings);
  }

  int flush(const int fd, const int queue_selector) noexcept override {
    return ::tcflush(fd, queue_selector);
  }

  ssize_t read_fd(const int fd, void *buffer, const std::size_t size) noexcept override {
    return ::read(fd, buffer, size);
  }

  ssize_t write_fd(const int fd, const void *data, const std::size_t size) noexcept override {
    return ::write(fd, data, size);
  }

  int poll_fds(pollfd *descriptors, const nfds_t count,
               const std::chrono::milliseconds timeout) noexcept override {
    return ::poll(descriptors, count, static_cast<int>(timeout.count()));
  }
};

} // namespace

std::unique_ptr<PosixSerialApi> make_posix_serial_api() {
  return std::make_unique<SystemPosixSerialApi>();
}

} // namespace detail

namespace {

[[nodiscard]] bool baud_to_speed(const BaudRate baud_rate, speed_t &speed) noexcept {
  switch (baud_rate) {
  case BaudRate::baud_9600:
    speed = B9600;
    return true;
  case BaudRate::baud_19200:
    speed = B19200;
    return true;
  case BaudRate::baud_38400:
    speed = B38400;
    return true;
  case BaudRate::baud_115200:
    speed = B115200;
    return true;
  }
  return false;
}

[[nodiscard]] bool valid_parity(const Parity parity) noexcept {
  return parity == Parity::none || parity == Parity::even || parity == Parity::odd;
}

[[nodiscard]] bool valid_stop_bits(const StopBits stop_bits) noexcept {
  return stop_bits == StopBits::one || stop_bits == StopBits::two;
}

[[nodiscard]] bool is_disconnect_error(const int system_error) noexcept {
  return system_error == EIO || system_error == ENXIO || system_error == ENODEV ||
         system_error == EPIPE;
}

[[nodiscard]] int
remaining_timeout_ms(const std::chrono::steady_clock::time_point deadline) noexcept {
  const auto now = std::chrono::steady_clock::now();
  if (now >= deadline) {
    return 0;
  }

  const auto remaining = deadline - now;
  auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
  if (std::chrono::duration_cast<std::chrono::steady_clock::duration>(milliseconds) < remaining) {
    milliseconds += std::chrono::milliseconds(1);
  }
  if (milliseconds.count() > std::numeric_limits<int>::max()) {
    return INT_MAX;
  }
  return static_cast<int>(milliseconds.count());
}

[[nodiscard]] IoResult io_error(const SerialErrorCategory category,
                                const int system_error) noexcept {
  return IoResult{IoStatus::error, 0U, SerialError{category, system_error}};
}

} // namespace

class SerialPort::Impl {
public:
  explicit Impl(std::unique_ptr<detail::PosixSerialApi> api) : api_(std::move(api)) {}

  ~Impl() { close(); }

  [[nodiscard]] bool open(const SerialConfig &config, SerialError &error) noexcept {
    error = {};
    speed_t speed{};
    if (config.device_path.empty() || config.data_bits != 8U ||
        !baud_to_speed(config.baud_rate, speed) || !valid_parity(config.parity) ||
        !valid_stop_bits(config.stop_bits)) {
      error = {SerialErrorCategory::invalid_configuration, EINVAL};
      return false;
    }
    if (fd_ >= 0) {
      error = {SerialErrorCategory::already_open, EALREADY};
      return false;
    }

    const int descriptor =
        api_->open_device(config.device_path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (descriptor < 0) {
      error = {SerialErrorCategory::open_failed, errno};
      return false;
    }
    fd_ = descriptor;

    termios settings{};
    if (api_->get_attributes(fd_, &settings) != 0) {
      return fail_configuration(error);
    }

    ::cfmakeraw(&settings);
    settings.c_cflag &= static_cast<tcflag_t>(~(CSIZE | PARENB | PARODD | CSTOPB));
#ifdef CRTSCTS
    settings.c_cflag &= static_cast<tcflag_t>(~CRTSCTS);
#endif
    settings.c_cflag |= static_cast<tcflag_t>(CS8 | CLOCAL | CREAD);
    settings.c_iflag &= static_cast<tcflag_t>(~(IXON | IXOFF | IXANY | INPCK));

    if (config.parity == Parity::even) {
      settings.c_cflag |= PARENB;
      settings.c_iflag |= INPCK;
    } else if (config.parity == Parity::odd) {
      settings.c_cflag |= static_cast<tcflag_t>(PARENB | PARODD);
      settings.c_iflag |= INPCK;
    }
    if (config.stop_bits == StopBits::two) {
      settings.c_cflag |= CSTOPB;
    }

    settings.c_cc[VMIN] = 0U;
    settings.c_cc[VTIME] = 0U;
    if (::cfsetispeed(&settings, speed) != 0 || ::cfsetospeed(&settings, speed) != 0 ||
        api_->set_attributes(fd_, TCSANOW, &settings) != 0 || api_->flush(fd_, TCIOFLUSH) != 0) {
      return fail_configuration(error);
    }
    return true;
  }

  void close() noexcept {
    if (fd_ < 0) {
      return;
    }
    const int descriptor = fd_;
    fd_ = -1;
    static_cast<void>(api_->close_fd(descriptor));
  }

  [[nodiscard]] bool is_open() const noexcept { return fd_ >= 0; }

  [[nodiscard]] IoResult read_some(std::uint8_t *buffer, const std::size_t capacity) noexcept {
    if (capacity == 0U) {
      return IoResult{IoStatus::completed, 0U, {}};
    }
    if (buffer == nullptr) {
      return io_error(SerialErrorCategory::invalid_configuration, EINVAL);
    }
    if (fd_ < 0) {
      return io_error(SerialErrorCategory::not_open, EBADF);
    }

    while (true) {
      const auto result = api_->read_fd(fd_, buffer, capacity);
      if (result > 0) {
        return IoResult{IoStatus::completed, static_cast<std::size_t>(result), {}};
      }
      if (result == 0) {
        return IoResult{IoStatus::would_block, 0U, {}};
      }
      const int system_error = errno;
      if (system_error == EINTR) {
        continue;
      }
      if (system_error == EAGAIN || system_error == EWOULDBLOCK) {
        return IoResult{IoStatus::would_block, 0U, {}};
      }
      if (is_disconnect_error(system_error)) {
        close();
        return IoResult{IoStatus::disconnected, 0U,
                        SerialError{SerialErrorCategory::disconnected, system_error}};
      }
      return io_error(SerialErrorCategory::read_failed, system_error);
    }
  }

  [[nodiscard]] IoResult write_some(const std::uint8_t *data, const std::size_t size) noexcept {
    if (size == 0U) {
      return IoResult{IoStatus::completed, 0U, {}};
    }
    if (data == nullptr) {
      return io_error(SerialErrorCategory::invalid_configuration, EINVAL);
    }
    if (fd_ < 0) {
      return io_error(SerialErrorCategory::not_open, EBADF);
    }

    while (true) {
      const auto result = api_->write_fd(fd_, data, size);
      if (result > 0) {
        return IoResult{IoStatus::completed, static_cast<std::size_t>(result), {}};
      }
      if (result == 0) {
        return IoResult{IoStatus::would_block, 0U, {}};
      }
      const int system_error = errno;
      if (system_error == EINTR) {
        continue;
      }
      if (system_error == EAGAIN || system_error == EWOULDBLOCK) {
        return IoResult{IoStatus::would_block, 0U, {}};
      }
      if (is_disconnect_error(system_error)) {
        close();
        return IoResult{IoStatus::disconnected, 0U,
                        SerialError{SerialErrorCategory::disconnected, system_error}};
      }
      return io_error(SerialErrorCategory::write_failed, system_error);
    }
  }

  [[nodiscard]] WaitResult wait(const WaitInterest interest,
                                const std::chrono::steady_clock::time_point deadline) noexcept {
    if (!interest.readable && !interest.writable) {
      return WaitResult{WaitStatus::error, false, false, false,
                        SerialError{SerialErrorCategory::invalid_configuration, EINVAL}};
    }
    if (fd_ < 0) {
      return WaitResult{WaitStatus::error, false, false, false,
                        SerialError{SerialErrorCategory::not_open, EBADF}};
    }

    while (true) {
      pollfd descriptor{};
      descriptor.fd = fd_;
      if (interest.readable) {
        descriptor.events |= POLLIN;
      }
      if (interest.writable) {
        descriptor.events |= POLLOUT;
      }

      const int result = api_->poll_fds(&descriptor, 1U,
                                        std::chrono::milliseconds(remaining_timeout_ms(deadline)));
      if (result == 0) {
        return WaitResult{WaitStatus::timeout, false, false, false, {}};
      }
      if (result < 0) {
        const int system_error = errno;
        if (system_error == EINTR) {
          continue;
        }
        return WaitResult{WaitStatus::error, false, false, false,
                          SerialError{SerialErrorCategory::wait_failed, system_error}};
      }

      const bool readable = (descriptor.revents & POLLIN) != 0;
      const bool writable = (descriptor.revents & POLLOUT) != 0;
      const bool peer_closed = (descriptor.revents & POLLHUP) != 0;
      const bool fatal_event = (descriptor.revents & (POLLERR | POLLNVAL)) != 0;
      if (fatal_event || (peer_closed && !readable)) {
        close();
        return WaitResult{WaitStatus::disconnected, false, false, true,
                          SerialError{SerialErrorCategory::disconnected, 0}};
      }
      if (readable || writable) {
        return WaitResult{WaitStatus::ready, readable, writable, peer_closed, {}};
      }

      return WaitResult{WaitStatus::error, false, false, peer_closed,
                        SerialError{SerialErrorCategory::wait_failed, EIO}};
    }
  }

private:
  [[nodiscard]] bool fail_configuration(SerialError &error) noexcept {
    const int system_error = errno;
    close();
    error = {SerialErrorCategory::configure_failed, system_error};
    return false;
  }

  std::unique_ptr<detail::PosixSerialApi> api_;
  int fd_{-1};
};

SerialPort::SerialPort() : SerialPort(detail::make_posix_serial_api()) {}

SerialPort::SerialPort(std::unique_ptr<detail::PosixSerialApi> api)
    : impl_(std::make_unique<Impl>(std::move(api))) {}

SerialPort::~SerialPort() = default;
SerialPort::SerialPort(SerialPort &&) noexcept = default;
SerialPort &SerialPort::operator=(SerialPort &&) noexcept = default;

bool SerialPort::open(const SerialConfig &config, SerialError &error) noexcept {
  if (!impl_) {
    error = {SerialErrorCategory::not_open, EBADF};
    return false;
  }
  return impl_->open(config, error);
}

bool SerialPort::reopen(const SerialConfig &config, SerialError &error) noexcept {
  close();
  return open(config, error);
}

void SerialPort::close() noexcept {
  if (impl_) {
    impl_->close();
  }
}

bool SerialPort::is_open() const noexcept { return impl_ && impl_->is_open(); }

IoResult SerialPort::read_some(std::uint8_t *buffer, const std::size_t capacity) noexcept {
  if (!impl_) {
    return io_error(SerialErrorCategory::not_open, EBADF);
  }
  return impl_->read_some(buffer, capacity);
}

IoResult SerialPort::write_some(const std::uint8_t *data, const std::size_t size) noexcept {
  if (!impl_) {
    return io_error(SerialErrorCategory::not_open, EBADF);
  }
  return impl_->write_some(data, size);
}

WaitResult SerialPort::wait(const WaitInterest interest,
                            const std::chrono::steady_clock::time_point deadline) noexcept {
  if (!impl_) {
    return WaitResult{WaitStatus::error, false, false, false,
                      SerialError{SerialErrorCategory::not_open, EBADF}};
  }
  return impl_->wait(interest, deadline);
}

} // namespace industrial_iot_gateway::transport
