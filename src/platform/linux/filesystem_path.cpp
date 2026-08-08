#include "arena/platform/filesystem_path.hpp"

namespace arena::platform {

std::filesystem::path filesystem_api_path(const std::filesystem::path& path) {
    return path;
}

} // namespace arena::platform
