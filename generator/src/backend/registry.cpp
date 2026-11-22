#include "backend/backend.hpp"
#include "backend/c/c_backend.hpp"
#include "backend/cpp/cpp_backend.hpp"

namespace gmpc {

const std::vector<std::unique_ptr<Backend>>& backends() {
    static const std::vector<std::unique_ptr<Backend>> list = [] {
        std::vector<std::unique_ptr<Backend>> v;
        v.push_back(make_cpp_backend());
        v.push_back(make_c_backend());
        // Register additional language backends here.
        return v;
    }();
    return list;
}

const Backend* find_backend(std::string_view name) {
    for (const auto& b : backends())
        if (b->name() == name) return b.get();
    return nullptr;
}

} // namespace gmpc
