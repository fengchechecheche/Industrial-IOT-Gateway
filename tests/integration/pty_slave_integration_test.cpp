#include "industrial_iot_gateway/protocol/modbus_codec.hpp"
#include "industrial_iot_gateway/protocol/rtu_stream_parser.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <poll.h>
#include <string>
#include <sys/wait.h>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

namespace {

using industrial_iot_gateway::protocol::Adu;
using industrial_iot_gateway::protocol::encode_request;
using industrial_iot_gateway::protocol::ExceptionResponse;
using industrial_iot_gateway::protocol::FunctionCode;
using industrial_iot_gateway::protocol::MonotonicTimeUs;
using industrial_iot_gateway::protocol::ParsedResponseEvent;
using industrial_iot_gateway::protocol::ParserError;
using industrial_iot_gateway::protocol::ParserErrorCategory;
using industrial_iot_gateway::protocol::ParserEvent;
using industrial_iot_gateway::protocol::ParserTiming;
using industrial_iot_gateway::protocol::ReadRequest;
using industrial_iot_gateway::protocol::ReadResponse;
using industrial_iot_gateway::protocol::Request;
using industrial_iot_gateway::protocol::Response;
using industrial_iot_gateway::protocol::RtuStreamParser;
using industrial_iot_gateway::protocol::WriteSingleRegisterRequest;
using industrial_iot_gateway::protocol::WriteSingleRegisterResponse;

constexpr ParserTiming kTiming{860U, 2'006U};
const std::string kExecutable = PTY_SLAVE_EXECUTABLE;
const std::string kRegisterMap = PTY_REGISTER_MAP_PATH;
const std::string kScenarioConfig = PTY_SCENARIO_CONFIG_PATH;

class SimulatorProcess {
public:
  SimulatorProcess() = default;
  ~SimulatorProcess() { cleanup(); }
  SimulatorProcess(const SimulatorProcess &) = delete;
  SimulatorProcess &operator=(const SimulatorProcess &) = delete;

  [[nodiscard]] bool start(const std::string &fault, const std::size_t maximum_requests,
                           const std::optional<std::uint32_t> delay_ms = std::nullopt,
                           const std::optional<std::uint8_t> exception_code = std::nullopt,
                           const std::optional<std::size_t> truncate_bytes = std::nullopt) {
    std::vector<std::string> arguments{kExecutable,
                                       "--register-map",
                                       kRegisterMap,
                                       "--scenario-config",
                                       kScenarioConfig,
                                       "--slave-id",
                                       "1",
                                       "--fault",
                                       fault,
                                       "--max-requests",
                                       std::to_string(maximum_requests)};
    if (delay_ms.has_value()) {
      arguments.emplace_back("--delay-ms");
      arguments.push_back(std::to_string(*delay_ms));
    }
    if (exception_code.has_value()) {
      arguments.emplace_back("--exception-code");
      arguments.push_back(std::to_string(*exception_code));
    }
    if (truncate_bytes.has_value()) {
      arguments.emplace_back("--truncate-bytes");
      arguments.push_back(std::to_string(*truncate_bytes));
    }
    std::vector<char *> argument_pointers;
    argument_pointers.reserve(arguments.size() + 1U);
    for (auto &argument : arguments) {
      argument_pointers.push_back(argument.data());
    }
    argument_pointers.push_back(nullptr);

    int ready_pipe[2]{};
    if (::pipe2(ready_pipe, O_CLOEXEC) != 0) {
      error_ = std::string("pipe2 failed: ") + std::strerror(errno);
      return false;
    }
    child_pid_ = ::fork();
    if (child_pid_ < 0) {
      error_ = std::string("fork failed: ") + std::strerror(errno);
      static_cast<void>(::close(ready_pipe[0]));
      static_cast<void>(::close(ready_pipe[1]));
      return false;
    }
    if (child_pid_ == 0) {
      static_cast<void>(::dup2(ready_pipe[1], STDOUT_FILENO));
      static_cast<void>(::close(ready_pipe[0]));
      static_cast<void>(::close(ready_pipe[1]));
      ::execv(argument_pointers[0], argument_pointers.data());
      _exit(127);
    }

    static_cast<void>(::close(ready_pipe[1]));
    ready_fd_ = ready_pipe[0];
    const auto ready_line = read_ready_line();
    if (!ready_line.has_value()) {
      return false;
    }
    constexpr std::string_view kPathPrefix = "path=";
    const auto path_start = ready_line->find(kPathPrefix);
    if (path_start == std::string::npos) {
      error_ = "pty_slave ready line has no path";
      return false;
    }
    const auto value_start = path_start + kPathPrefix.size();
    const auto value_end = ready_line->find(' ', value_start);
    const auto path = ready_line->substr(value_start, value_end - value_start);
    serial_fd_ = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (serial_fd_ < 0) {
      error_ = std::string("failed to open PTY slave path: ") + std::strerror(errno);
      return false;
    }

    termios settings{};
    if (::tcgetattr(serial_fd_, &settings) != 0) {
      error_ = std::string("tcgetattr failed: ") + std::strerror(errno);
      return false;
    }
    ::cfmakeraw(&settings);
    settings.c_cflag |= CLOCAL | CREAD | CS8;
    settings.c_cflag &= static_cast<tcflag_t>(~(PARENB | PARODD));
    if (::cfsetispeed(&settings, B19200) != 0 || ::cfsetospeed(&settings, B19200) != 0 ||
        ::tcsetattr(serial_fd_, TCSANOW, &settings) != 0) {
      error_ = std::string("failed to configure PTY slave: ") + std::strerror(errno);
      return false;
    }
    return true;
  }

  [[nodiscard]] const std::string &error() const noexcept { return error_; }

  [[nodiscard]] bool send(const Request &request) {
    const auto encoded = encode_request(request);
    const auto *adu = std::get_if<Adu>(&encoded);
    if (adu == nullptr) {
      error_ = "request encoding failed";
      return false;
    }
    std::size_t offset = 0U;
    while (offset < adu->size) {
      const auto written = ::write(serial_fd_, adu->bytes.data() + offset, adu->size - offset);
      if (written > 0) {
        offset += static_cast<std::size_t>(written);
        continue;
      }
      if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        error_ = std::string("request write failed: ") + std::strerror(errno);
        return false;
      }
      pollfd descriptor{serial_fd_, POLLOUT, 0};
      if (::poll(&descriptor, 1U, 100) <= 0) {
        error_ = "request write timed out";
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] std::vector<std::uint8_t> receive(const std::size_t expected_size,
                                                  const std::chrono::milliseconds timeout) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(expected_size);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (bytes.size() < expected_size && std::chrono::steady_clock::now() < deadline) {
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now());
      const auto timeout_ms = static_cast<int>(std::max<std::int64_t>(1, remaining.count()));
      pollfd descriptor{serial_fd_, POLLIN, 0};
      const auto poll_result = ::poll(&descriptor, 1U, timeout_ms);
      if (poll_result < 0 && errno == EINTR) {
        continue;
      }
      if (poll_result <= 0) {
        break;
      }
      if ((descriptor.revents & (POLLIN | POLLHUP)) != 0) {
        std::array<std::uint8_t, 256U> buffer{};
        const auto count = ::read(serial_fd_, buffer.data(), buffer.size());
        if (count > 0) {
          bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + count);
          continue;
        }
      }
      if ((descriptor.revents & (POLLHUP | POLLERR)) != 0) {
        break;
      }
    }
    return bytes;
  }

