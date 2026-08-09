#include "industrial_iot_gateway/observability/structured_log.hpp"

#include <iomanip>
#include <ostream>
#include <sstream>
#include <string_view>

namespace industrial_iot_gateway::observability {
namespace {

[[nodiscard]] std::string escape_json(std::string_view value) {
  std::ostringstream escaped{};
  for (const unsigned char character : value) {
    switch (character) {
    case '"':
      escaped << "\\\"";
      break;
    case '\\':
      escaped << "\\\\";
      break;
    case '\b':
      escaped << "\\b";
      break;
    case '\f':
      escaped << "\\f";
      break;
    case '\n':
      escaped << "\\n";
      break;
    case '\r':
      escaped << "\\r";
      break;
    case '\t':
      escaped << "\\t";
      break;
    default:
      if (character < 0x20U) {
        escaped << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                << static_cast<unsigned int>(character) << std::dec;
      } else {
        escaped << static_cast<char>(character);
      }
      break;
    }
  }
  return escaped.str();
}

[[nodiscard]] std::string_view severity_name(LogSeverity severity) noexcept {
  switch (severity) {
  case LogSeverity::debug:
    return "debug";
  case LogSeverity::info:
    return "info";
  case LogSeverity::warning:
    return "warning";
  case LogSeverity::error:
    return "error";
  case LogSeverity::critical:
    return "critical";
  }
  return "unknown";
}

void append_string(std::ostringstream &output, std::string_view name, const std::string &value) {
  if (!value.empty()) {
    output << ",\"" << name << "\":\"" << escape_json(value) << '"';
  }
}

template <typename T>
void append_number(std::ostringstream &output, std::string_view name,
                   const std::optional<T> &value) {
  if (value.has_value()) {
    output << ",\"" << name << "\":" << +(*value);
  }
}

} // namespace

std::string format_jsonl(const StructuredEvent &event) {
  std::ostringstream output{};
  output << "{\"event\":\"" << escape_json(event.event) << "\",\"component\":\""
         << escape_json(event.component) << "\",\"severity\":\"" << severity_name(event.severity)
         << '"';
  append_number(output, "request_id", event.request_id);
  append_number(output, "attempt", event.attempt);
  append_number(output, "slave_id", event.slave_id);
  append_number(output, "function", event.function);
  append_number(output, "address", event.address);
  append_number(output, "deadline_ms", event.deadline_ms);
  append_string(output, "previous_state", event.previous_state);
  append_string(output, "next_state", event.next_state);
  append_string(output, "result", event.result);
  append_string(output, "reason", event.reason);
  append_number(output, "queue_depth", event.queue_depth);
  output << "}\n";
  return output.str();
}

JsonlLogWriter::JsonlLogWriter(std::ostream &output) noexcept : output_(output) {}

bool JsonlLogWriter::write(const StructuredEvent &event) noexcept {
  try {
    const auto line = format_jsonl(event);
    const std::lock_guard<std::mutex> lock(mutex_);
    output_ << line;
    if (!output_.good()) {
      ++statistics_.failures;
      return false;
    }
    ++statistics_.written;
    return true;
  } catch (...) {
    const std::lock_guard<std::mutex> lock(mutex_);
    ++statistics_.failures;
    return false;
  }
}

LogWriterStatistics JsonlLogWriter::statistics() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return statistics_;
}

} // namespace industrial_iot_gateway::observability
