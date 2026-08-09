#include "industrial_iot_gateway/transport/serial_port.hpp"

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <gtest/gtest.h>
#include <poll.h>
#include <termios.h>

#include "transport/serial_port_detail.hpp"

namespace industrial_iot_gateway::transport {

struct SerialPortTestAccess {
  [[nodiscard]] static SerialPort create(std::unique_ptr<detail::PosixSerialApi> api) {
    return SerialPort(std::move(api));
  }
};

namespace {

struct IoStep {
  ssize_t result{};
  int error{};
};

struct PollStep {
  int result{};
  int error{};
  short returned_events{};
};

class FakePosixSerialApi final : public detail::PosixSerialApi {
public:
  int open_device(const char *, const int flags) noexcept override {
    open_flags = flags;
    ++open_calls;
    const auto index = static_cast<std::size_t>(open_calls - 1);
    if (index < open_results.size()) {
      errno = index < open_errors.size() ? open_errors[index] : 0;
      return open_results[index];
    }
    return 40 + open_calls;
  }

  int close_fd(const int) noexcept override {
    ++close_calls;
    if (external_close_calls != nullptr) {
      ++(*external_close_calls);
    }
    return 0;
  }

  int get_attributes(const int, termios *settings) noexcept override {
    ++get_attributes_calls;
    if (get_attributes_result != 0) {
      errno = get_attributes_error;
      return get_attributes_result;
    }
    *settings = initial_settings;
    return 0;
  }

  int set_attributes(const int, const int, const termios *settings) noexcept override {
    ++set_attributes_calls;
    applied_settings = *settings;
    if (set_attributes_result != 0) {
      errno = set_attributes_error;
    }
    return set_attributes_result;
  }

  int flush(const int, const int) noexcept override {
    ++flush_calls;
    if (flush_result != 0) {
      errno = flush_error;
    }
    return flush_result;
  }

  ssize_t read_fd(const int, void *buffer, const std::size_t size) noexcept override {
    ++read_calls;
    const auto step = next_io_step(read_steps, read_index);
    errno = step.error;
    if (step.result > 0 && buffer != nullptr) {
      const auto count = static_cast<std::size_t>(step.result);
      std::memset(buffer, 0xA5, count < size ? count : size);
    }
    return step.result;
  }

  ssize_t write_fd(const int, const void *, const std::size_t) noexcept override {
    ++write_calls;
    const auto step = next_io_step(write_steps, write_index);
    errno = step.error;
    return step.result;
  }

  int poll_fds(pollfd *descriptors, const nfds_t count,
               const std::chrono::milliseconds timeout) noexcept override {
    ++poll_calls;
    observed_timeouts.push_back(static_cast<int>(timeout.count()));
    PollStep step{};
    if (poll_index < poll_steps.size()) {
      step = poll_steps[poll_index++];
    }
    errno = step.error;
    if (count > 0U && descriptors != nullptr) {
      descriptors[0].revents = step.returned_events;
    }
    return step.result;
  }

  static IoStep next_io_step(const std::vector<IoStep> &steps, std::size_t &index) noexcept {
    if (index < steps.size()) {
      return steps[index++];
    }
    return IoStep{-1, EAGAIN};
  }

  int open_calls{};
  int close_calls{};
  int get_attributes_calls{};
  int set_attributes_calls{};
  int flush_calls{};
  int read_calls{};
  int write_calls{};
  int poll_calls{};
  int open_flags{};
  int *external_close_calls{};
  int get_attributes_result{};
  int get_attributes_error{EIO};
  int set_attributes_result{};
  int set_attributes_error{EIO};
  int flush_result{};
  int flush_error{EIO};
  termios initial_settings{};
  termios applied_settings{};
  std::vector<int> open_results{};
  std::vector<int> open_errors{};
  std::vector<IoStep> read_steps{};
  std::vector<IoStep> write_steps{};
  std::vector<PollStep> poll_steps{};
  std::vector<int> observed_timeouts{};
  std::size_t read_index{};
  std::size_t write_index{};
  std::size_t poll_index{};
};

struct PortFixture {
  PortFixture() {
    auto owned_api = std::make_unique<FakePosixSerialApi>();
    api = owned_api.get();
    port = SerialPortTestAccess::create(std::move(owned_api));
  }

