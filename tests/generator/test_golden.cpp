// Golden-file tests: generated code for each schema must match the checked-in expected output.
// To accept intentional changes: set GMP_UPDATE_GOLDEN=1 and re-run, then review the diff.

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

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

std::string generate(const fs::path& schema) {
    gmpc::Diagnostics diag;
    auto s = gmpc::load_schema_file(schema.string(), diag);
    REQUIRE(s);
    REQUIRE(gmpc::lower_schema(*s, diag));
    auto files = gmpc::find_backend("cpp")->generate(*s, {}, diag);
    std::ostringstream os;
    diag.print(os);
    INFO(os.str());
    REQUIRE_FALSE(diag.has_errors());
    REQUIRE(files.size() == 1);
    // The header records only the schema's file name, so output is independent of checkout location.
    return files[0].content;
}

} // namespace

TEST_CASE("generated C++ matches golden files") {
    const bool update = std::getenv("GMP_UPDATE_GOLDEN") != nullptr;
    for (const char* name : {"telemetry", "nmea", "hdlc", "can", "custom"}) {
        INFO("schema: " << name);
        const auto out = generate(fs::path(GMP_SCHEMA_DIR) / (std::string(name) + ".yaml"));
        const auto golden = fs::path(GMP_GOLDEN_DIR) / (std::string(name) + ".gmp.hpp");
        if (update) {
            fs::create_directories(golden.parent_path());
            std::ofstream(golden, std::ios::binary) << out;
            continue;
        }
        REQUIRE(fs::exists(golden));
        CHECK(out == read_file(golden));
    }
}

TEST_CASE("generation is deterministic") {
    const auto p = fs::path(GMP_SCHEMA_DIR) / "telemetry.yaml";
    CHECK(generate(p) == generate(p));
}
