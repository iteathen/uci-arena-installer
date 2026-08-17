#pragma once

#include "arena/ports/archive_extraction.hpp"
#include "arena/ports/filesystem_identity.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <stop_token>

namespace arena::application::installer {

struct ArchiveExpansionRequest {
    std::filesystem::path destination_root;
    std::uint64_t expected_expanded_file_bytes{};
    arena::ports::ArchiveTargetPlatform target_platform{};
    std::chrono::milliseconds timeout{std::chrono::hours{24}};
    std::stop_token stop_token;
};

struct ExpandedArchive {
    std::filesystem::path root;
    std::uint64_t file_count{};
    std::uint64_t directory_count{};
    std::uint64_t total_file_bytes{};
};

class ArchiveExpander final {
  public:
    explicit ArchiveExpander(
        arena::ports::ArchiveExtractionPlatform& extraction_platform);

    [[nodiscard]] ExpandedArchive expand(
        arena::ports::AdmittedRegularFile& archive_file,
        const ArchiveExpansionRequest& request) const;

  private:
    arena::ports::ArchiveExtractionPlatform& extraction_platform_;
};

} // namespace arena::application::installer