  FakePosixSerialApi *api{};
  SerialPort port{};
};

[[nodiscard]] SerialConfig valid_config() {
  SerialConfig config{};
  config.device_path = "/dev/tty-test";
  return config;
}

TEST(SerialConfigTest, UsesFrozen19200EightEvenOneDefaults) {
  const SerialConfig config{};

  EXPECT_EQ(config.baud_rate, BaudRate::baud_19200);
  EXPECT_EQ(config.data_bits, 8U);
  EXPECT_EQ(config.parity, Parity::even);
  EXPECT_EQ(config.stop_bits, StopBits::one);
}

TEST(SerialPortTest, AppliesRawNonblockingCloseOnExecConfiguration) {
  PortFixture fixture{};
  SerialError error{};

  ASSERT_TRUE(fixture.port.open(valid_config(), error));
  EXPECT_EQ(error.category, SerialErrorCategory::none);
  EXPECT_NE(fixture.api->open_flags & O_NONBLOCK, 0);
  EXPECT_NE(fixture.api->open_flags & O_NOCTTY, 0);
  EXPECT_NE(fixture.api->open_flags & O_CLOEXEC, 0);
  EXPECT_EQ(fixture.api->applied_settings.c_cflag & CSIZE, CS8);
  EXPECT_NE(fixture.api->applied_settings.c_cflag & CLOCAL, 0U);
  EXPECT_NE(fixture.api->applied_settings.c_cflag & CREAD, 0U);
  EXPECT_NE(fixture.api->applied_settings.c_cflag & PARENB, 0U);
  EXPECT_EQ(fixture.api->applied_settings.c_cflag & PARODD, 0U);
  EXPECT_EQ(fixture.api->applied_settings.c_cflag & CSTOPB, 0U);
  EXPECT_EQ(fixture.api->applied_settings.c_cc[VMIN], 0U);
  EXPECT_EQ(fixture.api->applied_settings.c_cc[VTIME], 0U);
  EXPECT_EQ(::cfgetispeed(&fixture.api->applied_settings), B19200);
  EXPECT_EQ(::cfgetospeed(&fixture.api->applied_settings), B19200);
}

TEST(SerialPortTest, MapsEveryFrozenBaudRate) {
  const std::vector<std::pair<BaudRate, speed_t>> cases{{BaudRate::baud_9600, B9600},
                                                        {BaudRate::baud_19200, B19200},
                                                        {BaudRate::baud_38400, B38400},
                                                        {BaudRate::baud_115200, B115200}};

  for (const auto &[baud_rate, expected_speed] : cases) {
    PortFixture fixture{};
    auto config = valid_config();
    config.baud_rate = baud_rate;
    SerialError error{};
    ASSERT_TRUE(fixture.port.open(config, error));
    EXPECT_EQ(::cfgetispeed(&fixture.api->applied_settings), expected_speed);
    EXPECT_EQ(::cfgetospeed(&fixture.api->applied_settings), expected_speed);
  }
}

TEST(SerialPortTest, RejectsInvalidConfigurationBeforeOpeningDevice) {
  PortFixture fixture{};
  SerialError error{};
  auto config = valid_config();
  config.device_path.clear();
  EXPECT_FALSE(fixture.port.open(config, error));
  EXPECT_EQ(error.category, SerialErrorCategory::invalid_configuration);

  config = valid_config();
  config.data_bits = 7U;
  EXPECT_FALSE(fixture.port.open(config, error));
  EXPECT_EQ(error.category, SerialErrorCategory::invalid_configuration);

  config = valid_config();
  config.baud_rate = static_cast<BaudRate>(12345U);
  EXPECT_FALSE(fixture.port.open(config, error));
  EXPECT_EQ(error.category, SerialErrorCategory::invalid_configuration);
  EXPECT_EQ(fixture.api->open_calls, 0);
}

TEST(SerialPortTest, ReportsOpenFailureWithoutRetrying) {
  PortFixture fixture{};
  fixture.api->open_results = {-1};
  fixture.api->open_errors = {ENOENT};
  SerialError error{};

  EXPECT_FALSE(fixture.port.open(valid_config(), error));
  EXPECT_EQ(error.category, SerialErrorCategory::open_failed);
  EXPECT_EQ(error.system_error, ENOENT);
  EXPECT_EQ(fixture.api->open_calls, 1);
  EXPECT_EQ(fixture.api->close_calls, 0);
}

TEST(SerialPortTest, ClosesOnceWhenConfigurationFails) {
  PortFixture fixture{};
  fixture.api->set_attributes_result = -1;
  fixture.api->set_attributes_error = EINVAL;
  SerialError error{};

  EXPECT_FALSE(fixture.port.open(valid_config(), error));
  EXPECT_EQ(error.category, SerialErrorCategory::configure_failed);
  EXPECT_EQ(error.system_error, EINVAL);
  EXPECT_EQ(fixture.api->close_calls, 1);
  EXPECT_FALSE(fixture.port.is_open());
}

TEST(SerialPortTest, MoveTransfersTheOnlyCloseOwner) {
  auto owned_api = std::make_unique<FakePosixSerialApi>();
  int close_calls{};
  owned_api->external_close_calls = &close_calls;
  {
    auto original = SerialPortTestAccess::create(std::move(owned_api));
    SerialError error{};
    ASSERT_TRUE(original.open(valid_config(), error));
    SerialPort moved{std::move(original)};
    EXPECT_TRUE(moved.is_open());
  }
  EXPECT_EQ(close_calls, 1);
}

TEST(SerialPortTest, RetriesInterruptedReadAndReturnsShortRead) {
  PortFixture fixture{};
  fixture.api->read_steps = {{-1, EINTR}, {3, 0}};
  SerialError error{};
  ASSERT_TRUE(fixture.port.open(valid_config(), error));
  std::uint8_t buffer[8]{};

  const auto result = fixture.port.read_some(buffer, sizeof(buffer));

  EXPECT_EQ(result.status, IoStatus::completed);
  EXPECT_EQ(result.bytes_transferred, 3U);
  EXPECT_EQ(fixture.api->read_calls, 2);
}

TEST(SerialPortTest, ReportsReadWouldBlockWithoutSpinning) {
  PortFixture fixture{};
  fixture.api->read_steps = {{-1, EAGAIN}};
  SerialError error{};
  ASSERT_TRUE(fixture.port.open(valid_config(), error));
  std::uint8_t buffer[8]{};

  const auto result = fixture.port.read_some(buffer, sizeof(buffer));

  EXPECT_EQ(result.status, IoStatus::would_block);
  EXPECT_EQ(fixture.api->read_calls, 1);
}

TEST(SerialPortTest, RetriesInterruptedWriteAndReturnsShortWrite) {
  PortFixture fixture{};
  fixture.api->write_steps = {{-1, EINTR}, {2, 0}};
  SerialError error{};
  ASSERT_TRUE(fixture.port.open(valid_config(), error));
  const std::uint8_t data[4]{1U, 2U, 3U, 4U};

  const auto result = fixture.port.write_some(data, sizeof(data));

  EXPECT_EQ(result.status, IoStatus::completed);
  EXPECT_EQ(result.bytes_transferred, 2U);
  EXPECT_EQ(fixture.api->write_calls, 2);
}

TEST(SerialPortTest, ReportsWriteWouldBlockWithoutSpinning) {
  PortFixture fixture{};
  fixture.api->write_steps = {{-1, EWOULDBLOCK}};
  SerialError error{};
  ASSERT_TRUE(fixture.port.open(valid_config(), error));
  const std::uint8_t data[1]{1U};

  const auto result = fixture.port.write_some(data, sizeof(data));

  EXPECT_EQ(result.status, IoStatus::would_block);
  EXPECT_EQ(fixture.api->write_calls, 1);
}

TEST(SerialPortTest, RetriesInterruptedPollWithinOriginalDeadline) {
  PortFixture fixture{};
  fixture.api->poll_steps = {{-1, EINTR, 0}, {1, 0, POLLIN}};
  SerialError error{};
  ASSERT_TRUE(fixture.port.open(valid_config(), error));

  const auto result = fixture.port.wait(WaitInterest{true, false},
                                        std::chrono::steady_clock::now() + std::chrono::seconds(1));

  EXPECT_EQ(result.status, WaitStatus::ready);
  EXPECT_TRUE(result.readable);
  EXPECT_FALSE(result.writable);
  ASSERT_EQ(fixture.api->observed_timeouts.size(), 2U);
  EXPECT_LE(fixture.api->observed_timeouts[1], fixture.api->observed_timeouts[0]);
}

TEST(SerialPortTest, ReportsFinitePollTimeout) {
  PortFixture fixture{};
  fixture.api->poll_steps = {{0, 0, 0}};
  SerialError error{};
  ASSERT_TRUE(fixture.port.open(valid_config(), error));

  const auto result =
      fixture.port.wait(WaitInterest{true, false}, std::chrono::steady_clock::now());

  EXPECT_EQ(result.status, WaitStatus::timeout);
  EXPECT_EQ(fixture.api->poll_calls, 1);
}

TEST(SerialPortTest, DisconnectClosesOnceAndAllowsExplicitReopen) {
  PortFixture fixture{};
  fixture.api->open_results = {41, 42};
  fixture.api->poll_steps = {{1, 0, POLLHUP}};
  SerialError error{};
  ASSERT_TRUE(fixture.port.open(valid_config(), error));

  const auto wait_result = fixture.port.wait(
      WaitInterest{true, false}, std::chrono::steady_clock::now() + std::chrono::seconds(1));

  EXPECT_EQ(wait_result.status, WaitStatus::disconnected);
  EXPECT_EQ(wait_result.error.category, SerialErrorCategory::disconnected);
  EXPECT_EQ(fixture.api->close_calls, 1);
  EXPECT_FALSE(fixture.port.is_open());

  auto second_config = valid_config();
  second_config.device_path = "/dev/tty-test-2";
  EXPECT_TRUE(fixture.port.reopen(second_config, error));
  EXPECT_TRUE(fixture.port.is_open());
  EXPECT_EQ(fixture.api->open_calls, 2);
}

} // namespace
} // namespace industrial_iot_gateway::transport
