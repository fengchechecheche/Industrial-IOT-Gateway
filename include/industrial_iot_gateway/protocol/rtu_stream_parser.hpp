#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>

#include "industrial_iot_gateway/protocol/modbus_codec.hpp"

namespace industrial_iot_gateway::protocol {

inline constexpr std::size_t kRtuParserBufferCapacity = 2U * kMaxModbusAduSize;

enum class ParserMode {
  request_stream,
  response_stream,
};

enum class ParserErrorCategory {
  invalid_timing,
  no_request_context,
  inter_character_timeout,
  frame_too_short,
  frame_too_long,
  crc_mismatch,
  invalid_frame,
  unexpected_slave,
  unexpected_function,
  byte_count_mismatch,
  response_echo_mismatch,
  trailing_bytes,
};

struct MonotonicTimeUs {
  std::uint64_t value{};
};

struct ParserTiming {
  std::uint64_t inter_character_timeout_us{};
  std::uint64_t frame_boundary_timeout_us{};
};

struct ParserError {
  ParserErrorCategory category{};
  bool has_codec_error{};
  CodecError codec_error{};
  Adu raw_adu{};
  std::size_t discarded_bytes{};
};

struct ParsedRequestEvent {
  Request request{};
  Adu raw_adu{};
};

struct ParsedResponseEvent {
  std::uint64_t request_id{};
  Response response{};
  Adu raw_adu{};
};

using ParserEventPayload = std::variant<ParsedRequestEvent, ParsedResponseEvent, ParserError>;

struct ParserEvent {
  std::uint64_t timestamp_us{};
  ParserEventPayload payload{};
};

struct ParseBatchResult {
  std::size_t bytes_consumed{};
  std::size_t events_written{};
  bool output_full{};
};

class RtuStreamParser {
public:
  explicit RtuStreamParser(ParserTiming timing) noexcept;

  [[nodiscard]] bool timing_valid() const noexcept;
  void begin_request_stream() noexcept;
  [[nodiscard]] bool begin_response_stream(std::uint64_t request_id,
                                           const Request &expected_request) noexcept;
  void clear_request_context() noexcept;

  [[nodiscard]] ParseBatchResult ingest(const std::uint8_t *data, std::size_t size,
                                        MonotonicTimeUs arrival_time, ParserEvent *output,
                                        std::size_t output_capacity) noexcept;
  [[nodiscard]] ParseBatchResult on_time_advanced(MonotonicTimeUs now, ParserEvent *output,
                                                  std::size_t output_capacity) noexcept;

  [[nodiscard]] std::size_t buffered_size() const noexcept;
  [[nodiscard]] std::size_t maximum_buffered_size() const noexcept;
  [[nodiscard]] std::size_t discarded_noise_bytes() const noexcept;

private:
  [[nodiscard]] bool process_buffer(std::uint64_t timestamp_us, ParserEvent *output,
                                    std::size_t output_capacity,
                                    std::size_t &events_written) noexcept;
  [[nodiscard]] bool emit_error(ParserErrorCategory category, std::uint64_t timestamp_us,
                                const Adu &raw_adu, std::size_t discarded_bytes,
                                const CodecError *codec_error, ParserEvent *output,
                                std::size_t output_capacity, std::size_t &events_written) noexcept;
  void discard_prefix(std::size_t count) noexcept;
  void clear_buffer() noexcept;

  ParserTiming timing_{};
  ParserMode mode_{ParserMode::request_stream};
  bool timing_valid_{};
  std::optional<Request> expected_request_{};
  std::uint64_t request_id_{};
  std::array<std::uint8_t, kRtuParserBufferCapacity> buffer_{};
  std::size_t buffered_size_{};
  std::size_t maximum_buffered_size_{};
  std::size_t discarded_noise_bytes_{};
  bool has_last_byte_time_{};
  std::uint64_t last_byte_time_us_{};
};

} // namespace industrial_iot_gateway::protocol
