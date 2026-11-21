// Invalid schemas must be rejected with a helpful, located diagnostic.

#include <catch2/catch_test_macros.hpp>

#include <sstream>
#include <string>

#include "ir/lowering.hpp"
#include "schema/loader.hpp"

namespace {

struct Outcome {
    bool ok;
    std::string text;
};

Outcome compile(const std::string& yaml) {
    gmpc::Diagnostics d;
    auto s = gmpc::load_schema_string(yaml, "test.yaml", d);
    if (s) gmpc::lower_schema(*s, d);
    std::ostringstream os;
    d.print(os);
    return {!d.has_errors(), os.str()};
}

void expect_error(const std::string& yaml, const std::string& needle) {
    const auto r = compile(yaml);
    INFO("schema:\n" << yaml << "\ndiagnostics:\n" << r.text);
    CHECK_FALSE(r.ok);
    CHECK(r.text.find(needle) != std::string::npos);
}

} // namespace

TEST_CASE("valid minimal schema") {
    const auto r = compile(R"(
messages:
  A:
    fields:
      - { name: x, type: u8 }
)");
    INFO(r.text);
    CHECK(r.ok);
}

TEST_CASE("syntax and structure errors") {
    expect_error("messages: [", "YAML syntax error");
    expect_error("messages:\n  A:\n    feilds: []\n", "unknown key 'feilds'");
    expect_error("schema_version: 2\n", "unsupported schema_version");
    expect_error("namespace: 'a::1b'\n", "namespace 'a::1b'");
}

TEST_CASE("diagnostics carry file and line") {
    const auto r = compile("messages:\n  A:\n    fields:\n      - { name: x, type: nope }\n");
    CHECK(r.text.find("test.yaml:4:") != std::string::npos);
}

TEST_CASE("type errors") {
    expect_error("messages:\n  A:\n    fields:\n      - { name: x, type: nope }\n", "unknown type 'nope'");
    expect_error("messages:\n  A:\n    fields:\n      - { name: x, type: u8 }\n      - { name: x, type: u8 }\n",
                 "duplicate field 'x'");
    expect_error("messages:\n  A:\n    fields:\n      - { name: s, type: string }\n", "needs 'length'");
    expect_error("messages:\n  A:\n    fields:\n      - { name: a, type: array, count: 3 }\n", "needs 'of'");
    expect_error("messages:\n  A:\n    fields:\n      - { name: n, type: u8 }\n      - { name: b, type: bytes, length: n }\n",
                 "needs 'max'");
    expect_error("messages:\n  A:\n    fields:\n      - { name: f, type: f32, const: 1 }\n", "cannot be 'const'");
    expect_error("enums:\n  u8: { values: { a: 1 } }\n", "shadows a built-in type");
    expect_error("enums:\n  E: { values: { a: 1 } }\nmessages:\n  E:\n    fields: []\n", "conflicts");
}

TEST_CASE("bits errors") {
    expect_error(R"(
messages:
  A:
    fields:
      - name: g
        type: bits
        fields: [ { name: a, bits: 3 } ]
)",
                 "add { pad: 5 }");
    expect_error(R"(
messages:
  A:
    fields:
      - name: g
        type: bits
        fields: [ { name: a, bits: 9, type: u8 }, { pad: 7 } ]
)",
                 "too small");
}

TEST_CASE("expression errors") {
    expect_error("messages:\n  A:\n    fields:\n      - { name: x, type: u8, when: 'y > 1' }\n      - { name: y, type: u8 }\n",
                 "does not name an earlier field");
    expect_error(R"(
messages:
  A:
    fields:
      - { name: c, type: u8 }
      - { name: o, type: u8, when: "c == 1" }
      - { name: p, type: u8, when: "o == 1" }
)",
                 "cannot reference conditional field");
    expect_error("enums:\n  E: { values: { a: 1 } }\nmessages:\n  A:\n    fields:\n      - { name: x, type: u8, when: 'E.b' }\n",
                 "has no value 'b'");
    expect_error("messages:\n  A:\n    fields:\n      - { name: x, type: u8, when: 'x +' }\n", "invalid expression");
}

TEST_CASE("text message restrictions") {
    expect_error("messages:\n  T:\n    format: text\n    fields:\n      - { name: b, type: bytes, length: 2 }\n",
                 "not supported in text messages");
    expect_error("messages:\n  T:\n    format: text\n    fields:\n      - { name: s, type: string }\n", "needs 'max'");
    expect_error("messages:\n  B:\n    fields:\n      - { name: x, type: u8, optional: true }\n", "applies to text messages");
}

TEST_CASE("protocol errors") {
    const std::string msgs = R"(
messages:
  A:
    type_id: 1
    fields: [ { name: x, type: u8 } ]
  B:
    type_id: 1
    fields: [ { name: y, type: u16 } ]
)";
    expect_error(msgs + "protocols:\n  P:\n    messages: [A, B]\n", "needs 'message_type'");
    expect_error(msgs + R"(
protocols:
  P:
    header: [ { name: id, type: u8 } ]
    message_type: { field: id }
    messages: [A, B]
)",
                 "duplicate type_id 1");
    expect_error(msgs + R"(
protocols:
  P:
    header: [ { name: id, type: u8 } ]
    message_type: { field: nope }
    messages: [A]
)",
                 "is not a header field");
    expect_error(msgs + R"(
protocols:
  P:
    header: [ { name: len, type: i16 } ]
    framing: { type: length_prefixed, sync: [0xAA], length: { field: len } }
    messages: [A]
)",
                 "length field must be an unsigned integer");
    expect_error(msgs + R"(
protocols:
  P:
    framing: { type: delimited, start: "$" }
    messages: [A]
)",
                 "needs 'end'");
    expect_error(msgs + R"(
protocols:
  P:
    checksum: { algo: crc99 }
    messages: [A]
)",
                 "unknown checksum algo 'crc99'");
    expect_error(msgs + "protocols:\n  P:\n    messages: [Nope]\n", "unknown message 'Nope'");
}

