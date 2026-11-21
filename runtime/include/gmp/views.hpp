#pragma once
// gmp/views.hpp - zero-copy array views used by generated lazy message views.

#include <cstddef>
#include <cstdint>
#include <iterator>

#include "gmp/endian.hpp"
#include "gmp/result.hpp"

namespace gmp {

/// Random-access view of `count` packed scalars of type T stored with the given endianness.
template <class T>
class ScalarArrayView {
public:
    constexpr ScalarArrayView() noexcept = default;
    constexpr ScalarArrayView(bytes_view data, std::size_t count, Endian e) noexcept
        : data_(data.data()), count_(count), endian_(e) {}

    [[nodiscard]] constexpr std::size_t size() const noexcept { return count_; }
    [[nodiscard]] constexpr bool empty() const noexcept { return count_ == 0; }
    constexpr T operator[](std::size_t i) const noexcept { return load<T>(data_ + i * sizeof(T), endian_); }

    class iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        constexpr iterator() noexcept = default;
        constexpr iterator(const ScalarArrayView* v, std::size_t i) noexcept : v_(v), i_(i) {}
        constexpr T operator*() const noexcept { return (*v_)[i_]; }
        constexpr iterator& operator++() noexcept { ++i_; return *this; }
        constexpr iterator operator++(int) noexcept { auto t = *this; ++i_; return t; }
        constexpr bool operator==(const iterator& o) const noexcept { return i_ == o.i_; }
    private:
        const ScalarArrayView* v_ = nullptr;
        std::size_t i_ = 0;
    };
    constexpr iterator begin() const noexcept { return {this, 0}; }
    constexpr iterator end() const noexcept { return {this, count_}; }

private:
    const std::uint8_t* data_ = nullptr;
    std::size_t count_ = 0;
    Endian endian_ = Endian::big;
};

/// Random-access view of `count` fixed-size elements, each exposed via V::unchecked(bytes_view).
template <class V, std::size_t ElemSize>
class FixedArrayView {
public:
    constexpr FixedArrayView() noexcept = default;
    constexpr FixedArrayView(bytes_view data, std::size_t count) noexcept : data_(data.data()), count_(count) {}

    [[nodiscard]] constexpr std::size_t size() const noexcept { return count_; }
    [[nodiscard]] constexpr bool empty() const noexcept { return count_ == 0; }
    constexpr V operator[](std::size_t i) const noexcept {
        return V::unchecked(bytes_view(data_ + i * ElemSize, ElemSize));
    }

    class iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = V;
        using difference_type = std::ptrdiff_t;
        constexpr iterator() noexcept = default;
        constexpr iterator(const FixedArrayView* v, std::size_t i) noexcept : v_(v), i_(i) {}
        constexpr V operator*() const noexcept { return (*v_)[i_]; }
        constexpr iterator& operator++() noexcept { ++i_; return *this; }
        constexpr iterator operator++(int) noexcept { auto t = *this; ++i_; return t; }
        constexpr bool operator==(const iterator& o) const noexcept { return i_ == o.i_; }
    private:
        const FixedArrayView* v_ = nullptr;
        std::size_t i_ = 0;
    };
    constexpr iterator begin() const noexcept { return {this, 0}; }
    constexpr iterator end() const noexcept { return {this, count_}; }

private:
    const std::uint8_t* data_ = nullptr;
    std::size_t count_ = 0;
};

/// Forward-only view of variable-size elements laid out back to back, filling `data` exactly.
/// V must provide `static V unchecked(bytes_view)` and `std::size_t wire_size() const`.
template <class V>
class SeqArrayView {
public:
    constexpr SeqArrayView() noexcept = default;
    constexpr explicit SeqArrayView(bytes_view data) noexcept : data_(data) {}

    [[nodiscard]] constexpr bool empty() const noexcept { return data_.empty(); }
    /// O(n): walks the elements.
    [[nodiscard]] constexpr std::size_t size() const noexcept {
        std::size_t n = 0;
        for (auto it = begin(); it != end(); ++it) ++n;
        return n;
    }

    class iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = V;
        using difference_type = std::ptrdiff_t;
        constexpr iterator() noexcept = default;
        constexpr explicit iterator(bytes_view rest) noexcept : rest_(rest) {}
        constexpr V operator*() const noexcept { return V::unchecked(rest_); }
        constexpr iterator& operator++() noexcept {
            rest_ = rest_.subspan(V::unchecked(rest_).wire_size());
            return *this;
        }
        constexpr iterator operator++(int) noexcept { auto t = *this; ++*this; return t; }
        constexpr bool operator==(const iterator& o) const noexcept { return rest_.size() == o.rest_.size(); }
    private:
        bytes_view rest_{};
    };
    constexpr iterator begin() const noexcept { return iterator{data_}; }
    constexpr iterator end() const noexcept { return iterator{bytes_view{}}; }

private:
    bytes_view data_{};
};

} // namespace gmp
