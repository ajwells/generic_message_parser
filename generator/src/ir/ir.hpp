#pragma once
// Language-neutral intermediate representation of a GMP schema.
//
// The loader fills the "declared" members from YAML; lowering (ir/lowering.cpp) resolves type
// references and expressions and computes layout ("derived" members). Backends only read the IR.

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "diagnostics.hpp"

namespace gmpc::ir {

enum class Prim { U8, U16, U32, U64, I8, I16, I32, I64, F32, F64, Bool, Char };
enum class Endian { Big, Little };
enum class BitOrder { MsbFirst, LsbFirst };
enum class Format { Binary, Text };

std::size_t prim_size(Prim p);
bool prim_is_signed(Prim p);
bool prim_is_float(Prim p);
bool prim_is_integer(Prim p);  // includes Bool/Char = false
const char* prim_name(Prim p);
std::optional<Prim> parse_prim(const std::string& s);

struct Field;
struct BitField;
struct Message;
struct EnumDef;

// ------------------------------------------------------------------------------------------------
// Expressions (conditions, counts, lengths)
// ------------------------------------------------------------------------------------------------
struct Expr {
    enum class Kind { Int, Ref, EnumConst, Unary, Binary };
    Kind kind = Kind::Int;
    SourceLoc loc;
    std::int64_t value = 0;           // Int
    std::vector<std::string> path;    // Ref: field[.bitfield]; EnumConst: Enum.value
    std::string op;                   // Unary/Binary operator
    std::shared_ptr<Expr> lhs, rhs;   // Unary uses lhs

    // resolved
    const Field* field = nullptr;       // Ref
    const BitField* bitfield = nullptr; // Ref into a bits group
    const EnumDef* enum_def = nullptr;  // EnumConst
};
using ExprPtr = std::shared_ptr<Expr>;

// ------------------------------------------------------------------------------------------------
// Declarations
// ------------------------------------------------------------------------------------------------
struct EnumDef {
    std::string name;
    SourceLoc loc;
    Prim underlying = Prim::U8;
    std::vector<std::pair<std::string, std::int64_t>> values;
    std::string doc;

    [[nodiscard]] std::optional<std::int64_t> find(const std::string& v) const {
        for (const auto& [n, x] : values)
            if (n == v) return x;
        return std::nullopt;
    }
};

/// Per-language key/value settings, e.g. lang["cpp"]["type"] = "my::Time".
using LangBlocks = std::map<std::string, std::map<std::string, std::string>>;

struct CustomType {
    std::string name;
    SourceLoc loc;
    LangBlocks lang;
    std::optional<std::size_t> wire_size;      // binary: fixed size if known
    std::optional<std::size_t> max_wire_size;  // binary: upper bound if variable
};

struct CustomChecksum {
    std::string name;
    SourceLoc loc;
    LangBlocks lang;
    std::size_t size = 0;  // bytes on the wire
};

struct CustomFramer {
    std::string name;
    SourceLoc loc;
    LangBlocks lang;
};

// ------------------------------------------------------------------------------------------------
// Fields
// ------------------------------------------------------------------------------------------------
enum class FieldKind {
    Scalar,   // integer / float / bool / char
    Enum,
    Bits,     // group of bit fields packed into whole bytes
    Bytes,    // raw byte blob
    String,   // character data
    Array,    // repeated element (see Field::elem)
    Message,  // nested message
    Custom,   // user codec
    Pad,      // skipped bytes (binary) / ignored tokens (text)
};

enum class LenKind { None, Fixed, Expr, Remaining, Terminator };

struct LengthSpec {
    LenKind kind = LenKind::None;
    std::size_t fixed = 0;
    ExprPtr expr;
    std::uint8_t terminator = 0;
};

struct BitField {
    std::string name;  // empty for padding
    SourceLoc loc;
    unsigned bits = 0;
    unsigned offset = 0;  // bit offset inside the group (derived)
    bool pad = false;
    std::string type_name;           // declared ("bool", "u8", "i16", enum name) - may be empty
    Prim prim = Prim::U8;            // storage type (derived)
    const EnumDef* enum_def = nullptr;
    std::optional<double> scale, offset_value;
    std::optional<std::int64_t> const_value;
    std::string doc;
};

struct Field {
    std::string name;  // empty for Pad
    SourceLoc loc;
    std::string type_name;  // as declared

