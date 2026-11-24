#pragma once
// gmp/containers.hpp - fixed capacity containers used by generated eager structs (no heap).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "gmp/result.hpp"

namespace gmsg {

/// Vector with inline storage of N elements. T must be default constructible.
template <class T, std::size_t N>
class static_vector {
public:
    using value_type = T;
    using size_type = std::size_t;
    using iterator = T*;
    using const_iterator = const T*;

    constexpr static_vector() noexcept = default;
    constexpr static_vector(std::initializer_list<T> init) noexcept {
        for (const auto& v : init) (void)push_back(v);
    }

    static constexpr size_type capacity() noexcept { return N; }
    [[nodiscard]] constexpr size_type size() const noexcept { return size_; }
    [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] constexpr bool full() const noexcept { return size_ == N; }

    constexpr T* data() noexcept { return data_.data(); }
    constexpr const T* data() const noexcept { return data_.data(); }
    constexpr iterator begin() noexcept { return data_.data(); }
    constexpr iterator end() noexcept { return data_.data() + size_; }
    constexpr const_iterator begin() const noexcept { return data_.data(); }
    constexpr const_iterator end() const noexcept { return data_.data() + size_; }

    constexpr T& operator[](size_type i) noexcept { return data_[i]; }
    constexpr const T& operator[](size_type i) const noexcept { return data_[i]; }
    constexpr T& back() noexcept { return data_[size_ - 1]; }
    constexpr const T& back() const noexcept { return data_[size_ - 1]; }

    /// Returns false (and does nothing) when full.
    constexpr bool push_back(const T& v) noexcept {
        if (size_ >= N) return false;
        data_[size_++] = v;
        return true;
    }
    constexpr void pop_back() noexcept { if (size_) --size_; }
    constexpr void clear() noexcept { size_ = 0; }
    /// Resize (new elements value-initialised). Returns false when n > N.
    constexpr bool resize(size_type n) noexcept {
        if (n > N) return false;
        for (size_type i = size_; i < n; ++i) data_[i] = T{};
        size_ = n;
        return true;
    }
    constexpr bool assign(std::span<const T> src) noexcept {
        if (src.size() > N) return false;
        std::copy(src.begin(), src.end(), data_.begin());
        size_ = src.size();
        return true;
    }

    constexpr std::span<T> span() noexcept { return {data_.data(), size_}; }
    constexpr std::span<const T> span() const noexcept { return {data_.data(), size_}; }

    friend constexpr bool operator==(const static_vector& a, const static_vector& b) noexcept {
        return std::equal(a.begin(), a.end(), b.begin(), b.end());
    }

private:
    std::array<T, N> data_{};
    size_type size_ = 0;
};

/// String with inline storage of up to N characters (not necessarily NUL terminated).
template <std::size_t N>
class static_string {
public:
    constexpr static_string() noexcept = default;
    constexpr static_string(std::string_view s) noexcept { (void)assign(s); }  // NOLINT(implicit)
    constexpr static_string(const char* s) noexcept { (void)assign(std::string_view(s)); }  // NOLINT(implicit)

    static constexpr std::size_t capacity() noexcept { return N; }
    [[nodiscard]] constexpr std::size_t size() const noexcept { return size_; }
    [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }
    constexpr const char* data() const noexcept { return data_.data(); }
    constexpr char* data() noexcept { return data_.data(); }
    constexpr std::string_view view() const noexcept { return {data_.data(), size_}; }
    constexpr operator std::string_view() const noexcept { return view(); }  // NOLINT(implicit)

    /// Returns false and truncates when s does not fit.
    constexpr bool assign(std::string_view s) noexcept {
        const auto n = std::min(s.size(), N);
        std::copy_n(s.begin(), n, data_.begin());
        size_ = n;
        return n == s.size();
    }
    /// Appends as much of s as fits; returns false when s was truncated.
    constexpr bool append(std::string_view s) noexcept {
        const auto n = std::min(s.size(), N - size_);
        std::copy_n(s.begin(), n, data_.begin() + size_);
        size_ += n;
        return n == s.size();
    }
    constexpr void clear() noexcept { size_ = 0; }

    friend constexpr bool operator==(const static_string& a, const static_string& b) noexcept {
        return a.view() == b.view();
    }
    friend constexpr bool operator==(const static_string& a, std::string_view b) noexcept { return a.view() == b; }
    friend constexpr bool operator==(const static_string& a, const char* b) noexcept {
        return a.view() == std::string_view(b);
    }

private:
    std::array<char, N> data_{};
    std::size_t size_ = 0;
};

inline std::string_view as_string_view(bytes_view b) noexcept {
    return {reinterpret_cast<const char*>(b.data()), b.size()};
}

/// Strip trailing pad bytes (used by fixed-width string fields).
inline std::string_view strip_padding(std::string_view s, char pad) noexcept {
    while (!s.empty() && s.back() == pad) s.remove_suffix(1);
    return s;
}

} // namespace gmsg
