#pragma once
// gmp/concepts.hpp - contracts for user supplied extensions (custom codecs, checksums, framers).
// Generated code static_asserts these so signature mistakes produce a readable error.

#include <concepts>
#include <cstddef>
#include <string_view>
#include <type_traits>

#include "gmp/byte_reader.hpp"
#include "gmp/byte_writer.hpp"
#include "gmp/framing/frame.hpp"
#include "gmp/result.hpp"
#include "gmp/text.hpp"

namespace gmsg {

/// Binary decoder:  gmsg::Result<T> fn(gmsg::ByteReader&)
template <class F, class T>
concept BinaryDecoder = requires(F f, ByteReader& r) {
    { f(r) } -> std::same_as<Result<T>>;
};

/// Binary encoder:  gmsg::VoidResult fn(gmsg::ByteWriter&, const T&)
template <class F, class T>
concept BinaryEncoder = requires(F f, ByteWriter& w, const T& v) {
    { f(w, v) } -> std::same_as<VoidResult>;
};

/// Text decoder:    gmsg::Result<T> fn(std::string_view token)
template <class F, class T>
concept TextDecoder = requires(F f, std::string_view s) {
    { f(s) } -> std::same_as<Result<T>>;
};

/// Text encoder:    gmsg::VoidResult fn(gmsg::text::TextWriter&, const T&)   (must call begin_token())
template <class F, class T>
concept TextEncoder = requires(F f, text::TextWriter& w, const T& v) {
    { f(w, v) } -> std::same_as<VoidResult>;
};

/// Checksum function: <unsigned> fn(gmsg::bytes_view)
template <class F>
concept ChecksumFunction = requires(F f, bytes_view d) {
    { f(d) } -> std::unsigned_integral;
};

/// Custom framer: a type with  static gmsg::FrameScan scan(gmsg::bytes_view)
template <class T>
concept Framer = requires(bytes_view b) {
    { T::scan(b) } -> std::same_as<FrameScan>;
};

} // namespace gmsg
