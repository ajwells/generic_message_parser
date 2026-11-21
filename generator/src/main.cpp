// gmpc - generic message parser compiler.
//
//   gmpc [--lang cpp] [--out DIR] [--check] [--opt key=value]... schema.yaml...
//   gmpc --list-backends

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "backend/backend.hpp"
#include "ir/lowering.hpp"
#include "schema/loader.hpp"

namespace {

void usage(std::ostream& os) {
    os << "usage: gmpc [options] schema.yaml...\n"
          "  --lang NAME        output language backend (default: cpp)\n"
          "  --out DIR          output directory (default: current directory)\n"
          "  --check            validate schemas only, write nothing\n"
          "  --stdout           print generated code instead of writing files\n"
          "  --opt KEY=VALUE    backend specific option\n"
          "  --list-backends    list available backends\n"
          "  -h, --help         show this help\n";
}

/// Write only when content changed so build systems do not rebuild needlessly.
bool write_if_changed(const std::filesystem::path& path, const std::string& content) {
    {
        std::ifstream in(path, std::ios::binary);
        if (in) {
            std::ostringstream ss;
            ss << in.rdbuf();
            if (ss.str() == content) return true;
        }
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << content;
    return static_cast<bool>(out);
}

} // namespace

int main(int argc, char** argv) {
    std::string lang = "cpp";
    std::filesystem::path out_dir = ".";
    bool check_only = false, to_stdout = false;
    gmpc::BackendOptions opts;
    std::vector<std::string> inputs;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "gmpc: " << name << " needs a value\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") { usage(std::cout); return 0; }
        if (a == "--list-backends") {
            for (const auto& b : gmpc::backends()) std::cout << b->name() << "\t" << b->description() << "\n";
            return 0;
        }
        if (a == "--lang") lang = value("--lang");
        else if (a == "--out") out_dir = value("--out");
        else if (a == "--check") check_only = true;
        else if (a == "--stdout") to_stdout = true;
        else if (a == "--opt") {
            const auto kv = value("--opt");
            const auto eq = kv.find('=');
            opts.values[kv.substr(0, eq)] = eq == std::string::npos ? "1" : kv.substr(eq + 1);
        } else if (!a.empty() && a[0] == '-') {
            std::cerr << "gmpc: unknown option " << a << "\n";
            usage(std::cerr);
            return 2;
        } else {
            inputs.push_back(a);
        }
    }
    if (inputs.empty()) {
        usage(std::cerr);
        return 2;
    }
    const gmpc::Backend* backend = gmpc::find_backend(lang);
    if (!backend) {
        std::cerr << "gmpc: unknown backend '" << lang << "' (see --list-backends)\n";
        return 2;
    }

    int status = 0;
    for (const auto& input : inputs) {
        gmpc::Diagnostics diag;
        auto schema = gmpc::load_schema_file(input, diag);
        if (schema) gmpc::lower_schema(*schema, diag);
        std::vector<gmpc::OutputFile> files;
        if (schema && !diag.has_errors() && !check_only) files = backend->generate(*schema, opts, diag);
        diag.print(std::cerr);
        if (diag.has_errors()) {
            status = 1;
            continue;
        }
        for (const auto& f : files) {
            if (to_stdout) {
                std::cout << f.content;
                continue;
            }
            std::error_code ec;
            std::filesystem::create_directories(out_dir, ec);
            const auto path = out_dir / f.name;
            if (!write_if_changed(path, f.content)) {
                std::cerr << "gmpc: cannot write " << path.string() << "\n";
                status = 1;
            }
        }
    }
    return status;
}
