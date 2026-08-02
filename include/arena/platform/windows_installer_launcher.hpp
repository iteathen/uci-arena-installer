#pragma once

#ifdef _WIN32

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "arena/platform/windows_install_scope.hpp"

namespace arena::platform {

inline constexpr std::size_t windows_installer_envelope_footer_size = 128;

struct WindowsInstallerPayload final {
    std::uint64_t offset{};
    std::uint64_t size{};
    std::array<std::byte, 32> sha256{};
};

struct WindowsInstallerEnvelope final {
    std::uint64_t stub_size{};
    WindowsInstallerPayload per_user;
    WindowsInstallerPayload per_machine;
};

enum class WindowsInstallationState {
    none,
    per_user,
    per_machine,
    conflicting,
};

struct WindowsInstallationSnapshot final {
    bool per_user_installed{};
    bool per_machine_installed{};
};

struct WindowsInstallScopePlan final {
    WindowsInstallationState state{WindowsInstallationState::none};
    std::optional<WindowsInstallScope> selected_scope;
    bool selection_required{};
    bool allowed{};
    std::string detail;
};

[[nodiscard]] WindowsInstallerEnvelope
parse_windows_installer_envelope(std::span<const std::byte> bytes);

[[nodiscard]] WindowsInstallScopePlan plan_windows_install_scope(
    const WindowsInstallationSnapshot& snapshot,
    std::optional<WindowsInstallScope> requested_scope);

[[nodiscard]] WindowsInstallationSnapshot
detect_windows_installation();

[[nodiscard]] int run_windows_installer_launcher(
    const std::vector<std::wstring>& arguments);

} // namespace arena::platform

#endif
