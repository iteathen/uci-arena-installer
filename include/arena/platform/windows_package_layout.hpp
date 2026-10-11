#pragma once

#ifdef _WIN32
#include "arena/ports/install_layout.hpp"
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace arena::platform {
// The caller supplies the namespace to the narrow Windows adapter; product
// composition uses the fixed MSI-owned namespace, fixtures use owned namespaces.
[[nodiscard]] std::optional<nlohmann::json> read_windows_package_layout_selection(
    arena::ports::InstallScope scope,
    const std::wstring& registry_base = L"Software\\UCI Arena\\Installer\\PackageLayout");
} // namespace arena::platform
#endif