  [[nodiscard]] bool wait_for_success() { return wait_for_exit(false); }

  [[nodiscard]] bool stop_and_wait() {
    if (child_pid_ > 0) {
      static_cast<void>(::kill(child_pid_, SIGTERM));
    }
    return wait_for_exit(true);
  }

private:
  [[nodiscard]] std::optional<std::string> read_ready_line() {
    std::string line;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
      pollfd descriptor{ready_fd_, POLLIN, 0};
      const auto poll_result = ::poll(&descriptor, 1U, 50);
      if (poll_result < 0 && errno == EINTR) {
        continue;
      }
      if (poll_result < 0) {
        error_ = std::string("ready pipe poll failed: ") + std::strerror(errno);
        return std::nullopt;
      }
      if ((descriptor.revents & POLLIN) != 0) {
        char character{};
        const auto count = ::read(ready_fd_, &character, 1U);
        if (count == 1) {
          if (character == '\n') {
            return line;
          }
          line.push_back(character);
        }
      }
      if ((descriptor.revents & (POLLHUP | POLLERR)) != 0) {
        break;
      }
    }
    error_ = "timed out waiting for pty_slave ready line";
    return std::nullopt;
  }

  [[nodiscard]] bool wait_for_exit(const bool was_stopped) {
    if (child_pid_ <= 0) {
      return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    int status{};
    while (std::chrono::steady_clock::now() < deadline) {
      const auto result = ::waitpid(child_pid_, &status, WNOHANG);
      if (result == child_pid_) {
        child_pid_ = -1;
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
          return true;
        }
        error_ =
            was_stopped ? "pty_slave failed during graceful stop" : "pty_slave exited with failure";
        return false;
      }
      if (result < 0 && errno != EINTR) {
        error_ = std::string("waitpid failed: ") + std::strerror(errno);
        child_pid_ = -1;
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    error_ = "pty_slave did not exit within two seconds";
    return false;
  }

  void cleanup() noexcept {
    if (serial_fd_ >= 0) {
      static_cast<void>(::close(serial_fd_));
      serial_fd_ = -1;
    }
    if (ready_fd_ >= 0) {
      static_cast<void>(::close(ready_fd_));
      ready_fd_ = -1;
    }
    if (child_pid_ > 0) {
      static_cast<void>(::kill(child_pid_, SIGTERM));
      for (int attempt = 0; attempt < 20; ++attempt) {
        int status{};
        const auto result = ::waitpid(child_pid_, &status, WNOHANG);
        if (result == child_pid_ || (result < 0 && errno != EINTR)) {
          child_pid_ = -1;
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    }
    if (child_pid_ > 0) {
      static_cast<void>(::kill(child_pid_, SIGKILL));
      int status{};
      static_cast<void>(::waitpid(child_pid_, &status, 0));
      child_pid_ = -1;
    }
  }

  pid_t child_pid_{-1};
  int ready_fd_{-1};
  int serial_fd_{-1};
  std::string error_{};
};

[[nodiscard]] std::vector<ParserEvent> parse_response_events(const std::vector<std::uint8_t> &bytes,
                                                             const Request &request,
                                                             const bool advance_time) {
  RtuStreamParser parser(kTiming);
  if (!parser.begin_response_stream(42U, request)) {
    return {};
  }
  std::array<ParserEvent, 16U> output{};
  auto parsed = parser.ingest(bytes.data(), bytes.size(), MonotonicTimeUs{1'000U}, output.data(),
                              output.size());
  std::size_t event_count = parsed.events_written;
  if (advance_time) {
    const auto timed = parser.on_time_advanced(
        MonotonicTimeUs{10'000U}, output.data() + event_count, output.size() - event_count);
    event_count += timed.events_written;
  }
  return {output.begin(), output.begin() + static_cast<std::ptrdiff_t>(event_count)};
}

struct ResponseParseResult {
  bool valid{};
  Response response{};
};

[[nodiscard]] ResponseParseResult valid_response(const std::vector<std::uint8_t> &bytes,
                                                 const Request &request) {
  const auto events = parse_response_events(bytes, request, false);
  for (const auto &event : events) {
    if (const auto *parsed = std::get_if<ParsedResponseEvent>(&event.payload)) {
      return ResponseParseResult{true, parsed->response};
    }
  }
  return {};
}

TEST(PtySlaveIntegrationTest, RoundTripsReadWriteAndReadBackThroughRealPty) {
  SimulatorProcess process;
  ASSERT_TRUE(process.start("normal", 4U)) << process.error();

  const Request holding_request = ReadRequest{1U, FunctionCode::read_holding_registers, 0U, 1U};
  ASSERT_TRUE(process.send(holding_request)) << process.error();
  const auto holding =
      valid_response(process.receive(7U, std::chrono::milliseconds(500)), holding_request);
  ASSERT_TRUE(holding.valid);
  const auto *holding_values = std::get_if<ReadResponse>(&holding.response);
  ASSERT_NE(holding_values, nullptr);
  ASSERT_EQ(holding_values->value_count, 1U);
  EXPECT_EQ(holding_values->values[0], 1'000U);

  const Request input_request = ReadRequest{1U, FunctionCode::read_input_registers, 0U, 2U};
  ASSERT_TRUE(process.send(input_request)) << process.error();
  const auto input =
      valid_response(process.receive(9U, std::chrono::milliseconds(500)), input_request);
  ASSERT_TRUE(input.valid);
  const auto *input_values = std::get_if<ReadResponse>(&input.response);
  ASSERT_NE(input_values, nullptr);
  ASSERT_EQ(input_values->value_count, 2U);
  EXPECT_EQ(input_values->values[0], 235U);
  EXPECT_EQ(input_values->values[1], 503U);

  const Request write_request = WriteSingleRegisterRequest{1U, 0U, 1'200U};
  ASSERT_TRUE(process.send(write_request)) << process.error();
  const auto write =
      valid_response(process.receive(8U, std::chrono::milliseconds(500)), write_request);
  ASSERT_TRUE(write.valid);
  const auto *write_echo = std::get_if<WriteSingleRegisterResponse>(&write.response);
  ASSERT_NE(write_echo, nullptr);
  EXPECT_EQ(write_echo->register_value, 1'200U);

  ASSERT_TRUE(process.send(holding_request)) << process.error();
  const auto read_back =
      valid_response(process.receive(7U, std::chrono::milliseconds(500)), holding_request);
  ASSERT_TRUE(read_back.valid);
  const auto *read_back_values = std::get_if<ReadResponse>(&read_back.response);
  ASSERT_NE(read_back_values, nullptr);
  EXPECT_EQ(read_back_values->values[0], 1'200U);
  EXPECT_TRUE(process.wait_for_success()) << process.error();
}

TEST(PtySlaveIntegrationTest, ProducesConfiguredExceptionResponse) {
  SimulatorProcess process;
  ASSERT_TRUE(process.start("exception", 1U, std::nullopt, 4U)) << process.error();
  const Request request = ReadRequest{1U, FunctionCode::read_holding_registers, 0U, 1U};
  ASSERT_TRUE(process.send(request)) << process.error();
  const auto response =
      valid_response(process.receive(5U, std::chrono::milliseconds(500)), request);
  ASSERT_TRUE(response.valid);
  const auto *exception = std::get_if<ExceptionResponse>(&response.response);
  ASSERT_NE(exception, nullptr);
  EXPECT_EQ(exception->exception_code, 4U);
  EXPECT_TRUE(process.wait_for_success()) << process.error();
}

TEST(PtySlaveIntegrationTest, DelaysResponseByConfiguredInterval) {
  SimulatorProcess process;
  ASSERT_TRUE(process.start("delay", 1U, 40U)) << process.error();
  const Request request = ReadRequest{1U, FunctionCode::read_holding_registers, 0U, 1U};
  const auto started = std::chrono::steady_clock::now();
  ASSERT_TRUE(process.send(request)) << process.error();
  const auto bytes = process.receive(7U, std::chrono::milliseconds(500));
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);
  EXPECT_GE(elapsed.count(), 30);
  EXPECT_TRUE(valid_response(bytes, request).valid);
  EXPECT_TRUE(process.wait_for_success()) << process.error();
}

TEST(PtySlaveIntegrationTest, RemainsSilentUntilMasterTimeout) {
  SimulatorProcess process;
  ASSERT_TRUE(process.start("silent", 2U)) << process.error();
  const Request request = ReadRequest{1U, FunctionCode::read_holding_registers, 0U, 1U};
  ASSERT_TRUE(process.send(request)) << process.error();
  const auto started = std::chrono::steady_clock::now();
  const auto bytes = process.receive(1U, std::chrono::milliseconds(120));
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);
  EXPECT_TRUE(bytes.empty());
  EXPECT_GE(elapsed.count(), 100);
  EXPECT_TRUE(process.stop_and_wait()) << process.error();
}

TEST(PtySlaveIntegrationTest, CorruptsCrcAndResponseParserRejectsFrame) {
  SimulatorProcess process;
  ASSERT_TRUE(process.start("bad-crc", 1U)) << process.error();
  const Request request = ReadRequest{1U, FunctionCode::read_holding_registers, 0U, 1U};
  ASSERT_TRUE(process.send(request)) << process.error();
  const auto events =
      parse_response_events(process.receive(7U, std::chrono::milliseconds(500)), request, false);
  const auto crc_error = std::find_if(events.begin(), events.end(), [](const ParserEvent &event) {
    const auto *error = std::get_if<ParserError>(&event.payload);
    return error != nullptr && error->category == ParserErrorCategory::crc_mismatch;
  });
  EXPECT_NE(crc_error, events.end());
  EXPECT_TRUE(process.wait_for_success()) << process.error();
}

TEST(PtySlaveIntegrationTest, TruncatesResponseAndParserReportsShortFrame) {
  SimulatorProcess process;
  ASSERT_TRUE(process.start("truncated", 1U, std::nullopt, std::nullopt, 2U)) << process.error();
  const Request request = ReadRequest{1U, FunctionCode::read_holding_registers, 0U, 1U};
  ASSERT_TRUE(process.send(request)) << process.error();
  const auto events =
      parse_response_events(process.receive(7U, std::chrono::milliseconds(500)), request, true);
  const auto short_frame = std::find_if(events.begin(), events.end(), [](const ParserEvent &event) {
    const auto *error = std::get_if<ParserError>(&event.payload);
    return error != nullptr && error->category == ParserErrorCategory::frame_too_short;
  });
  EXPECT_NE(short_frame, events.end());
  EXPECT_TRUE(process.wait_for_success()) << process.error();
}

} // namespace
