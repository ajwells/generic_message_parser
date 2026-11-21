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
    id: 1
    fields: [ { name: x, type: u8 } ]
  B:
    id: 1
    fields: [ { name: y, type: u16 } ]
)";
    expect_error(msgs + "protocols:\n  P:\n    messages: [A, B]\n", "needs 'dispatch'");
    expect_error(msgs + R"(
protocols:
  P:
    header: [ { name: id, type: u8 } ]
    dispatch: { field: id }
    messages: [A, B]
)",
                 "duplicate message id 1");
    expect_error(msgs + R"(
protocols:
  P:
    header: [ { name: id, type: u8 } ]
    dispatch: { field: nope }
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
