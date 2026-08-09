#include "industrial_iot_gateway/transport/serial_port.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

#include <fcntl.h>
#include <gtest/gtest.h>
#include <poll.h>
#include <pty.h>
#include <unistd.h>

namespace industrial_iot_gateway::transport {
namespace {

class PtyPair {
public:
  PtyPair() {
    std::array<char, 128U> path{};
    if (::openpty(&master_fd_, &slave_fd_, path.data(), nullptr, nullptr) != 0) {
      return;
    }
    slave_path_ = path.data();
    static_cast<void>(::close(slave_fd_));
    slave_fd_ = -1;
    const int flags = ::fcntl(master_fd_, F_GETFL, 0);
    if (flags < 0 || ::fcntl(master_fd_, F_SETFL, flags | O_NONBLOCK) != 0) {
      close_master();
    }
  }

  ~PtyPair() {
    close_master();
    if (slave_fd_ >= 0) {
      static_cast<void>(::close(slave_fd_));
    }
  }

  PtyPair(const PtyPair &) = delete;
  PtyPair &operator=(const PtyPair &) = delete;

  [[nodiscard]] bool valid() const noexcept { return master_fd_ >= 0 && !slave_path_.empty(); }

  [[nodiscard]] int master_fd() const noexcept { return master_fd_; }
  [[nodiscard]] const std::string &slave_path() const noexcept { return slave_path_; }

