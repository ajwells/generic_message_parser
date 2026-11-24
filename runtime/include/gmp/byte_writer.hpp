#pragma once
// gmp/byte_writer.hpp - bounds-checked forward cursor writing into a caller supplied buffer.

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "gmp/endian.hpp"
#include "gmp/result.hpp"

namespace gmsg {

class ByteWriter {
public:
    constexpr ByteWriter() noexcept = default;
    constexpr explicit ByteWriter(mut_bytes out) noexcept : out_(out) {}

    [[nodiscard]] constexpr std::size_t position() const noexcept { return pos_; }
    [[nodiscard]] constexpr std::size_t remaining() const noexcept { return out_.size() - pos_; }
    [[nodiscard]] constexpr mut_bytes buffer() const noexcept { return out_; }
    /// Bytes written so far.
    [[nodiscard]] constexpr bytes_view written() const noexcept { return bytes_view(out_.data(), pos_); }

    template <Arithmetic T>
    [[nodiscard]] constexpr VoidResult write(T v, Endian e = Endian::big) noexcept {
        if (remaining() < sizeof(T)) return fail(Error::BufferFull);
        store(out_.data() + pos_, v, e);
        pos_ += sizeof(T);
        return {};
    }

    [[nodiscard]] constexpr VoidResult write_uint(std::uint64_t v, std::size_t n, Endian e = Endian::big) noexcept {
        if (remaining() < n) return fail(Error::BufferFull);
        store_uint(out_.data() + pos_, n, v, e);
        pos_ += n;
        return {};
    }

    [[nodiscard]] constexpr VoidResult write_bytes(bytes_view b) noexcept {
        if (remaining() < b.size()) return fail(Error::BufferFull);
        std::copy(b.begin(), b.end(), out_.begin() + static_cast<std::ptrdiff_t>(pos_));
        pos_ += b.size();
        return {};
    }

    [[nodiscard]] constexpr VoidResult fill(std::uint8_t value, std::size_t n) noexcept {
        if (remaining() < n) return fail(Error::BufferFull);
        std::fill_n(out_.begin() + static_cast<std::ptrdiff_t>(pos_), n, value);
        pos_ += n;
        return {};
    }

    /// Reserve n bytes and return their offset (for back-patching length fields).
    [[nodiscard]] constexpr Result<std::size_t> reserve(std::size_t n) noexcept {
        if (remaining() < n) return fail(Error::BufferFull);
        const auto at = pos_;
        std::fill_n(out_.begin() + static_cast<std::ptrdiff_t>(pos_), n, std::uint8_t{0});
        pos_ += n;
        return at;
    }

    /// Overwrite previously written bytes at absolute offset `at`.
    template <Arithmetic T>
    constexpr void patch(std::size_t at, T v, Endian e = Endian::big) noexcept {
        store(out_.data() + at, v, e);
    }

    constexpr void patch_uint(std::size_t at, std::uint64_t v, std::size_t n, Endian e = Endian::big) noexcept {
        store_uint(out_.data() + at, n, v, e);
    }

private:
    mut_bytes out_{};
    std::size_t pos_ = 0;
};

} // namespace gmsg