TEST_CASE("nested access consistency") {
    expect_error(R"(
messages:
  Inner:
    access: struct
    fields: [ { name: x, type: u8 } ]
  Outer:
    fields: [ { name: i, type: Inner } ]
)",
                 "needs view access too");
}

TEST_CASE("message_type / type_id; the old names are rejected with a hint") {
    CHECK(compile(R"(
messages:
  A: { type_id: 1, fields: [ { name: x, type: u8 } ] }
  B: { type_id: 2, fields: [ { name: y, type: u8 } ] }
protocols:
  P:
    header: [ { name: kind, type: u8 } ]
    message_type: { field: kind }
    messages: [A, B]
)")
              .ok);
    expect_error("messages:\n  A: { id: 1, fields: [ { name: x, type: u8 } ] }\n",
                 "unknown key 'id' in message 'A' ('id' was renamed to 'type_id')");
    expect_error(R"(
messages:
  A: { type_id: 1, fields: [ { name: x, type: u8 } ] }
protocols:
  P:
    header: [ { name: kind, type: u8 } ]
    dispatch: { field: kind }
    messages: [A]
)",
                 "unknown key 'dispatch' in protocol 'P' ('dispatch' was renamed to 'message_type')");
    // a field named with a key that was never renamed in that context gets no hint
    const auto r = compile("messages:\n  A:\n    fields: [ { name: x, type: u8, id: 3 } ]\n");
    CHECK_FALSE(r.ok);
    CHECK(r.text.find("was renamed") == std::string::npos);
}

TEST_CASE("field crc errors") {
    expect_error("messages:\n  A:\n    fields:\n      - { name: x, type: u16, crc: crc99 }\n", "unknown checksum algo 'crc99'");
    expect_error("messages:\n  A:\n    fields:\n      - { name: b, type: bytes, length: 2, crc: crc8 }\n",
                 "'crc' is only valid on scalar and enum fields");
    expect_error("messages:\n  A:\n    fields:\n      - { name: a, type: array, count: 2, of: { type: u16, crc: crc8 } }\n",
                 "'crc' is not supported on array elements");
    expect_error("messages:\n  T:\n    format: text\n    fields:\n      - { name: x, type: u16, crc: crc8 }\n",
                 "'crc' is not supported in text messages");
    expect_error("messages:\n  A:\n    fields:\n      - { name: x, type: u16, crc: { crc: { width: 70, poly: 1 } } }\n",
                 "crc width must be 1..64");
}

TEST_CASE("conversion errors") {
    expect_error("messages:\n  A:\n    fields:\n      - { name: x, type: u16, conversions: { f: { unit: degF } } }\n",
                 "conversion 'f' needs 'scale' and/or 'offset'");
    expect_error("messages:\n  A:\n    fields:\n      - { name: x, type: u16, conversions: { scaled: { scale: 2 } } }\n",
                 "conversion name 'scaled' is reserved");
    expect_error("messages:\n  A:\n    fields:\n      - { name: x, type: u16, conversions: { 2x: { scale: 2 } } }\n",
                 "conversion name '2x' is not a valid identifier");
    expect_error(
        "messages:\n  A:\n    fields:\n      - { name: x, type: u16, conversions: { y: { scale: 2 } } }\n      - { name: x_y, type: u8 }\n",
        "conversion accessor 'x_y' clashes with a field");
    expect_error("enums:\n  E: { values: { a: 1 } }\nmessages:\n  A:\n    fields:\n      - { name: e, type: E, conversions: { y: { scale: 2 } } }\n",
                 "'conversions' are only valid on numeric scalar fields");
    expect_error("messages:\n  A:\n    fields:\n      - { name: x, type: u16, conversions: [1] }\n",
                 "'conversions' must be a mapping");
}

TEST_CASE("computed value errors") {
    const std::string m = "messages:\n  A:\n    fields:\n      - { name: x, type: u8 }\n      - { name: y, type: u8, when: 'x == 1' }\n    computed:\n";
    expect_error(m + "      c: { type: u32 }\n", "needs a 'value' expression");
    expect_error(m + "      c: { value: x }\n", "needs a 'type'");
    expect_error(m + "      c: { type: bool, value: x }\n", "type must be numeric");
    expect_error(m + "      c: { type: u8, value: 'nope + 1' }\n", "does not name an earlier field");
    expect_error(m + "      c: { type: u8, value: y }\n", "cannot reference conditional field");
    expect_error(m + "      x: { type: u8, value: x }\n", "clashes with a field or accessor");
    expect_error(m + "      c: { type: u8, value: x, set: { nope: value } }\n", "set target 'nope' is not a field");
    expect_error(m + "      c: { type: u8, value: x, set: { y: value } }\n", "must be a scalar or enum field that is not const, conditional");
    expect_error(m + "      c: { type: u8, value: x, set: { x: 'value +' } }\n", "invalid expression");
    expect_error("messages:\n  T:\n    format: text\n    fields: [ { name: x, type: u8 } ]\n    computed:\n      c: { type: u8, value: x }\n",
                 "only supported in binary messages");
    // `value` only means the input inside `set`
    expect_error(m + "      c: { type: u8, value: value }\n", "does not name an earlier field");
}