  void close_master() noexcept {
    if (master_fd_ < 0) {
      return;
    }
    static_cast<void>(::close(master_fd_));
    master_fd_ = -1;
  }

private:
  int master_fd_{-1};
  int slave_fd_{-1};
  std::string slave_path_{};
};

[[nodiscard]] SerialConfig pty_config(const std::string &path) {
  SerialConfig config{};
  config.device_path = path;
  config.parity = Parity::none;
  config.stop_bits = StopBits::one;
  return config;
}

[[nodiscard]] bool write_all(const int fd, const std::uint8_t *data, const std::size_t size) {
  std::size_t offset{};
  while (offset < size) {
    const auto result = ::write(fd, data + offset, size - offset);
    if (result > 0) {
      offset += static_cast<std::size_t>(result);
      continue;
    }
    if (result < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

[[nodiscard]] std::size_t read_with_timeout(const int fd, std::uint8_t *buffer,
                                            const std::size_t capacity) {
  pollfd descriptor{};
  descriptor.fd = fd;
  descriptor.events = POLLIN;
  int poll_result{};
  do {
    poll_result = ::poll(&descriptor, 1U, 500);
  } while (poll_result < 0 && errno == EINTR);
  if (poll_result <= 0 || (descriptor.revents & POLLIN) == 0) {
    return 0U;
  }

  ssize_t read_result{};
  do {
    read_result = ::read(fd, buffer, capacity);
  } while (read_result < 0 && errno == EINTR);
  return read_result > 0 ? static_cast<std::size_t>(read_result) : 0U;
}

TEST(SerialPortPtyIntegrationTest, RoundTripsBytesThroughRealPty) {
  PtyPair pair{};
  ASSERT_TRUE(pair.valid());
  SerialPort port{};
  SerialError error{};
  ASSERT_TRUE(port.open(pty_config(pair.slave_path()), error));

  const std::array<std::uint8_t, 4U> inbound{0x01U, 0x03U, 0x00U, 0x10U};
  ASSERT_TRUE(write_all(pair.master_fd(), inbound.data(), inbound.size()));
  const auto wait_read = port.wait(WaitInterest{true, false},
                                   std::chrono::steady_clock::now() + std::chrono::seconds(1));
  ASSERT_EQ(wait_read.status, WaitStatus::ready);
  ASSERT_TRUE(wait_read.readable);
  std::array<std::uint8_t, 16U> read_buffer{};
  const auto read_result = port.read_some(read_buffer.data(), read_buffer.size());
  ASSERT_EQ(read_result.status, IoStatus::completed);
  ASSERT_EQ(read_result.bytes_transferred, inbound.size());
  EXPECT_TRUE(std::equal(inbound.begin(), inbound.end(), read_buffer.begin()));

  const std::array<std::uint8_t, 5U> outbound{0x01U, 0x03U, 0x02U, 0x12U, 0x34U};
  const auto write_result = port.write_some(outbound.data(), outbound.size());
  ASSERT_EQ(write_result.status, IoStatus::completed);
  ASSERT_EQ(write_result.bytes_transferred, outbound.size());
  std::array<std::uint8_t, 16U> master_buffer{};
  const auto master_count =
      read_with_timeout(pair.master_fd(), master_buffer.data(), master_buffer.size());
  ASSERT_EQ(master_count, outbound.size());
  EXPECT_TRUE(std::equal(outbound.begin(), outbound.end(), master_buffer.begin()));
}

TEST(SerialPortPtyIntegrationTest, PreservesFragmentsAndReportsNoDataWithoutSpinning) {
  PtyPair pair{};
  ASSERT_TRUE(pair.valid());
  SerialPort port{};
  SerialError error{};
  ASSERT_TRUE(port.open(pty_config(pair.slave_path()), error));
  std::array<std::uint8_t, 8U> buffer{};

  const auto empty_result = port.read_some(buffer.data(), buffer.size());
  EXPECT_EQ(empty_result.status, IoStatus::would_block);

  const std::array<std::uint8_t, 2U> first{0xAAU, 0xBBU};
  ASSERT_TRUE(write_all(pair.master_fd(), first.data(), first.size()));
  ASSERT_EQ(port.wait(WaitInterest{true, false},
                      std::chrono::steady_clock::now() + std::chrono::seconds(1))
                .status,
            WaitStatus::ready);
  const auto first_read = port.read_some(buffer.data(), buffer.size());
  ASSERT_EQ(first_read.status, IoStatus::completed);
  ASSERT_EQ(first_read.bytes_transferred, first.size());
  EXPECT_TRUE(std::equal(first.begin(), first.end(), buffer.begin()));

  const std::array<std::uint8_t, 3U> second{0xCCU, 0xDDU, 0xEEU};
  ASSERT_TRUE(write_all(pair.master_fd(), second.data(), second.size()));
  ASSERT_EQ(port.wait(WaitInterest{true, false},
                      std::chrono::steady_clock::now() + std::chrono::seconds(1))
                .status,
            WaitStatus::ready);
  const auto second_read = port.read_some(buffer.data(), buffer.size());
  ASSERT_EQ(second_read.status, IoStatus::completed);
  ASSERT_EQ(second_read.bytes_transferred, second.size());
  EXPECT_TRUE(std::equal(second.begin(), second.end(), buffer.begin()));
}

TEST(SerialPortPtyIntegrationTest, DetectsDisconnectAndReopensOnANewPty) {
  PtyPair first_pair{};
  ASSERT_TRUE(first_pair.valid());
  SerialPort port{};
  SerialError error{};
  ASSERT_TRUE(port.open(pty_config(first_pair.slave_path()), error));

  first_pair.close_master();
  const auto disconnected = port.wait(WaitInterest{true, false},
                                      std::chrono::steady_clock::now() + std::chrono::seconds(1));
  ASSERT_EQ(disconnected.status, WaitStatus::disconnected);
  EXPECT_EQ(disconnected.error.category, SerialErrorCategory::disconnected);
  EXPECT_FALSE(port.is_open());

  PtyPair second_pair{};
  ASSERT_TRUE(second_pair.valid());
  ASSERT_TRUE(port.reopen(pty_config(second_pair.slave_path()), error));
  ASSERT_TRUE(port.is_open());

  const std::array<std::uint8_t, 3U> data{0x11U, 0x22U, 0x33U};
  ASSERT_TRUE(write_all(second_pair.master_fd(), data.data(), data.size()));
  ASSERT_EQ(port.wait(WaitInterest{true, false},
                      std::chrono::steady_clock::now() + std::chrono::seconds(1))
                .status,
            WaitStatus::ready);
  std::array<std::uint8_t, 8U> buffer{};
  const auto result = port.read_some(buffer.data(), buffer.size());
  ASSERT_EQ(result.status, IoStatus::completed);
  ASSERT_EQ(result.bytes_transferred, data.size());
  EXPECT_TRUE(std::equal(data.begin(), data.end(), buffer.begin()));
}

} // namespace
} // namespace industrial_iot_gateway::transport
