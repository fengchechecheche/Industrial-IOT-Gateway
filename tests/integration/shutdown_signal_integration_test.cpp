#include "industrial_iot_gateway/concurrency/bounded_queue.hpp"
#include "industrial_iot_gateway/lifecycle/shutdown_coordinator.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <thread>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <gtest/gtest.h>

namespace industrial_iot_gateway::lifecycle {
namespace {

using namespace std::chrono_literals;

class FakeSerialOwner {
public:
  explicit FakeSerialOwner(std::atomic<std::uint32_t> &close_count) noexcept
      : close_count_(close_count) {}
  ~FakeSerialOwner() { ++close_count_; }

private:
  std::atomic<std::uint32_t> &close_count_;
};

[[nodiscard]] int run_shutdown_child(int ready_fd) {
  if (!SynchronousSignalWaiter::block_shutdown_signals_for_current_thread()) {
    return 10;
  }

  concurrency::BoundedQueue<int> queue({4U, 3U});
  ShutdownCoordinator coordinator{};
  std::atomic<std::uint32_t> close_count{};
  std::atomic<bool> consumer_closed{};

  std::thread serial_owner([&] {
    FakeSerialOwner owner(close_count);
    const auto result = queue.wait_pop_until(std::chrono::steady_clock::now() + 5s);
    consumer_closed = result.status == concurrency::QueuePopStatus::closed;
  });
  std::thread signal_waiter([&] {
    const auto reason = SynchronousSignalWaiter::wait();
    static_cast<void>(coordinator.request_stop(reason, LifecycleClock::now()));
    queue.close();
  });

  const char ready = 'R';
  if (::write(ready_fd, &ready, 1U) != 1) {
    return 11;
  }
  static_cast<void>(::close(ready_fd));
  signal_waiter.join();
  serial_owner.join();

  const auto snapshot = coordinator.snapshot();
  if (snapshot.reason != ShutdownReason::sigterm || !consumer_closed || close_count != 1U) {
    return 12;
  }
  return 0;
}

TEST(ShutdownSignalIntegrationTest, SigtermWakesQueueJoinsThreadsAndClosesOwnerOnce) {
  int ready_pipe[2]{};
  ASSERT_EQ(::pipe(ready_pipe), 0);
  const auto child = ::fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    static_cast<void>(::close(ready_pipe[0]));
    const int result = run_shutdown_child(ready_pipe[1]);
    ::_exit(result);
  }

  static_cast<void>(::close(ready_pipe[1]));
  char ready = 0;
  ASSERT_EQ(::read(ready_pipe[0], &ready, 1U), 1);
  static_cast<void>(::close(ready_pipe[0]));
  ASSERT_EQ(ready, 'R');
  ASSERT_EQ(::kill(child, SIGTERM), 0);

  int status = 0;
  bool exited = false;
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto result = ::waitpid(child, &status, WNOHANG);
    if (result == child) {
      exited = true;
      break;
    }
    ASSERT_NE(result, -1);
    std::this_thread::sleep_for(10ms);
  }
  if (!exited) {
    static_cast<void>(::kill(child, SIGKILL));
    static_cast<void>(::waitpid(child, &status, 0));
  }
  ASSERT_TRUE(exited);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
}

} // namespace
} // namespace industrial_iot_gateway::lifecycle
