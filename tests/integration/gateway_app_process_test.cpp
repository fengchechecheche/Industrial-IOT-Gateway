#include "industrial_iot_gateway/simulation/pty_bus_harness.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include <gtest/gtest.h>

namespace {

using industrial_iot_gateway::test_support::PtyBusHarness;
using namespace std::chrono_literals;

const std::string kGatewayApp = GATEWAY_APP_EXECUTABLE;
const std::string kRegisterMap = GATEWAY_REGISTER_MAP_PATH;
const std::string kScenarioMap = GATEWAY_PTY_SCENARIO_PATH;

class GatewayProcess {
public:
  GatewayProcess() = default;
  ~GatewayProcess() { cleanup(); }
  GatewayProcess(const GatewayProcess &) = delete;
  GatewayProcess &operator=(const GatewayProcess &) = delete;

  [[nodiscard]] bool start(const std::string &serial_path) {
    int output_pipe[2]{};
    if (::pipe2(output_pipe, O_CLOEXEC) != 0) {
      error_ = std::strerror(errno);
      return false;
    }
    child_ = ::fork();
    if (child_ < 0) {
      error_ = std::strerror(errno);
      static_cast<void>(::close(output_pipe[0]));
      static_cast<void>(::close(output_pipe[1]));
      return false;
    }
    if (child_ == 0) {
      static_cast<void>(::close(output_pipe[0]));
      static_cast<void>(::dup2(output_pipe[1], STDOUT_FILENO));
      static_cast<void>(::dup2(output_pipe[1], STDERR_FILENO));
      static_cast<void>(::close(output_pipe[1]));
      ::execl(kGatewayApp.c_str(), kGatewayApp.c_str(), "--serial-device", serial_path.c_str(),
              "--register-map", kRegisterMap.c_str(), static_cast<char *>(nullptr));
      ::_exit(127);
    }
    static_cast<void>(::close(output_pipe[1]));
    output_fd_ = output_pipe[0];
    const auto flags = ::fcntl(output_fd_, F_GETFL, 0);
    if (flags >= 0) {
      static_cast<void>(::fcntl(output_fd_, F_SETFL, flags | O_NONBLOCK));
    }
    return true;
  }

  [[nodiscard]] bool wait_for_output(const std::string &marker,
                                     const std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      read_available();
      if (output_.find(marker) != std::string::npos) {
        return true;
      }
      pollfd descriptor{output_fd_, POLLIN, 0};
      static_cast<void>(::poll(&descriptor, 1U, 20));
    }
    read_available();
    return output_.find(marker) != std::string::npos;
  }

  [[nodiscard]] bool terminate_and_wait() {
    if (child_ <= 0 || ::kill(child_, SIGTERM) != 0) {
      return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
      const auto result = ::waitpid(child_, &status_, WNOHANG);
      if (result == child_) {
        child_ = -1;
        read_available();
        return true;
      }
      if (result < 0) {
        return false;
      }
      read_available();
      std::this_thread::sleep_for(10ms);
    }
    return false;
  }

  [[nodiscard]] const std::string &output() const noexcept { return output_; }
  [[nodiscard]] const std::string &error() const noexcept { return error_; }
  [[nodiscard]] int status() const noexcept { return status_; }

private:
  void read_available() {
    if (output_fd_ < 0) {
      return;
    }
    std::array<char, 4096U> buffer{};
    while (true) {
      const auto count = ::read(output_fd_, buffer.data(), buffer.size());
      if (count > 0) {
        output_.append(buffer.data(), static_cast<std::size_t>(count));
        continue;
      }
      if (count < 0 && errno == EINTR) {
        continue;
      }
      break;
    }
  }

  void cleanup() noexcept {
    if (child_ > 0) {
      static_cast<void>(::kill(child_, SIGKILL));
      static_cast<void>(::waitpid(child_, &status_, 0));
      child_ = -1;
    }
    if (output_fd_ >= 0) {
      static_cast<void>(::close(output_fd_));
      output_fd_ = -1;
    }
  }

  pid_t child_{-1};
  int output_fd_{-1};
  int status_{};
  std::string output_{};
  std::string error_{};
};

TEST(GatewayAppProcessTest, SigtermStopsLiveThreeSlaveRuntimeAndPrintsSummary) {
  PtyBusHarness bus(kRegisterMap, kScenarioMap);
  ASSERT_TRUE(bus.start()) << bus.last_error();
  GatewayProcess process;
  ASSERT_TRUE(process.start(bus.gateway_path())) << process.error();
  ASSERT_TRUE(process.wait_for_output("gateway_ready", 2s)) << process.output();
  std::this_thread::sleep_for(300ms);
  ASSERT_TRUE(process.terminate_and_wait()) << process.output();
  ASSERT_TRUE(WIFEXITED(process.status()));
  EXPECT_EQ(WEXITSTATUS(process.status()), 0) << process.output();
  EXPECT_NE(process.output().find("request_completed"), std::string::npos);
  EXPECT_NE(process.output().find("telemetry"), std::string::npos);
  EXPECT_NE(process.output().find("gateway_summary"), std::string::npos);
  EXPECT_NE(process.output().find("\"stopped\":true"), std::string::npos);
}

} // namespace
