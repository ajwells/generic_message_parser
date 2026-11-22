#pragma once

#include <memory>

#include "backend/backend.hpp"

namespace gmpc {

std::unique_ptr<Backend> make_c_backend();

} // namespace gmpc
