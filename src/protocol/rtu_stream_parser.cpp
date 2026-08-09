#include "industrial_iot_gateway/protocol/rtu_stream_parser.hpp"

#include <algorithm>

namespace industrial_iot_gateway::protocol {
namespace {

constexpr std::size_t kFixedRequestSize = 8U;
constexpr std::size_t kFixedWriteResponseSize = 8U;
constexpr std::size_t kExceptionResponseSize = 5U;
constexpr std::size_t kReadResponseBaseSize = 5U;
constexpr std::uint8_t kExceptionMask = 0x80U;

bool is_supported_function(const std::uint8_t function) noexcept {
  return function == static_cast<std::uint8_t>(FunctionCode::read_holding_registers) ||
         function == static_cast<std::uint8_t>(FunctionCode::read_input_registers) ||
         function == static_cast<std::uint8_t>(FunctionCode::write_single_register);
}

bool is_supported_exception_function(const std::uint8_t function) noexcept {
  const auto request_function = static_cast<std::uint8_t>(function & ~kExceptionMask);
  return (function & kExceptionMask) != 0U && is_supported_function(request_function);
}

std::uint8_t request_slave_id(const Request &request) noexcept {
  if (const auto *read = std::get_if<ReadRequest>(&request)) {
    return read->slave_id;
  }
  if (const auto *write = std::get_if<WriteSingleRegisterRequest>(&request)) {
    return write->slave_id;
  }
  return 0U;
}

ParserErrorCategory map_codec_error(const CodecErrorCategory category) noexcept {
  switch (category) {
  case CodecErrorCategory::frame_too_short:
    return ParserErrorCategory::frame_too_short;
  case CodecErrorCategory::frame_too_long:
    return ParserErrorCategory::frame_too_long;
  case CodecErrorCategory::crc_mismatch:
    return ParserErrorCategory::crc_mismatch;
  case CodecErrorCategory::unexpected_slave:
    return ParserErrorCategory::unexpected_slave;
  case CodecErrorCategory::unexpected_function:
    return ParserErrorCategory::unexpected_function;
  case CodecErrorCategory::byte_count_mismatch:
    return ParserErrorCategory::byte_count_mismatch;
  case CodecErrorCategory::response_echo_mismatch:
    return ParserErrorCategory::response_echo_mismatch;
  case CodecErrorCategory::trailing_bytes:
    return ParserErrorCategory::trailing_bytes;
  default:
    return ParserErrorCategory::invalid_frame;
  }
}

} // namespace

RtuStreamParser::RtuStreamParser(const ParserTiming timing) noexcept
    : timing_(timing),
      timing_valid_(timing.inter_character_timeout_us > 0U &&
                    timing.frame_boundary_timeout_us > timing.inter_character_timeout_us) {}

bool RtuStreamParser::timing_valid() const noexcept { return timing_valid_; }

void RtuStreamParser::begin_request_stream() noexcept {
  mode_ = ParserMode::request_stream;
  expected_request_.reset();
  request_id_ = 0U;
  clear_buffer();
}

bool RtuStreamParser::begin_response_stream(const std::uint64_t request_id,
                                            const Request &expected_request) noexcept {
  if (!timing_valid_ || std::holds_alternative<CodecError>(encode_request(expected_request))) {
    return false;
  }
  mode_ = ParserMode::response_stream;
  expected_request_ = expected_request;
  request_id_ = request_id;
  clear_buffer();
  return true;
}

void RtuStreamParser::clear_request_context() noexcept {
  expected_request_.reset();
  request_id_ = 0U;
  clear_buffer();
}

ParseBatchResult RtuStreamParser::ingest(const std::uint8_t *data, const std::size_t size,
                                         const MonotonicTimeUs arrival_time, ParserEvent *output,
                                         const std::size_t output_capacity) noexcept {
  ParseBatchResult result{};
  const auto arrival_time_us = arrival_time.value;
  if (data == nullptr && size > 0U) {
    return result;
  }
  if (output == nullptr && output_capacity > 0U) {
    result.output_full = true;
    return result;
  }

  if (!timing_valid_) {
    Adu raw{};
    if (size > 0U) {
      raw.bytes[0] = data[0];
      raw.size = 1U;
    }
    if (!emit_error(ParserErrorCategory::invalid_timing, arrival_time_us, raw, 0U, nullptr, output,
                    output_capacity, result.events_written)) {
      result.output_full = size > 0U;
      return result;
    }
  }

  if (buffered_size_ > 0U && has_last_byte_time_ && arrival_time_us > last_byte_time_us_) {
    const auto gap = arrival_time_us - last_byte_time_us_;
    if (gap > timing_.inter_character_timeout_us) {
      Adu raw{};
      raw.size = std::min(buffered_size_, kMaxModbusAduSize);
      std::copy_n(buffer_.begin(), raw.size, raw.bytes.begin());
      const auto category = gap >= timing_.frame_boundary_timeout_us
                                ? ParserErrorCategory::frame_too_short
                                : ParserErrorCategory::inter_character_timeout;
      if (!emit_error(category, arrival_time_us, raw, buffered_size_, nullptr, output,
                      output_capacity, result.events_written)) {
        result.output_full = true;
        return result;
      }
      clear_buffer();
    }
  }

  if (!process_buffer(arrival_time_us, output, output_capacity, result.events_written)) {
    result.output_full = true;
    return result;
  }

  while (result.bytes_consumed < size) {
    if (result.events_written >= output_capacity && result.bytes_consumed < size) {
      result.output_full = true;
      break;
    }

    if (buffered_size_ >= buffer_.size()) {
      Adu raw{};
      raw.size = kMaxModbusAduSize;
      std::copy_n(buffer_.begin(), raw.size, raw.bytes.begin());
      if (!emit_error(ParserErrorCategory::frame_too_long, arrival_time_us, raw, buffered_size_,
                      nullptr, output, output_capacity, result.events_written)) {
        result.output_full = true;
        break;
      }
      clear_buffer();
    }

    buffer_[buffered_size_] = data[result.bytes_consumed];
    ++buffered_size_;
    ++result.bytes_consumed;
    maximum_buffered_size_ = std::max(maximum_buffered_size_, buffered_size_);
    has_last_byte_time_ = true;
    last_byte_time_us_ = arrival_time_us;

    if (!process_buffer(arrival_time_us, output, output_capacity, result.events_written)) {
      result.output_full = result.bytes_consumed < size || result.events_written >= output_capacity;
      break;
    }
  }

  return result;
}

ParseBatchResult RtuStreamParser::on_time_advanced(const MonotonicTimeUs now, ParserEvent *output,
                                                   const std::size_t output_capacity) noexcept {
  ParseBatchResult result{};
  const auto now_us = now.value;
  if (output == nullptr && output_capacity > 0U) {
    result.output_full = true;
    return result;
  }
  if (!process_buffer(now_us, output, output_capacity, result.events_written)) {
    result.output_full = true;
    return result;
  }
  if (buffered_size_ == 0U || !has_last_byte_time_ || now_us <= last_byte_time_us_) {
    return result;
  }

  const auto gap = now_us - last_byte_time_us_;
  if (gap <= timing_.inter_character_timeout_us) {
    return result;
  }

  Adu raw{};
  raw.size = std::min(buffered_size_, kMaxModbusAduSize);
  std::copy_n(buffer_.begin(), raw.size, raw.bytes.begin());
  const auto category = gap >= timing_.frame_boundary_timeout_us
                            ? ParserErrorCategory::frame_too_short
                            : ParserErrorCategory::inter_character_timeout;
  if (!emit_error(category, now_us, raw, buffered_size_, nullptr, output, output_capacity,
                  result.events_written)) {
    result.output_full = true;
    return result;
  }
  clear_buffer();
  return result;
}

std::size_t RtuStreamParser::buffered_size() const noexcept { return buffered_size_; }

std::size_t RtuStreamParser::maximum_buffered_size() const noexcept {
  return maximum_buffered_size_;
}

std::size_t RtuStreamParser::discarded_noise_bytes() const noexcept {
  return discarded_noise_bytes_;
}

bool RtuStreamParser::process_buffer(const std::uint64_t timestamp_us, ParserEvent *output,
                                     const std::size_t output_capacity,
                                     std::size_t &events_written) noexcept {
  while (buffered_size_ > 0U) {
    if (buffered_size_ < 2U) {
      return true;
    }

    std::size_t candidate_size = 0U;
    if (mode_ == ParserMode::request_stream) {
      if (!is_supported_function(buffer_[1])) {
        ++discarded_noise_bytes_;
        discard_prefix(1U);
        continue;
      }
      candidate_size = kFixedRequestSize;
    } else {
      if (!expected_request_.has_value()) {
        Adu raw{};
        raw.bytes[0] = buffer_[0];
        raw.size = 1U;
        if (!emit_error(ParserErrorCategory::no_request_context, timestamp_us, raw, 1U, nullptr,
                        output, output_capacity, events_written)) {
          return false;
        }
        discard_prefix(1U);
        continue;
      }

      const auto expected_slave = request_slave_id(*expected_request_);
      const auto function = buffer_[1];
      if (buffer_[0] != expected_slave &&
          !(buffer_[0] >= 1U && buffer_[0] <= 247U &&
            (is_supported_function(function) || is_supported_exception_function(function)))) {
        ++discarded_noise_bytes_;
        discard_prefix(1U);
        continue;
      }
      if (!is_supported_function(function) && !is_supported_exception_function(function)) {
        ++discarded_noise_bytes_;
        discard_prefix(1U);
        continue;
      }

      if (is_supported_exception_function(function)) {
        candidate_size = kExceptionResponseSize;
      } else if (function == static_cast<std::uint8_t>(FunctionCode::write_single_register)) {
        candidate_size = kFixedWriteResponseSize;
      } else {
        if (buffered_size_ < 3U) {
          return true;
        }
        const auto byte_count = buffer_[2];
        candidate_size = kReadResponseBaseSize + byte_count;
        if (candidate_size > kMaxModbusAduSize) {
          Adu raw{};
          raw.size = std::min(buffered_size_, kMaxModbusAduSize);
          std::copy_n(buffer_.begin(), raw.size, raw.bytes.begin());
          if (!emit_error(ParserErrorCategory::frame_too_long, timestamp_us, raw, 1U, nullptr,
                          output, output_capacity, events_written)) {
            return false;
          }
          discard_prefix(1U);
          continue;
        }
        if ((byte_count & 0x01U) != 0U || byte_count > kMaxReadRegisterQuantity * 2U) {
          Adu raw{};
          raw.size = std::min(buffered_size_, kMaxModbusAduSize);
          std::copy_n(buffer_.begin(), raw.size, raw.bytes.begin());
          if (!emit_error(ParserErrorCategory::byte_count_mismatch, timestamp_us, raw, 1U, nullptr,
                          output, output_capacity, events_written)) {
            return false;
          }
          discard_prefix(1U);
          continue;
        }
      }
    }

    if (candidate_size > kMaxModbusAduSize) {
      Adu raw{};
      raw.size = std::min(buffered_size_, kMaxModbusAduSize);
      std::copy_n(buffer_.begin(), raw.size, raw.bytes.begin());
      if (!emit_error(ParserErrorCategory::frame_too_long, timestamp_us, raw, 1U, nullptr, output,
                      output_capacity, events_written)) {
        return false;
      }
      discard_prefix(1U);
      continue;
    }
    if (buffered_size_ < candidate_size) {
      return true;
    }

    Adu raw{};
    raw.size = candidate_size;
    std::copy_n(buffer_.begin(), candidate_size, raw.bytes.begin());

    if (mode_ == ParserMode::request_stream) {
      const auto decoded = decode_request(raw.bytes.data(), raw.size);
      if (const auto *request = std::get_if<Request>(&decoded)) {
        if (events_written >= output_capacity || output == nullptr) {
          return false;
        }
        output[events_written] = ParserEvent{timestamp_us, ParsedRequestEvent{*request, raw}};
        ++events_written;
        discard_prefix(candidate_size);
        continue;
      }
      const auto *codec_error = std::get_if<CodecError>(&decoded);
      if (codec_error == nullptr ||
          !emit_error(map_codec_error(codec_error->category), timestamp_us, raw, 1U, codec_error,
                      output, output_capacity, events_written)) {
        return false;
      }
      discard_prefix(1U);
      continue;
    }

    const auto decoded = decode_response(raw.bytes.data(), raw.size, expected_request_.value());
    if (const auto *response = std::get_if<Response>(&decoded)) {
      if (events_written >= output_capacity || output == nullptr) {
        return false;
      }
      output[events_written] =
          ParserEvent{timestamp_us, ParsedResponseEvent{request_id_, *response, raw}};
      ++events_written;
      discard_prefix(candidate_size);
      continue;
    }
    const auto *codec_error = std::get_if<CodecError>(&decoded);
    if (codec_error == nullptr ||
        !emit_error(map_codec_error(codec_error->category), timestamp_us, raw, 1U, codec_error,
                    output, output_capacity, events_written)) {
      return false;
    }
    discard_prefix(1U);
  }
  return true;
}

bool RtuStreamParser::emit_error(const ParserErrorCategory category,
                                 const std::uint64_t timestamp_us, const Adu &raw_adu,
                                 const std::size_t discarded_bytes, const CodecError *codec_error,
                                 ParserEvent *output, const std::size_t output_capacity,
                                 std::size_t &events_written) noexcept {
  if (events_written >= output_capacity || output == nullptr) {
    return false;
  }
  ParserError error{};
  error.category = category;
  error.raw_adu = raw_adu;
  error.discarded_bytes = discarded_bytes;
  if (codec_error != nullptr) {
    error.has_codec_error = true;
    error.codec_error = *codec_error;
  }
  output[events_written] = ParserEvent{timestamp_us, error};
  ++events_written;
  return true;
}

void RtuStreamParser::discard_prefix(const std::size_t count) noexcept {
  if (count >= buffered_size_) {
    clear_buffer();
    return;
  }
  std::move(buffer_.begin() + static_cast<std::ptrdiff_t>(count),
            buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_size_), buffer_.begin());
  buffered_size_ -= count;
}

void RtuStreamParser::clear_buffer() noexcept {
  buffered_size_ = 0U;
  has_last_byte_time_ = false;
  last_byte_time_us_ = 0U;
}

} // namespace industrial_iot_gateway::protocol
