#pragma once

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>

namespace arena::platform {

enum class WindowsInstallScope {
    per_user,
    per_machine,
};

inline WindowsInstallScope windows_install_scope_from_executable(
    const std::filesystem::path& executable) {
    if (executable.empty()) {
        return WindowsInstallScope::per_machine;
    }

    constexpr std::size_t marker_ancestor_limit = 4;
    std::filesystem::path directory = executable.parent_path();
    for (std::size_t depth = 0;
         depth < marker_ancestor_limit && !directory.empty(); ++depth) {
        const std::filesystem::path marker =
            directory / L"share" / L"uci-arena" / L"install-scope.txt";
        std::error_code status_error;
        const std::filesystem::file_status marker_status =
            std::filesystem::symlink_status(marker, status_error);
        if (status_error) {
            if (status_error == std::errc::no_such_file_or_directory) {
                const std::filesystem::path parent = directory.parent_path();
                if (parent == directory) {
                    break;
                }
                directory = parent;
                continue;
            }
            return WindowsInstallScope::per_machine;
        }
        if (std::filesystem::exists(marker_status)) {
            if (!std::filesystem::is_regular_file(marker_status)) {
                return WindowsInstallScope::per_machine;
            }
            std::ifstream input(marker, std::ios::binary);
            std::string value;
            if (!input || !std::getline(input, value)) {
                return WindowsInstallScope::per_machine;
            }
            if (!value.empty() && value.back() == '\r') {
                value.pop_back();
            }
            if (value == "per-user") {
                return WindowsInstallScope::per_user;
            }
            return WindowsInstallScope::per_machine;
        }

        const std::filesystem::path parent = directory.parent_path();
        if (parent == directory) {
            break;
        }
        directory = parent;
    }
    return WindowsInstallScope::per_machine;
}

inline std::filesystem::path windows_executable_path() {
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return {};
    }
    buffer.resize(length);
    return std::filesystem::path(buffer);
}

inline WindowsInstallScope windows_install_scope() {
    return windows_install_scope_from_executable(windows_executable_path());
}

inline bool is_windows_per_user_install() {
    return windows_install_scope() == WindowsInstallScope::per_user;
}

} // namespace arena::platform

#endif
