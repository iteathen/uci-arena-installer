#pragma once

#include "arena/ports/install_layout.hpp"

#include <cstdint>
#include <filesystem>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <string_view>

namespace arena::application::installer {

struct SetupPaths;

struct InstallLayoutRoots {
    std::filesystem::path integration;
    std::filesystem::path data;
    std::filesystem::path install;
    std::filesystem::path cache;
    std::filesystem::path workspace;
    std::filesystem::path knowledge;
    std::filesystem::path release;
    std::filesystem::path receipts;
    std::filesystem::path transactions;
    std::filesystem::path preservation;

    bool operator==(const InstallLayoutRoots&) const = default;
};

struct InstallLayoutStablePaths {
    std::filesystem::path policy;
    std::filesystem::path descriptor;
    std::filesystem::path scope_marker;
    std::filesystem::path setup_launcher;
    std::filesystem::path installer_runtime;
    std::filesystem::path path_directory;

    bool operator==(const InstallLayoutStablePaths&) const = default;
};

struct ResolvedInstallLayout {
    std::string policy_id;
    std::uint64_t policy_version{};
    arena::ports::InstallScope scope{};
    InstallLayoutRoots roots;
    InstallLayoutStablePaths stable_paths;

    bool operator==(const ResolvedInstallLayout&) const = default;
};

struct InstalledLayout {
    std::string installation_id;
    std::uint64_t layout_generation{};
    std::string policy_sha256;
    ResolvedInstallLayout resolved;

    bool operator==(const InstalledLayout&) const = default;
};

[[nodiscard]] nlohmann::json
validate_install_layout_policy(const nlohmann::json& candidate);

[[nodiscard]] ResolvedInstallLayout
resolve_install_layout(const nlohmann::json& validated_policy,
                       arena::ports::InstallScope scope,
                       const arena::ports::InstallLayoutAnchorResolver& anchors);

[[nodiscard]] InstalledLayout
validate_installed_layout(const nlohmann::json& validated_policy,
                          std::string_view policy_sha256,
                          const nlohmann::json& descriptor,
                          const arena::ports::InstallLayoutAnchorResolver& anchors);

[[nodiscard]] arena::ports::InstallLayoutLocatorRecord
locator_from_installed_layout(const InstalledLayout& layout,
                              std::string_view descriptor_sha256);

[[nodiscard]] SetupPaths
setup_paths_from_layout(const InstalledLayout& layout,
                        std::string bundle_owner,
                        bool restart_manager_service,
                        bool manager_service_access);

}  // namespace arena::application::installer
