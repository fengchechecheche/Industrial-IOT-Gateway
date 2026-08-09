#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <mutex>
#include <optional>
#include <string>

namespace industrial_iot_gateway::observability {

enum class LogSeverity {
  debug,
  info,
  warning,
  error,
  critical,
};

struct StructuredEvent {
  std::string event{};
  std::string component{};
  LogSeverity severity{LogSeverity::info};
  std::optional<std::uint64_t> request_id{};
  std::optional<std::uint32_t> attempt{};
  std::optional<std::uint8_t> slave_id{};
  std::optional<std::uint8_t> function{};
  std::optional<std::uint16_t> address{};
  std::optional<std::int64_t> deadline_ms{};
  std::string previous_state{};
  std::string next_state{};
  std::string result{};
  std::string reason{};
  std::optional<std::size_t> queue_depth{};
};

[[nodiscard]] std::string format_jsonl(const StructuredEvent &event);

struct LogWriterStatistics {
  std::uint64_t written{};
  std::uint64_t failures{};
};

class JsonlLogWriter {
public:
  explicit JsonlLogWriter(std::ostream &output) noexcept;
  [[nodiscard]] bool write(const StructuredEvent &event) noexcept;
  [[nodiscard]] LogWriterStatistics statistics() const;

private:
  std::ostream &output_;
  mutable std::mutex mutex_{};
  LogWriterStatistics statistics_{};
};

} // namespace industrial_iot_gateway::observability
