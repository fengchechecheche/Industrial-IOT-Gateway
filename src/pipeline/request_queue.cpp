#include "industrial_iot_gateway/pipeline/request_queue.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <utility>

namespace industrial_iot_gateway::pipeline {
namespace {

[[nodiscard]] bool is_explicit(scheduler::RequestKind kind) noexcept {
  return kind == scheduler::RequestKind::explicit_read ||
         kind == scheduler::RequestKind::explicit_write;
}

[[nodiscard]] bool eligible(const scheduler::ScheduledRequest &request,
                            scheduler::SchedulerTimePoint now) noexcept {
  return request.due_at <= now && request.attempt_eligible_at <= now;
}

} // namespace

struct RequestQueue::Impl {
  explicit Impl(RequestQueueConfig supplied) : config(supplied) {
    statistics.capacity = config.queue.capacity;
    statistics.high_watermark = config.queue.high_watermark;
    if (config.queue.capacity == 0U) {
      error = concurrency::QueueErrorCategory::invalid_capacity;
    } else if (config.queue.high_watermark == 0U ||
               config.queue.high_watermark >= config.queue.capacity) {
      error = concurrency::QueueErrorCategory::invalid_high_watermark;
    } else if (config.max_consecutive_explicit_requests == 0U) {
      error = concurrency::QueueErrorCategory::invalid_fairness;
    }
  }

  [[nodiscard]] std::size_t depth() const noexcept {
    return explicit_requests.size() + poll_requests.size();
  }

  [[nodiscard]] concurrency::QueuePopResult<scheduler::ScheduledRequest>
  pop_locked(scheduler::SchedulerTimePoint now) {
    const auto explicit_candidate = std::find_if(
        explicit_requests.begin(), explicit_requests.end(),
        [now](const scheduler::ScheduledRequest &request) { return eligible(request, now); });
    const auto poll_candidate = std::find_if(
        poll_requests.begin(), poll_requests.end(),
        [now](const scheduler::ScheduledRequest &request) { return eligible(request, now); });

    const bool explicit_due = explicit_candidate != explicit_requests.end();
    const bool poll_due = poll_candidate != poll_requests.end();
    if (!explicit_due && !poll_due) {
      return {closed && depth() == 0U ? concurrency::QueuePopStatus::closed
                                      : concurrency::QueuePopStatus::empty,
              std::nullopt};
    }

    const bool yield_to_poll =
        poll_due && consecutive_explicit >= config.max_consecutive_explicit_requests;
    scheduler::ScheduledRequest selected{};
    if (explicit_due && !yield_to_poll) {
      selected = *explicit_candidate;
      explicit_requests.erase(explicit_candidate);
      ++consecutive_explicit;
    } else {
      selected = *poll_candidate;
      poll_requests.erase(poll_candidate);
      consecutive_explicit = 0U;
    }
    record_pop();
    return {concurrency::QueuePopStatus::item, selected};
  }

  [[nodiscard]] scheduler::SchedulerTimePoint
  next_eligibility(scheduler::SchedulerTimePoint fallback) const noexcept {
    auto next = fallback;
    const auto consider = [&next](const scheduler::ScheduledRequest &request) {
      next = std::min(next, std::max(request.due_at, request.attempt_eligible_at));
    };
    for (const auto &request : explicit_requests) {
      consider(request);
    }
    for (const auto &request : poll_requests) {
      consider(request);
    }
    return next;
  }

  void record_accepted() noexcept {
    ++statistics.accepted;
    statistics.current_depth = depth();
    statistics.maximum_depth = std::max(statistics.maximum_depth, statistics.current_depth);
    if (!above_high_watermark && statistics.current_depth >= config.queue.high_watermark) {
      above_high_watermark = true;
      ++statistics.high_watermark_crossings;
    }
  }

  void record_pop() noexcept {
    ++statistics.popped;
    statistics.current_depth = depth();
    if (statistics.current_depth < config.queue.high_watermark) {
      above_high_watermark = false;
    }
  }

  RequestQueueConfig config{};
  concurrency::QueueErrorCategory error{concurrency::QueueErrorCategory::none};
  mutable std::mutex mutex{};
  std::condition_variable changed{};
  std::deque<scheduler::ScheduledRequest> explicit_requests{};
  std::deque<scheduler::ScheduledRequest> poll_requests{};
  concurrency::QueueStatistics statistics{};
  std::uint32_t consecutive_explicit{};
  bool closed{};
  bool above_high_watermark{};
};

RequestQueue::RequestQueue(RequestQueueConfig config) : impl_(std::make_unique<Impl>(config)) {}

RequestQueue::~RequestQueue() = default;

bool RequestQueue::valid() const noexcept {
  return impl_->error == concurrency::QueueErrorCategory::none;
}

concurrency::QueueErrorCategory RequestQueue::validation_error() const noexcept {
  return impl_->error;
}

concurrency::QueuePushStatus RequestQueue::push(scheduler::ScheduledRequest request) {
  std::unique_lock<std::mutex> lock(impl_->mutex);
  if (!valid()) {
    ++impl_->statistics.full;
    return concurrency::QueuePushStatus::full;
  }
  if (impl_->closed) {
    ++impl_->statistics.rejected_closed;
    return concurrency::QueuePushStatus::closed;
  }

  if (request.request_kind == scheduler::RequestKind::poll_read) {
    const auto duplicate = std::find_if(impl_->poll_requests.begin(), impl_->poll_requests.end(),
                                        [&request](const scheduler::ScheduledRequest &pending) {
                                          return pending.poll_job_id == request.poll_job_id;
                                        });
    if (duplicate != impl_->poll_requests.end()) {
      *duplicate = request;
      ++impl_->statistics.coalesced;
      lock.unlock();
      impl_->changed.notify_one();
      return concurrency::QueuePushStatus::coalesced;
    }
  }

  if (impl_->depth() >= impl_->config.queue.capacity) {
    ++impl_->statistics.full;
    return concurrency::QueuePushStatus::full;
  }
  if (is_explicit(request.request_kind)) {
    impl_->explicit_requests.push_back(request);
  } else {
    impl_->poll_requests.push_back(request);
  }
  impl_->record_accepted();
  lock.unlock();
  impl_->changed.notify_one();
  return concurrency::QueuePushStatus::accepted;
}

concurrency::QueuePopResult<scheduler::ScheduledRequest>
RequestQueue::try_pop(scheduler::SchedulerTimePoint now) {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->pop_locked(now);
}

concurrency::QueuePopResult<scheduler::ScheduledRequest>
RequestQueue::wait_pop_until(scheduler::SchedulerTimePoint deadline) {
  std::unique_lock<std::mutex> lock(impl_->mutex);
  while (true) {
    const auto now = scheduler::SchedulerClock::now();
    auto result = impl_->pop_locked(now);
    if (result.status != concurrency::QueuePopStatus::empty) {
      return result;
    }
    if (now >= deadline) {
      ++impl_->statistics.timed_out;
      return {concurrency::QueuePopStatus::timed_out, std::nullopt};
    }
    impl_->changed.wait_until(lock, impl_->next_eligibility(deadline));
  }
}

void RequestQueue::close() {
  {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->closed = true;
  }
  impl_->changed.notify_all();
}

bool RequestQueue::closed() const {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->closed;
}

concurrency::QueueStatistics RequestQueue::statistics() const {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->statistics;
}

std::uint32_t RequestQueue::consecutive_explicit_requests() const {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->consecutive_explicit;
}

} // namespace industrial_iot_gateway::pipeline
