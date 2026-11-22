// Golden-file tests: generated code for each schema must match the checked-in expected output.
// To accept intentional changes: set GMP_UPDATE_GOLDEN=1 and re-run, then review the diff.
//
// Goldens live in golden/<backend>/<output file name>, one directory per language backend. Every file a
// backend emits is compared, so backends that write several files (e.g. a header and a source) are covered.

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <set>
#include <utility>
#include <vector>

#include "backend/backend.hpp"
#include "ir/lowering.hpp"
#include "schema/loader.hpp"

namespace fs = std::filesystem;

namespace {

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::vector<gmpc::OutputFile> generate(const fs::path& schema, const char* lang) {
    gmpc::Diagnostics diag;
    auto s = gmpc::load_schema_file(schema.string(), diag);
    REQUIRE(s);
    REQUIRE(gmpc::lower_schema(*s, diag));
    const auto* backend = gmpc::find_backend(lang);
    REQUIRE(backend);
    auto files = backend->generate(*s, {}, diag);
    std::ostringstream os;
    diag.print(os);
    INFO(os.str());
    REQUIRE_FALSE(diag.has_errors());
    REQUIRE_FALSE(files.empty());
    // Output records only the schema's file name, so it is independent of the checkout location.
    return files;
}

struct SchemaCase {
    fs::path dir;
    const char* name;
};

/// Schemas covered by every backend's goldens.
std::vector<SchemaCase> schema_cases() {
    const auto schemas = fs::path(GMP_SCHEMA_DIR), examples = fs::path(GMP_EXAMPLES_DIR);
    return {
        {schemas, "telemetry"}, {schemas, "nmea"}, {schemas, "hdlc"}, {schemas, "can"},
        {schemas, "custom"}, {schemas, "fieldcrc"}, {schemas, "enums"}, {schemas, "computed"},
        {schemas, "stuffed"}, {schemas, "framing"}, {examples / "sensirion_sht4x", "sht4x"}, {examples / "sensirion_sht4x", "sht4x_enum"},
        {examples / "reserved_sync", "motor_link"},
    };
}

/// Compare (or with GMP_UPDATE_GOLDEN, rewrite) every output file of backend `lang` for every schema case,
/// except schemas in `skip` (features the backend does not support yet).
void check_goldens(const char* lang, const std::set<std::string>& skip = {}) {
    const bool update = std::getenv("GMP_UPDATE_GOLDEN") != nullptr;
    const auto golden_dir = fs::path(GMP_GOLDEN_DIR) / lang;
    for (const auto& [dir, name] : schema_cases()) {
        if (skip.contains(name)) continue;
        INFO("backend: " << lang << ", schema: " << name);
        for (const auto& file : generate(dir / (std::string(name) + ".yaml"), lang)) {
            INFO("file: " << file.name);
            const auto golden = golden_dir / file.name;
            if (update) {
                fs::create_directories(golden.parent_path());
                std::ofstream(golden, std::ios::binary) << file.content;
                continue;
            }
            REQUIRE(fs::exists(golden));
            CHECK(file.content == read_file(golden));
        }
    }
}

} // namespace

TEST_CASE("generated C++ matches golden files") { check_goldens("cpp"); }

TEST_CASE("generated C matches golden files") { check_goldens("c"); }

TEST_CASE("generation is deterministic") {
    const auto p = fs::path(GMP_SCHEMA_DIR) / "telemetry.yaml";
    for (const auto& b : gmpc::backends()) {
        INFO("backend: " << b->name());
        const auto a = generate(p, std::string(b->name()).c_str()), c = generate(p, std::string(b->name()).c_str());
        REQUIRE(a.size() == c.size());
        for (std::size_t i = 0; i < a.size(); ++i) {
            CHECK(a[i].name == c[i].name);
            CHECK(a[i].content == c[i].content);
        }
    }
}
