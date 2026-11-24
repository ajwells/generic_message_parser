#pragma once
// gmp/error.hpp - error codes shared by the runtime and generated code.

#include <cstdint>
#include <string_view>

namespace gmsg {

enum class Error : std::uint8_t {
    None = 0,
    Truncated,      // input ended before the message/frame was complete
    BadSync,        // sync bytes / start delimiter not found (garbage skipped)
    BadChecksum,    // checksum / CRC mismatch
    BadLength,      // length field invalid or frame exceeds max size
    BadDelimiter,   // unexpected delimiter inside a frame
    BadConst,       // constant / magic field did not match
    OutOfRange,     // value does not fit (count > capacity, etc.)
    UnknownType,    // the frame's type id (message_type field / token) matched no message
    BufferFull,     // output buffer too small / stream buffer full
    ParseError,     // text field could not be parsed
    MissingField,   // required (text) field was empty or absent
    InvalidValue,   // generic semantic failure (used by custom codecs)
    Custom,         // reserved for user codecs
    UnknownEnum,    // a `strict` enum field held a value that is not one of the enum's values
    ValueOutOfLimits,  // a field value is outside its min_value / max_value / min_raw / max_raw limits
    Overrun,        // input bytes were lost before parsing (a full gmsg::ByteRing dropped them)
    Timeout,        // an endpoint wait (wait_for / transact) or a retried send ran out of time
};

constexpr std::string_view to_string(Error e) noexcept {
    switch (e) {
        case Error::None: return "none";
        case Error::Truncated: return "truncated";
        case Error::BadSync: return "bad sync";
        case Error::BadChecksum: return "bad checksum";
        case Error::BadLength: return "bad length";
        case Error::BadDelimiter: return "bad delimiter";
        case Error::BadConst: return "bad constant";
        case Error::OutOfRange: return "out of range";
        case Error::UnknownType: return "unknown message type";
        case Error::BufferFull: return "buffer full";
        case Error::ParseError: return "parse error";
        case Error::MissingField: return "missing field";
        case Error::InvalidValue: return "invalid value";
        case Error::Custom: return "custom error";
        case Error::UnknownEnum: return "unknown enum value";
        case Error::ValueOutOfLimits: return "value out of limits";
        case Error::Overrun: return "overrun";
        case Error::Timeout: return "timeout";
    }
    return "unknown";
}

} // namespace gmsg
