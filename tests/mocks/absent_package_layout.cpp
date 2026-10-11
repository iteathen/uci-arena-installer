#include "arena/platform/windows_package_layout.hpp"

namespace arena::platform {
std::optional<nlohmann::json> read_windows_package_layout_selection(
    arena::ports::InstallScope, const std::wstring&) {
    return std::nullopt;
}
} // namespace arena::platform
