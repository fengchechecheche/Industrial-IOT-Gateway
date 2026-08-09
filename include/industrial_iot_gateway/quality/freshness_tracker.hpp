#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

#include "industrial_iot_gateway/scheduler/scheduled_request.hpp"

namespace industrial_iot_gateway::quality {

enum class RegisterQuality {
  no_valid_sample,
  fresh,
  stale,
  invalid,
  offline,
};

struct RegisterFreshnessConfig {
  std::uint32_t poll_job_id{};
  std::uint8_t slave_id{};
  std::chrono::milliseconds poll_period{};
  std::chrono::milliseconds freshness{};
};

enum class FreshnessErrorCategory {
  none,
  empty_config,
  duplicate_poll_job_id,
  invalid_slave_address,
  invalid_poll_period,
  invalid_freshness,
  unknown_poll_job,
  time_regression,
};

struct FreshnessError {
  FreshnessErrorCategory category{FreshnessErrorCategory::none};
  std::uint32_t poll_job_id{};
};

struct QualityTransition {
  std::uint32_t poll_job_id{};
  std::uint8_t slave_id{};
  RegisterQuality previous{RegisterQuality::no_valid_sample};
  RegisterQuality current{RegisterQuality::no_valid_sample};
  scheduler::SchedulerTimePoint occurred_at{};
  bool value_is_retained{};
};

struct QualityUpdateBatch {
  FreshnessError error{};
  std::vector<QualityTransition> transitions{};
};

class FreshnessTracker {
public:
  explicit FreshnessTracker(const std::vector<RegisterFreshnessConfig> &configs);

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] FreshnessError validation_error() const noexcept;
  [[nodiscard]] std::size_t register_count() const noexcept;
  [[nodiscard]] RegisterQuality quality(std::uint32_t poll_job_id) const noexcept;
  [[nodiscard]] std::optional<scheduler::SchedulerTimePoint>
  last_valid_time(std::uint32_t poll_job_id) const noexcept;

  [[nodiscard]] QualityUpdateBatch record_valid_sample(std::uint32_t poll_job_id,
                                                       scheduler::SchedulerTimePoint observed_at);
  [[nodiscard]] QualityUpdateBatch record_invalid_sample(std::uint32_t poll_job_id,
                                                         scheduler::SchedulerTimePoint observed_at);
  [[nodiscard]] QualityUpdateBatch record_poll_failure(std::uint32_t poll_job_id,
                                                       scheduler::SchedulerTimePoint observed_at);
  [[nodiscard]] QualityUpdateBatch set_device_offline(std::uint8_t slave_id, bool offline,
                                                      scheduler::SchedulerTimePoint observed_at);
  [[nodiscard]] QualityUpdateBatch advance_time(scheduler::SchedulerTimePoint now);

private:
  struct RegisterState {
    RegisterFreshnessConfig config{};
    RegisterQuality base_quality{RegisterQuality::no_valid_sample};
    std::optional<scheduler::SchedulerTimePoint> last_valid{};
  };

  [[nodiscard]] FreshnessError validate() const noexcept;
  [[nodiscard]] std::optional<std::size_t> find(std::uint32_t poll_job_id) const noexcept;
  [[nodiscard]] RegisterQuality effective_quality(const RegisterState &state) const noexcept;
  [[nodiscard]] bool accept_time(scheduler::SchedulerTimePoint now) noexcept;
  void append_transition(QualityUpdateBatch &batch, const RegisterState &state,
                         RegisterQuality previous, RegisterQuality current,
                         scheduler::SchedulerTimePoint now) const;

  std::vector<RegisterState> registers_{};
  std::array<bool, 256U> device_offline_{};
  bool valid_{};
  FreshnessError validation_error_{};
  std::optional<scheduler::SchedulerTimePoint> last_time_{};
};

} // namespace industrial_iot_gateway::quality
