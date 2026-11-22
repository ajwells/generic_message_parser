// Invalid schemas must be rejected with a helpful, located diagnostic.

#include <catch2/catch_test_macros.hpp>

#include <sstream>
#include <string>
#include <vector>

#include "backend/backend.hpp"
#include "ir/lowering.hpp"
#include "schema/loader.hpp"

namespace {

struct Outcome {
    bool ok;
    std::string text;
};

/// Load and lower `yaml`; with `backend` set (e.g. "cpp"), also run that backend for its language-specific checks.
Outcome compile(const std::string& yaml, const char* backend = nullptr) {
    gmpc::Diagnostics d;
    auto s = gmpc::load_schema_string(yaml, "test.yaml", d);
    if (s && gmpc::lower_schema(*s, d) && backend) {
        const auto* b = gmpc::find_backend(backend);
        REQUIRE(b);
        (void)b->generate(*s, {}, d);
    }
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

TEST_CASE("protocol messages: view access and text separator are checked by lowering (any backend)") {
    expect_error(R"(
messages:
  A:
    access: struct
    type_id: 1
    fields: [ { name: x, type: u8 } ]
protocols:
  P:
    header: [ { name: type, type: u8 } ]
    message_type: { field: type }
    messages: [A]
)",
                 "message 'A' needs view access to be part of protocol 'P'");
    expect_error(R"(
messages:
  T:
    format: text
    separator: ';'
    type_id: TT
    fields: [ { name: x, type: u8 } ]
protocols:
  P:
    format: text
    framing: { type: delimited, end: "\n", max_frame: 32 }
    message_type: { token: 0 }
    messages: [T]
)",
                 "message 'T' separator differs from protocol 'P'");
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

TEST_CASE("escape / byte stuffing errors") {
    const std::string head =
        "messages:\n  A:\n    type_id: 1\n    fields: [ { name: x, type: u8 } ]\nprotocols:\n  P:\n"
        "    header: [ { name: kind, type: u8 }, { name: len, type: u8 } ]\n    message_type: { field: kind }\n"
        "    messages: [A]\n";
    const std::string lp = head + "    framing:\n      type: length_prefixed\n      sync: [0x80]\n      length: { field: len }\n";
    auto with = [&](const std::string& escape) { return lp + "      escape: " + escape + "\n"; };

    // valid: index style with the escape char reserved
    {
        const auto r = compile(with("{ char: 0x81, code: index, reserved: [0x80, 0x81] }"));
        INFO(r.text);
        CHECK(r.ok);
        CHECK(r.text.empty());
    }
    expect_error(with("{ char: 0x81, code: index, reserved: [0x80] }"), "must include the escape char 0x81");
    expect_error(with("{ char: 0x81, code: index, reserved: [0x00, 0x81] }"),
                 "escape code 0x00 for 0x00 is itself a reserved byte (use escape.base");
    expect_error(with("{ char: 0x81, code: index, reserved: [0x80, 0x80, 0x81] }"), "lists 0x80 twice");
    expect_error(with("{ char: 0x81, code: index, base: 0x80, reserved: [0x80, 0x81] }"), "is itself a reserved byte");
    expect_error(with("{ char: 0x81, code: index, base: 0xFF, reserved: [0x80, 0x81] }"), "exceeds 0xFF");
    expect_error(with("{ char: 0x81, code: rot13 }"), "escape.code must be xor, index or map");
    expect_error(with("{ char: 0x81, code: index, xor: 0x20 }"), "escape.xor only applies to code: xor");
    expect_error(with("{ char: 0x81, base: 1 }"), "escape.base only applies to code: index");
    expect_error(with("{ char: 0xDB, map: { 0xC0: 0xDC } }"), "must include the escape char 0xDB");
    expect_error(with("{ char: 0xDB, map: { 0xC0: 0xDD, 0xDB: 0xDD } }"), "escape code 0xDD is used for both");
    expect_error(with("{ char: 0xDB, reserved: [0x80], map: { 0xC0: 0xDC, 0xDB: 0xDD } }"), "reserved bytes are the map's keys");
    expect_error(with("{ char: 0x81, code: map }"), "escape code 'map' needs 'map'");
    expect_error(with("{ char: 0x7D, xor: 0x01, reserved: [0x7C] }"), "is itself a reserved byte");
    expect_error(with("{ char: 0x81, code: index, reserved: [0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,0x81], base: 0x20 }"),
                 "at most 16 reserved bytes");
    // a sync byte that is not reserved only warns
    {
        const auto r = compile(with("{ char: 0x81, code: index, reserved: [0x7E, 0x81] }"));
        CHECK(r.ok);
        CHECK(r.text.find("sync byte 0x80 is not reserved") != std::string::npos);
    }
    // delimited: the end byte must be reserved
    expect_error(head + "    framing: { type: delimited, start: [0x80], end: [0x0D], escape: { char: 0x81, code: index, reserved: [0x80, 0x81] } }\n",
                 "end delimiter byte 0x0D must be reserved");
    expect_error(head + "    framing: { type: none, escape: { char: 0x81 } }\n", "'escape' needs delimited, fixed or length_prefixed framing");
    // length counting wire bytes
    const std::string lpw = head + "    framing:\n      type: length_prefixed\n      sync: [0x80]\n";
    const std::string esc = "      escape: { char: 0x81, code: index, reserved: [0x80, 0x81] }\n";
    {
        const auto r = compile(lpw + "      length: { field: len, counts: wire }\n" + esc);
        INFO(r.text);
        CHECK(r.ok);
    }
    expect_error(lpw + "      length: { field: len, counts: wire }\n", "length.counts: wire needs framing.escape");
    expect_error(lpw + "      length: { field: len, covers: all, counts: wire }\n" + esc, "length.counts: wire needs length.covers: body");
    expect_error(lpw + "      length: { field: len, counts: escaped }\n", "length.counts must be unescaped or wire");
    {
        const auto r = compile(lpw + "      length: { field: len, covers: body_checksum, counts: wire }\n" + esc +
                               "    checksum: { algo: crc8, covers: body }\n");
        INFO(r.text);
        CHECK(r.ok);
    }
    expect_error(lpw + "      length: { field: len, covers: body_checksum, counts: wire }\n" + esc + "    checksum: { algo: crc8 }\n",
                 "covers: body_checksum needs checksum.covers: body");
    expect_error(lpw + "      length: { field: len, covers: trailer }\n", "length.covers must be body, body_checksum, header_body or all");
    // checksum over the wire bytes
    expect_error(lp + "    checksum: { algo: crc8, over: wire }\n", "checksum.over: wire needs framing.escape");
    expect_error(with("{ char: 0x81, code: index, reserved: [0x80, 0x81] }") + "    checksum: { algo: crc8, over: escaped }\n",
                 "checksum.over must be unescaped or wire");
}

TEST_CASE("C++ backend: names generated inside a protocol struct are reserved") {
    const std::string proto = R"(
protocols:
  P:
    framing: { type: delimited, end: [0x0A], max_frame: 32 }
    messages: [A]
)";
    const std::string base = "messages:\n  A:\n    fields:\n      - { name: x, type: u8 }\n";
    for (const char* name : {"Callbacks", "CallbackStream", "StreamWith", "Frame", "AnyView"}) {
        const auto r = compile(base + "  " + name + ":\n    fields:\n      - { name: y, type: u8 }\n" + proto, "cpp");
        INFO(r.text);
        CHECK_FALSE(r.ok);
        CHECK(r.text.find("message '" + std::string(name) + "' clashes with a name generated inside protocol 'P'") !=
              std::string::npos);
    }
    const auto ok = compile(base + proto, "cpp");
    INFO(ok.text);
    CHECK(ok.ok);
}

