#include "arena/platform/filesystem_path.hpp"

#include <string>

namespace arena::platform {

std::filesystem::path filesystem_api_path(const std::filesystem::path& path) {
    const std::filesystem::path absolute =
        std::filesystem::absolute(path).lexically_normal();
    const std::wstring value = absolute.native();
    if (value.starts_with(L"\\\\?\\")) {
        return absolute;
    }
    if (value.starts_with(L"\\\\")) {
        return std::filesystem::path{L"\\\\?\\UNC\\" + value.substr(2)};
    }
    return std::filesystem::path{L"\\\\?\\" + value};
}

} // namespace arena::platform
