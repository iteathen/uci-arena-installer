#pragma once

#include <string_view>

namespace arena::application::installer {

enum class InstallerExitCode : int {
    success = 0,
    invalid_invocation = 2,
    invalid_manifest = 3,
    invalid_configuration = 4,
    failed_preflight = 5,
    unsupported_state = 6,
    package_validation_failed = 7,
    dependency_failed = 8,
    locked_files = 9,
    staging_failed = 10,
    deployment_failed = 11,
    post_install_validation_failed = 12,
    rollback_failed = 13,
    internal_failure = 14,
    operator_cancelled = 15,
};

[[nodiscard]] constexpr std::string_view
installer_exit_category(const InstallerExitCode code) noexcept {
    switch (code) {
    case InstallerExitCode::success:
        return "success";
    case InstallerExitCode::invalid_invocation:
        return "invalid_invocation";
    case InstallerExitCode::invalid_manifest:
        return "invalid_manifest";
    case InstallerExitCode::invalid_configuration:
        return "invalid_configuration";
    case InstallerExitCode::failed_preflight:
        return "failed_preflight";
    case InstallerExitCode::unsupported_state:
        return "unsupported_state";
    case InstallerExitCode::package_validation_failed:
        return "package_validation_failed";
    case InstallerExitCode::dependency_failed:
        return "dependency_failed";
    case InstallerExitCode::locked_files:
        return "locked_files";
    case InstallerExitCode::staging_failed:
        return "staging_failed";
    case InstallerExitCode::deployment_failed:
        return "deployment_failed";
    case InstallerExitCode::post_install_validation_failed:
        return "post_install_validation_failed";
    case InstallerExitCode::rollback_failed:
        return "rollback_failed";
    case InstallerExitCode::internal_failure:
        return "internal_failure";
    case InstallerExitCode::operator_cancelled:
        return "operator_cancelled";
    }
    return "internal_failure";
}

} // namespace arena::application::installer