// ---- C backend ---------------------------------------------------------------------------------------

namespace {

void expect_c_error(const std::string& yaml, const std::string& needle) {
    const auto r = compile(yaml, "c");
    INFO("schema:\n" << yaml << "\ndiagnostics:\n" << r.text);
    CHECK_FALSE(r.ok);
    CHECK(r.text.find(needle) != std::string::npos);
}

std::vector<gmpc::OutputFile> generate_c(const std::string& yaml, const gmpc::BackendOptions& opts, std::string& diag_text) {
    gmpc::Diagnostics d;
    auto s = gmpc::load_schema_string(yaml, "test.yaml", d);
    std::vector<gmpc::OutputFile> files;
    if (s && gmpc::lower_schema(*s, d)) files = gmpc::find_backend("c")->generate(*s, opts, d);
    std::ostringstream os;
    d.print(os);
    diag_text = os.str();
    return files;
}

} // namespace

TEST_CASE("C backend: c: entries of custom items, and name clashes") {
    // a custom type used by a generated message needs type / decode (/ encode with a struct)
    expect_c_error(R"(
custom:
  types:
    t: { wire_size: 2, cpp: { type: "int", decode: "d", encode: "e" } }
messages:
  A:
    fields: [ { name: x, type: t } ]
)",
                   "custom type 't' needs a 'c: { type: ... }' entry for the C backend");
    expect_c_error(R"(
custom:
  types:
    t: { wire_size: 2, c: { type: "int", decode: "d" } }
messages:
  A:
    fields: [ { name: x, type: t } ]
)",
                   "custom type 't' needs a 'c: { encode: ... }' entry");
    // custom framer and checksum used by a generated protocol
    expect_c_error(R"(
custom:
  framers:
    f: { cpp: { type: "F" } }
messages:
  A: { fields: [ { name: x, type: u8 } ] }
protocols:
  Q:
    framing: { type: custom, framer: f, max_frame: 16 }
    messages: [A]
)",
                   "custom framer 'f' needs a 'c: { scan: ... }' entry");
    expect_c_error(R"(
custom:
  checksums:
    k: { size: 1, cpp: { function: "k" } }
messages:
  A: { fields: [ { name: x, type: u8 } ] }
protocols:
  P:
    checksum: { algo: k }
    messages: [A]
)",
                   "custom checksum 'k' needs a 'c: { function: ... }' entry");
    // unused custom items need no c: entry; complete entries pass
    const auto ok = compile(R"(
custom:
  types:
    unused: { cpp: { type: "int", decode: "d" } }
    t: { wire_size: 2, c: { header: "codec.h", type: "my_t", decode: "my_t_decode", encode: "my_t_encode" } }
messages:
  A:
    fields: [ { name: x, type: t } ]
)",
                            "c");
    INFO(ok.text);
    CHECK(ok.ok);
    // The bits group Status.flags and the message StatusFlags would both be test_status_flags.
    expect_c_error(R"(
messages:
  Status:
    fields:
      - name: flags
        type: bits
        fields: [ { name: a, bits: 8 } ]
  StatusFlags: { fields: [ { name: y, type: u8 } ] }
)",
                   "generated name 'test_status_flags'");
    // A variable-length field `data` whose length is an expression gets a length member `data_len`.
    expect_c_error(R"(
messages:
  A:
    fields:
      - { name: n, type: u8 }
      - { name: data, type: bytes, length: "n * 2", max: 8 }
      - { name: data_len, type: u8 }
)",
                   "generated C name 'data_len'");
    // A bare length field is the container's size: no extra member, so `name_len` + `name` is fine.
    const auto len = compile(R"(
messages:
  A:
    fields:
      - { name: name_len, type: u8 }
      - { name: name, type: string, length: name_len, max: 8 }
)",
                             "c");
    INFO(len.text);
    CHECK(len.ok);
}

