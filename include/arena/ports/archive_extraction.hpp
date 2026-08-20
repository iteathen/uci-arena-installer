#pragma once

#include "arena/ports/filesystem_identity.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace arena::ports {

enum class ArchiveTargetPlatform { windows_x86_64, linux_x86_64 };

using ArchivePathCollisionKey = std::array<std::byte, 32>;

struct ArchiveExtractionDiagnostic {
    std::string code;
    std::string message;
};

struct ArchiveTargetPath {
    std::string portable_relative_path;
    ArchivePathCollisionKey collision_key;
    std::size_t depth{};
};

class ArchiveOutputFile {
  public:
    virtual ~ArchiveOutputFile() = default;
    virtual void write(std::span<const std::byte> bytes) = 0;
    virtual void finish() = 0;
};

class ArchiveExtractionSession {
  public:
    virtual ~ArchiveExtractionSession() = default;
    [[nodiscard]] virtual const std::filesystem::path& root() const noexcept = 0;
    [[nodiscard]] virtual ArchiveTargetPath validate_path(
        std::string_view portable_relative_path) const = 0;
    virtual void ensure_directory(const ArchiveTargetPath& path) = 0;
    [[nodiscard]] virtual std::unique_ptr<ArchiveOutputFile> create_file(
        const ArchiveTargetPath& path, std::uint64_t expected_size_bytes,
        bool executable) = 0;
    virtual void finish() = 0;
    [[nodiscard]] virtual std::optional<ArchiveExtractionDiagnostic>
    cleanup() noexcept = 0;
};

class ArchiveExtractionPlatform {
  public:
    virtual ~ArchiveExtractionPlatform() = default;
    [[nodiscard]] std::unique_ptr<ArchiveExtractionSession> begin(
        const std::filesystem::path& destination_root,
        ArchiveTargetPlatform target_platform,
        std::chrono::milliseconds timeout);

  protected:
    [[nodiscard]] virtual std::unique_ptr<ArchiveExtractionSession>
    begin_validated(const std::filesystem::path& destination_root,
                    ArchiveTargetPlatform target_platform,
                    std::chrono::steady_clock::time_point deadline) = 0;
};

} // namespace arena::ports
