// gmpc for WebAssembly (Emscripten): the generator as a function, for Schema Studio's exact decode engine.
//
//   const char* gmpc_generate(const char* yaml, const char* file, const char* lang, const char* options)
//
// lang: a backend name ("js", "cpp", "c"), or "" to validate only. options: "key=value" lines (--opt).
// Returns JSON (valid until the next call):
//   { "ok": bool, "files": [{ "name", "content" }], "diagnostics": [{ "level", "file", "line", "col", "message" }] }
// gmpc_version() returns the stamp of the generator sources the module was built from.

#include <cstdio>
#include <exception>
#include <sstream>
#include <string>

#include "backend/backend.hpp"
#include "ir/lowering.hpp"
#include "schema/loader.hpp"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#define GMPC_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define GMPC_EXPORT
#endif

#ifndef GMPC_SOURCE_STAMP
#define GMPC_SOURCE_STAMP "unknown"
#endif

namespace {

std::string json_string(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof b, "\\u%04x", c);
                    o += b;
                } else {
                    o += static_cast<char>(c);  // UTF-8 passes through
                }
        }
    }
    return o + "\"";
}

std::string result;

} // namespace

extern "C" {

GMPC_EXPORT const char* gmpc_generate(const char* yaml, const char* file, const char* lang, const char* options) {
    gmpc::Diagnostics diag;
    std::vector<gmpc::OutputFile> files;
    const std::string file_name = file && *file ? file : "schema.yaml";
    try {
        gmpc::BackendOptions opts;
        std::istringstream os(options ? options : "");
        for (std::string line; std::getline(os, line);) {
            if (line.empty()) continue;
            const auto eq = line.find('=');
            opts.values[line.substr(0, eq)] = eq == std::string::npos ? "1" : line.substr(eq + 1);
        }
        auto schema = gmpc::load_schema_string(yaml ? yaml : "", file_name, diag);
        if (schema) gmpc::lower_schema(*schema, diag);
        if (schema && !diag.has_errors() && lang && *lang) {
            const auto* backend = gmpc::find_backend(lang);
            if (!backend) diag.error({file_name, 0, 0}, std::string("unknown backend '") + lang + "'");
            else files = backend->generate(*schema, opts, diag);
        }
    } catch (const gmpc::FatalError&) {
        // already recorded
    } catch (const std::exception& e) {
        diag.error({file_name, 0, 0}, std::string("internal error: ") + e.what());
    }
    result = std::string("{\"ok\":") + (diag.has_errors() ? "false" : "true") + ",\"files\":[";
    for (std::size_t i = 0; i < files.size(); ++i)
        result += (i ? "," : "") + std::string("{\"name\":") + json_string(files[i].name) + ",\"content\":" +
                  json_string(files[i].content) + "}";
    result += "],\"diagnostics\":[";
    const auto& items = diag.items();
    for (std::size_t i = 0; i < items.size(); ++i) {
        const auto& d = items[i];
        result += (i ? "," : "") + std::string("{\"level\":") +
                  (d.level == gmpc::Diagnostic::Level::Error ? "\"error\"" : "\"warning\"") + ",\"file\":" +
                  json_string(d.loc.file) + ",\"line\":" + std::to_string(d.loc.line) + ",\"col\":" +
                  std::to_string(d.loc.col) + ",\"message\":" + json_string(d.message) + "}";
    }
    result += "]}";
    return result.c_str();
}

GMPC_EXPORT const char* gmpc_version() { return GMPC_SOURCE_STAMP; }

}  // extern "C"