TEST_CASE("C backend: binary and text messages and protocols are all generated") {
    const auto r = compile(R"(
messages:
  A: { type_id: 1, fields: [ { name: x, type: u8 } ] }
  T: { format: text, type_id: T, fields: [ { name: x, type: u8 } ] }
protocols:
  P:
    header: [ { name: id, type: u8 } ]
    message_type: { field: id }
    messages: [A]
  Text:
    format: text
    framing: { type: delimited, end: [0x0A], max_frame: 32 }
    message_type: { token: 0 }
    messages: [T]
)",
                           "c");
    INFO(r.text);
    CHECK(r.ok);
    CHECK(r.text.find("not generated") == std::string::npos);  // nothing is skipped any more
}

TEST_CASE("C backend: names use the namespace, the file stem or --opt prefix") {
    const std::string yaml = "messages:\n  MotorStatus: { fields: [ { name: rpm, type: u16 } ] }\n";
    std::string text;
    auto files = generate_c(yaml, {}, text);
    REQUIRE(files.size() == 2);
    CHECK(files[0].name == "test.gmp.h");
    CHECK(files[1].name == "test.gmp.c");
    CHECK(files[0].content.find("typedef struct test_motor_status {") != std::string::npos);
    CHECK(files[0].content.find("#define TEST_MOTOR_STATUS_WIRE_SIZE 2") != std::string::npos);

    files = generate_c("namespace: acme::drive\n" + yaml, {}, text);
    REQUIRE(files.size() == 2);
    CHECK(files[0].content.find("typedef struct acme_drive_motor_status {") != std::string::npos);

    gmpc::BackendOptions opts;
    opts.values["prefix"] = "md_";
    files = generate_c(yaml, opts, text);
    REQUIRE(files.size() == 2);
    CHECK(files[0].content.find("uint16_t md_motor_status_view_rpm(") != std::string::npos);

    opts.values["prefix"] = "9bad";
    files = generate_c(yaml, opts, text);
    CHECK(files.empty());
    CHECK(text.find("not a valid C identifier prefix") != std::string::npos);
}
