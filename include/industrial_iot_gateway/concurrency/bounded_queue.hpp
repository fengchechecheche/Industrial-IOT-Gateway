#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <utility>

namespace industrial_iot_gateway::concurrency {

enum class QueueErrorCategory {
  none,
  invalid_capacity,
  invalid_high_watermark,
  invalid_fairness,
};

struct QueueConfig {
  std::size_t capacity{};
  std::size_t high_watermark{};
};

enum class QueuePushStatus {
  accepted,
  coalesced,
  full,
  closed,
  timed_out,
};

enum class QueuePopStatus {
  item,
  empty,
  closed,
  timed_out,
};

struct QueueStatistics {
  std::size_t capacity{};
  std::size_t high_watermark{};
  std::size_t current_depth{};
  std::size_t maximum_depth{};
  std::uint64_t accepted{};
  std::uint64_t popped{};
  std::uint64_t high_watermark_crossings{};
  std::uint64_t full{};
  std::uint64_t coalesced{};
  std::uint64_t rejected_closed{};
  std::uint64_t timed_out{};
};

template <typename T> struct QueuePopResult {
  QueuePopStatus status{QueuePopStatus::empty};
  std::optional<T> value{};
};

template <typename T> class BoundedQueue {
public:
  explicit BoundedQueue(QueueConfig config) : config_(config) {
    statistics_.capacity = config.capacity;
    statistics_.high_watermark = config.high_watermark;
    if (config.capacity == 0U) {
      validation_error_ = QueueErrorCategory::invalid_capacity;
    } else if (config.high_watermark == 0U || config.high_watermark >= config.capacity) {
      validation_error_ = QueueErrorCategory::invalid_high_watermark;
    }
  }

  ~BoundedQueue() = default;
  BoundedQueue(const BoundedQueue &) = delete;
  BoundedQueue &operator=(const BoundedQueue &) = delete;
  BoundedQueue(BoundedQueue &&) = delete;
  BoundedQueue &operator=(BoundedQueue &&) = delete;

  [[nodiscard]] bool valid() const noexcept {
    return validation_error_ == QueueErrorCategory::none;
  }

  [[nodiscard]] QueueErrorCategory validation_error() const noexcept { return validation_error_; }

  [[nodiscard]] QueuePushStatus try_push(T value) {
    std::unique_lock<std::mutex> lock(mutex_);
    return push_locked(std::move(value));
  }

  [[nodiscard]] QueuePushStatus push_until(T value,
                                           std::chrono::steady_clock::time_point deadline) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!valid()) {
      ++statistics_.full;
      return QueuePushStatus::full;
    }
    if (!not_full_.wait_until(lock, deadline,
                              [this] { return closed_ || values_.size() < config_.capacity; })) {
      ++statistics_.timed_out;
      return QueuePushStatus::timed_out;
    }
    return push_locked(std::move(value));
  }

  [[nodiscard]] QueuePushStatus try_push_or_replace(T value,
                                                    const std::function<bool(const T &)> &matcher) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!valid()) {
      ++statistics_.full;
      return QueuePushStatus::full;
    }
    if (closed_) {
      ++statistics_.rejected_closed;
      return QueuePushStatus::closed;
    }
    const auto existing = std::find_if(values_.begin(), values_.end(), matcher);
    if (existing != values_.end()) {
      *existing = std::move(value);
      ++statistics_.coalesced;
      return QueuePushStatus::coalesced;
    }
    return push_locked(std::move(value));
  }

  [[nodiscard]] QueuePopResult<T> try_pop() {
    std::unique_lock<std::mutex> lock(mutex_);
    return pop_locked();
  }

  [[nodiscard]] QueuePopResult<T> wait_pop_until(std::chrono::steady_clock::time_point deadline) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!not_empty_.wait_until(lock, deadline, [this] { return closed_ || !values_.empty(); })) {
      ++statistics_.timed_out;
      return {QueuePopStatus::timed_out, std::nullopt};
    }
    return pop_locked();
  }

  void close() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      closed_ = true;
    }
    not_empty_.notify_all();
    not_full_.notify_all();
  }

  [[nodiscard]] bool closed() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
  }

  [[nodiscard]] QueueStatistics statistics() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return statistics_;
  }

private:
  [[nodiscard]] QueuePushStatus push_locked(T value) {
    if (!valid()) {
      ++statistics_.full;
      return QueuePushStatus::full;
    }
    if (closed_) {
      ++statistics_.rejected_closed;
      return QueuePushStatus::closed;
    }
    if (values_.size() >= config_.capacity) {
      ++statistics_.full;
      return QueuePushStatus::full;
    }
    values_.push_back(std::move(value));
    ++statistics_.accepted;
    statistics_.current_depth = values_.size();
    statistics_.maximum_depth = std::max(statistics_.maximum_depth, values_.size());
    if (!above_high_watermark_ && values_.size() >= config_.high_watermark) {
      above_high_watermark_ = true;
      ++statistics_.high_watermark_crossings;
    }
    not_empty_.notify_one();
    return QueuePushStatus::accepted;
  }

  [[nodiscard]] QueuePopResult<T> pop_locked() {
    if (values_.empty()) {
      return {closed_ ? QueuePopStatus::closed : QueuePopStatus::empty, std::nullopt};
    }
    T value = std::move(values_.front());
    values_.pop_front();
    ++statistics_.popped;
    statistics_.current_depth = values_.size();
    if (values_.size() < config_.high_watermark) {
      above_high_watermark_ = false;
    }
    not_full_.notify_one();
    return {QueuePopStatus::item, std::move(value)};
  }

  QueueConfig config_{};
  QueueErrorCategory validation_error_{QueueErrorCategory::none};
  mutable std::mutex mutex_{};
  std::condition_variable not_empty_{};
  std::condition_variable not_full_{};
  std::deque<T> values_{};
  QueueStatistics statistics_{};
  bool closed_{};
  bool above_high_watermark_{};
};

} // namespace industrial_iot_gateway::concurrency
