#include "ir/ir.hpp"

#include <algorithm>

namespace gmpc::ir {

std::size_t prim_size(Prim p) {
    switch (p) {
        case Prim::U8: case Prim::I8: case Prim::Bool: case Prim::Char: return 1;
        case Prim::U16: case Prim::I16: return 2;
        case Prim::U32: case Prim::I32: case Prim::F32: return 4;
        case Prim::U64: case Prim::I64: case Prim::F64: return 8;
    }
    return 0;
}

bool prim_is_signed(Prim p) {
    return p == Prim::I8 || p == Prim::I16 || p == Prim::I32 || p == Prim::I64;
}

bool prim_is_float(Prim p) { return p == Prim::F32 || p == Prim::F64; }

bool prim_is_integer(Prim p) { return !prim_is_float(p) && p != Prim::Bool && p != Prim::Char; }

const char* prim_name(Prim p) {
    switch (p) {
        case Prim::U8: return "u8";
        case Prim::U16: return "u16";
        case Prim::U32: return "u32";
        case Prim::U64: return "u64";
        case Prim::I8: return "i8";
        case Prim::I16: return "i16";
        case Prim::I32: return "i32";
        case Prim::I64: return "i64";
        case Prim::F32: return "f32";
        case Prim::F64: return "f64";
        case Prim::Bool: return "bool";
        case Prim::Char: return "char";
    }
    return "?";
}

std::optional<Prim> parse_prim(const std::string& s) {
    static const std::pair<const char*, Prim> table[] = {
        {"u8", Prim::U8},   {"u16", Prim::U16}, {"u32", Prim::U32}, {"u64", Prim::U64},
        {"i8", Prim::I8},   {"i16", Prim::I16}, {"i32", Prim::I32}, {"i64", Prim::I64},
        {"f32", Prim::F32}, {"f64", Prim::F64}, {"bool", Prim::Bool}, {"char", Prim::Char},
        {"uint8", Prim::U8}, {"uint16", Prim::U16}, {"uint32", Prim::U32}, {"uint64", Prim::U64},
        {"int8", Prim::I8}, {"int16", Prim::I16}, {"int32", Prim::I32}, {"int64", Prim::I64},
        {"float", Prim::F32}, {"double", Prim::F64},
    };
    for (const auto& [n, p] : table)
        if (s == n) return p;
    return std::nullopt;
}

namespace {
template <class Vec>
auto find_named(const Vec& v, const std::string& n) -> decltype(v.front().get()) {
    for (const auto& x : v)
        if (x->name == n) return x.get();
    return nullptr;
}
} // namespace

const EnumDef* Schema::find_enum(const std::string& n) const { return find_named(enums, n); }
const Message* Schema::find_message(const std::string& n) const { return find_named(messages, n); }
const CustomType* Schema::find_custom_type(const std::string& n) const { return find_named(custom_types, n); }
const CustomChecksum* Schema::find_custom_checksum(const std::string& n) const {
    return find_named(custom_checksums, n);
}
const CustomFramer* Schema::find_custom_framer(const std::string& n) const { return find_named(custom_framers, n); }

namespace {
const std::vector<std::pair<std::string, std::size_t>>& checksum_table() {
    static const std::vector<std::pair<std::string, std::size_t>> t = {
        {"crc8", 1},         {"crc8_maxim", 1},   {"crc8_autosar", 1}, {"crc8_nrsc5", 1},
        {"crc15_can", 2},
        {"crc16_ccitt", 2},  {"crc16_xmodem", 2}, {"crc16_kermit", 2}, {"crc16_x25", 2},
        {"crc16_modbus", 2}, {"crc16_arc", 2},    {"crc24_openpgp", 3}, {"crc32", 4},
        {"crc32c", 4},       {"crc32_mpeg2", 4},  {"crc64_ecma", 8},   {"xor8", 1},
        {"sum8", 1},         {"lrc8", 1},         {"sum16", 2},        {"fletcher16", 2},
        {"fletcher8_ubx", 2},
    };
    return t;
}
} // namespace

std::optional<std::size_t> builtin_checksum_size(const std::string& name) {
    for (const auto& [n, s] : checksum_table())
        if (n == name) return s;
    return std::nullopt;
}

const std::vector<std::string>& builtin_checksum_names() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> v;
        for (const auto& [n, s] : checksum_table()) v.push_back(n);
        return v;
    }();
    return names;
}

} // namespace gmpc::ir
