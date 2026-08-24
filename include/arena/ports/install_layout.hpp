#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace arena::ports {

enum class InstallScope { per_user, per_machine };

enum class LayoutAnchor {
    user_local_app_data,
    machine_program_files,
    machine_program_data,
};

class InstallLayoutAnchorResolver {
  public:
    virtual ~InstallLayoutAnchorResolver() = default;
    [[nodiscard]] virtual std::filesystem::path
    resolve(LayoutAnchor anchor) const = 0;
};

struct InstallLayoutLocatorRecord {
    std::string schema{"arena_install_layout_locator_v1"};
    InstallScope scope{};
    std::string installation_id;
    std::uint64_t layout_generation{};
    std::filesystem::path integration_root;
    std::filesystem::path policy_path;
    std::string policy_sha256;
    std::filesystem::path descriptor_path;
    std::string descriptor_sha256;
    std::filesystem::path setup_launcher_path;

    bool operator==(const InstallLayoutLocatorRecord&) const = default;
};

class InstallLayoutLocator {
  public:
    virtual ~InstallLayoutLocator() = default;
    [[nodiscard]] virtual std::optional<InstallLayoutLocatorRecord>
    read(InstallScope scope) const = 0;
};

}  // namespace arena::ports