    FieldKind kind = FieldKind::Scalar;
    Prim prim = Prim::U8;
    Endian endian = Endian::Big;
    BitOrder bit_order = BitOrder::MsbFirst;
    const EnumDef* enum_def = nullptr;
    const Message* message = nullptr;
    const CustomType* custom = nullptr;

    std::vector<BitField> bits;  // Bits
    std::size_t bits_bytes = 0;  // Bits (derived)

    LengthSpec len;                     // Bytes/String: length; Array: count; Pad: size/tokens
    std::optional<std::size_t> max;     // capacity for variable-length data
    std::shared_ptr<Field> elem;        // Array element

    ExprPtr when;              // conditional presence
    bool optional = false;     // text: empty token allowed

    std::optional<double> scale, offset_value;
    std::string unit, doc;
    std::optional<std::int64_t> const_value;  // constant / magic value (Scalar/Enum)
    char pad_char = '\0';                     // String with fixed length: trailing pad character

    // text formatting
    int base = 10;
    int width = 0;
    int precision = -1;

    // ---- derived by lowering ----
    std::size_t index = 0;                   // position in the owning message
    std::optional<std::size_t> static_size;  // fixed wire size
    std::optional<std::size_t> max_size;     // upper bound on wire size
    std::size_t min_size = 0;
    std::optional<std::size_t> static_offset;
    const Field* derived_from = nullptr;     // this field's encoded value is the length/count of that field
    std::size_t token_index = 0;             // text: first token index
    std::size_t token_count = 1;             // text: number of tokens consumed

    [[nodiscard]] bool has_member() const {
        return kind != FieldKind::Pad && !const_value.has_value();
    }
};
using FieldPtr = std::shared_ptr<Field>;

// ------------------------------------------------------------------------------------------------
// Messages
// ------------------------------------------------------------------------------------------------
using TypeId = std::variant<std::monostate, std::int64_t, std::string>;

struct Message {
    std::string name;
    SourceLoc loc;
    std::string doc;
    Format format = Format::Binary;
    Endian endian = Endian::Big;
    BitOrder bit_order = BitOrder::MsbFirst;
    char separator = ',';  // text field separator
    TypeId type_id;
    bool want_view = true;
    bool want_struct = true;
    bool is_header = false;  // synthesised protocol header
    std::vector<FieldPtr> fields;

    // ---- derived ----
    std::optional<std::size_t> static_size;
    std::optional<std::size_t> max_size;
    std::size_t min_size = 0;
    bool fixed_layout = true;      // every field at a static offset with static size
    bool needs_validation = false; // view::from must do more than a size check
    std::size_t conditional_count = 0;
    std::size_t token_count = 0;   // text: number of tokens
    bool layout_done = false;
    bool layout_in_progress = false;
};
using MessagePtr = std::shared_ptr<Message>;

// ------------------------------------------------------------------------------------------------
// Protocols
// ------------------------------------------------------------------------------------------------
struct CrcParams {
    unsigned width = 0;
    std::uint64_t poly = 0, init = 0, xorout = 0;
    bool refin = false, refout = false;
};

struct ChecksumSpec {
    enum class Covers { HeaderBody, Body, All };
    bool present = false;
    SourceLoc loc;
    std::string algo;                  // builtin name, custom name, or "crc" for inline params
    const CustomChecksum* custom = nullptr;
    std::optional<CrcParams> crc;
    std::size_t size = 0;              // bytes (binary) / bytes encoded as hex pairs (text)
    Endian endian = Endian::Big;
    Covers covers = Covers::HeaderBody;
    // text
    char separator = '*';
    bool required = true;
};

struct EscapeSpec {
    bool enabled = false;
    std::uint8_t esc = 0;
    std::uint8_t xor_mask = 0x20;
    std::vector<std::pair<std::uint8_t, std::uint8_t>> map;  // raw -> code (SLIP style)
};

struct FramingSpec {
    enum class Type { None, Fixed, LengthPrefixed, Delimited, Custom };
    enum class LengthCovers { Body, HeaderBody, All };
    Type type = Type::None;
    SourceLoc loc;
    std::vector<std::uint8_t> sync;   // sync word / start delimiter
    std::vector<std::uint8_t> end;    // end delimiter
    EscapeSpec escape;
    std::optional<std::size_t> size;  // fixed frame size
    std::string length_field;         // header field holding the length
    LengthCovers length_covers = LengthCovers::Body;
    std::int64_t length_adjust = 0;   // actual covered length = value + adjust
    std::optional<std::size_t> max_frame;
    const CustomFramer* custom = nullptr;
    std::string custom_name;

