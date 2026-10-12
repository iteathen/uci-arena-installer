#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace arena::platform {

// Windows Installer owns all file/registration bookkeeping. These are package
// operations inside SetupEngine's existing transaction, not a coordinator.
[[nodiscard]] nlohmann::json inspect_windows_original_msi(
    const std::filesystem::path& source, const std::string& expected_sha256);
[[nodiscard]] std::vector<nlohmann::json> installed_windows_msi_products(
    const std::string& upgrade_code, bool per_machine);
void retain_windows_original_msi(const std::filesystem::path& source,
                                 const std::filesystem::path& destination,
                                 const std::string& expected_sha256);
void restore_windows_msi_package(const std::filesystem::path& source,
                                 const nlohmann::json& expected_identity,
                                 const std::string& expected_sha256,
                                 const std::filesystem::path& integration_root,
                                 const std::filesystem::path& data_root,
                                 const std::filesystem::path& log_path);

} // namespace arena::platform
