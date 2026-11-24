#pragma once
// gmp/byte_reader.hpp - bounds-checked forward cursor over a byte span. No allocation.

#include <cstddef>
#include <cstdint>

#include "gmp/endian.hpp"
#include "gmp/result.hpp"

namespace gmsg {

class ByteReader {
public:
    constexpr ByteReader() noexcept = default;
    constexpr explicit ByteReader(bytes_view data) noexcept : data_(data) {}

    [[nodiscard]] constexpr std::size_t position() const noexcept { return pos_; }
    [[nodiscard]] constexpr std::size_t remaining() const noexcept { return data_.size() - pos_; }
    [[nodiscard]] constexpr bool empty() const noexcept { return pos_ >= data_.size(); }
    [[nodiscard]] constexpr bytes_view data() const noexcept { return data_; }
    [[nodiscard]] constexpr bytes_view rest() const noexcept { return data_.subspan(pos_); }

    template <Arithmetic T>
    [[nodiscard]] constexpr Result<T> read(Endian e = Endian::big) noexcept {
        if (remaining() < sizeof(T)) return fail(Error::Truncated);
        T v = load<T>(data_.data() + pos_, e);
        pos_ += sizeof(T);
        return v;
    }

    /// Read an unsigned integer of n bytes (1..8).
    [[nodiscard]] constexpr Result<std::uint64_t> read_uint(std::size_t n, Endian e = Endian::big) noexcept {
        if (remaining() < n) return fail(Error::Truncated);
        auto v = load_uint(data_.data() + pos_, n, e);
        pos_ += n;
        return v;
    }

    [[nodiscard]] constexpr Result<bytes_view> read_bytes(std::size_t n) noexcept {
        if (remaining() < n) return fail(Error::Truncated);
        auto v = data_.subspan(pos_, n);
        pos_ += n;
        return v;
    }

    /// Read bytes up to (not including) `terminator`; the terminator is consumed.
    [[nodiscard]] constexpr Result<bytes_view> read_until(std::uint8_t terminator) noexcept {
        for (std::size_t i = pos_; i < data_.size(); ++i) {
            if (data_[i] == terminator) {
                auto v = data_.subspan(pos_, i - pos_);
                pos_ = i + 1;
                return v;
            }
        }
        return fail(Error::Truncated);
    }

    [[nodiscard]] constexpr VoidResult skip(std::size_t n) noexcept {
        if (remaining() < n) return fail(Error::Truncated);
        pos_ += n;
        return {};
    }

private:
    bytes_view data_{};
    std::size_t pos_ = 0;
};

} // namespace gmsg