    // derived
    const Field* length = nullptr;
};

struct MessageTypeSpec {
    enum class Kind { None, Field, Token };
    Kind kind = Kind::None;
    SourceLoc loc;
    std::string field_name;
    std::size_t token = 0;
    const Field* field = nullptr;  // derived
};

struct Protocol {
    std::string name;
    SourceLoc loc;
    std::string doc;
    Format format = Format::Binary;
    Endian endian = Endian::Big;
    MessagePtr header;  // may have zero fields
    FramingSpec framing;
    ChecksumSpec checksum;
    MessageTypeSpec message_type;
    char separator = ',';                     // text: separator between id token and fields
    std::vector<std::string> message_names;   // declared
    std::vector<const Message*> messages;     // resolved
    std::vector<TypeId> type_ids;          // resolved type id per message (int for a header field, string for a token)

    // ---- derived layout ----
    std::size_t prefix_size = 0;   // sync / start delimiter
    std::size_t header_size = 0;
    std::size_t trailer_size = 0;  // binary checksum
    std::size_t suffix_size = 0;   // end delimiter
    std::size_t max_frame = 0;     // 0 = unknown (stream mode unavailable)
    std::size_t length_field_offset = 0;  // from frame start
};

// ------------------------------------------------------------------------------------------------
// Schema
// ------------------------------------------------------------------------------------------------
struct Schema {
    std::string file;
    std::string stem;  // file name without extension
    int version = 1;
    std::string ns;    // e.g. "acme::telemetry"
    Endian default_endian = Endian::Big;
    BitOrder default_bit_order = BitOrder::MsbFirst;
    Format default_format = Format::Binary;
    char default_separator = ',';
    bool default_view = true;
    bool default_struct = true;

    std::vector<std::shared_ptr<EnumDef>> enums;
    std::vector<std::shared_ptr<CustomType>> custom_types;
    std::vector<std::shared_ptr<CustomChecksum>> custom_checksums;
    std::vector<std::shared_ptr<CustomFramer>> custom_framers;
    std::vector<MessagePtr> messages;  // after lowering: dependency order (nested first)
    std::vector<std::shared_ptr<Protocol>> protocols;

    [[nodiscard]] const EnumDef* find_enum(const std::string& n) const;
    [[nodiscard]] const Message* find_message(const std::string& n) const;
    [[nodiscard]] const CustomType* find_custom_type(const std::string& n) const;
    [[nodiscard]] const CustomChecksum* find_custom_checksum(const std::string& n) const;
    [[nodiscard]] const CustomFramer* find_custom_framer(const std::string& n) const;
};

/// Built-in checksum algorithm table: name -> wire size in bytes.
std::optional<std::size_t> builtin_checksum_size(const std::string& name);
const std::vector<std::string>& builtin_checksum_names();

} // namespace gmpc::ir
