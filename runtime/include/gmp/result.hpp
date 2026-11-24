#pragma once
// gmp/result.hpp - exception-free result type and basic byte span aliases.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

#include "gmp/error.hpp"

namespace gmsg {

template <class T>
using Result = std::expected<T, Error>;

using VoidResult = std::expected<void, Error>;

using bytes_view = std::span<const std::uint8_t>;
using mut_bytes = std::span<std::uint8_t>;

[[nodiscard]] constexpr std::unexpected<Error> fail(Error e) noexcept { return std::unexpected<Error>(e); }

inline constexpr VoidResult ok() noexcept { return {}; }

/// Convenience for building byte views from string literals / char buffers.
inline bytes_view as_bytes(const char* data, std::size_t n) noexcept {
    return {reinterpret_cast<const std::uint8_t*>(data), n};
}

} // namespace gmsg
