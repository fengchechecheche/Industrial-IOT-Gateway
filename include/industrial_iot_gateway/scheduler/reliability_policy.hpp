#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

#include "industrial_iot_gateway/scheduler/scheduled_request.hpp"

namespace industrial_iot_gateway::scheduler {

struct ReliabilityConfig {
  std::chrono::milliseconds response_timeout{500};
  std::uint32_t max_attempts{3U};
  std::chrono::milliseconds backoff_initial{200};
  double backoff_multiplier{2.0};
  std::chrono::milliseconds backoff_max{2000};
  std::chrono::milliseconds request_deadline{5000};
};

enum class ReliabilityConfigErrorCategory {
  none,
  invalid_response_timeout,
  invalid_max_attempts,
  invalid_backoff,
  invalid_multiplier,
  invalid_deadline,
  insufficient_deadline_budget,
};

struct ReliabilityConfigError {
  ReliabilityConfigErrorCategory category{ReliabilityConfigErrorCategory::none};
};

enum class ReliabilityDisposition {
  retry_wait,
  completed,
  failed,
  cancelled,
};

struct ReliabilityDecision {
  ReliabilityDisposition disposition{ReliabilityDisposition::failed};
  RequestState next_state{RequestState::failed};
  RequestResultCategory result{RequestResultCategory::invalid_configuration};
  std::uint32_t next_attempt{};
  std::optional<SchedulerTimePoint> retry_at{};
  bool retry_exhausted{};
};

class ReliabilityPolicy {
public:
  explicit ReliabilityPolicy(ReliabilityConfig config = {});

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] ReliabilityConfigError validation_error() const noexcept;
  [[nodiscard]] const ReliabilityConfig &config() const noexcept;
  [[nodiscard]] SchedulerTimePoint request_deadline(SchedulerTimePoint created_at) const noexcept;
  [[nodiscard]] SchedulerTimePoint
  attempt_deadline(SchedulerTimePoint sent_at, SchedulerTimePoint request_deadline) const noexcept;
  [[nodiscard]] bool is_retryable(RequestResultCategory category) const noexcept;
  [[nodiscard]] std::chrono::milliseconds
  backoff_after_failed_attempt(std::uint32_t failed_attempt) const noexcept;
  [[nodiscard]] ReliabilityDecision evaluate(RequestResultCategory category,
                                             std::uint32_t current_attempt,
                                             SchedulerTimePoint observed_at,
                                             SchedulerTimePoint request_deadline) const noexcept;

private:
  [[nodiscard]] ReliabilityConfigError validate() const noexcept;
  [[nodiscard]] std::chrono::milliseconds worst_case_budget() const noexcept;

  ReliabilityConfig config_{};
  ReliabilityConfigError validation_error_{};
  bool valid_{};
};

} // namespace industrial_iot_gateway::scheduler
