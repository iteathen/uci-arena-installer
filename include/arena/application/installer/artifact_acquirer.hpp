#pragma once

#include "arena/ports/artifact_cache.hpp"
#include "arena/ports/filesystem_identity.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace arena::application::installer {

struct ArtifactAcquisitionPolicy {
    std::chrono::milliseconds cache_lock_timeout{std::chrono::seconds{30}};
    std::chrono::milliseconds connect_timeout{std::chrono::seconds{15}};
    std::chrono::milliseconds overall_timeout{std::chrono::hours{168}};
};

struct ArtifactAcquisitionRequest {
    std::vector<std::string> sources;
    std::string expected_sha256;
    std::uint64_t expected_size_bytes{};
    std::filesystem::path catalog_directory;
    std::filesystem::path cache_root;
    bool offline{false};
    std::stop_token stop_token;
    ArtifactAcquisitionPolicy policy;
};

struct AcquiredArtifact {
    std::filesystem::path cache_path;
    std::uint64_t size_bytes{};
    std::string sha256;
    std::unique_ptr<arena::ports::AdmittedRegularFile> file;

    AcquiredArtifact(AcquiredArtifact&&) noexcept = default;
    AcquiredArtifact& operator=(AcquiredArtifact&&) noexcept = default;
    AcquiredArtifact(const AcquiredArtifact&) = delete;
    AcquiredArtifact& operator=(const AcquiredArtifact&) = delete;

  private:
    friend class ArtifactAcquirer;

    AcquiredArtifact(
        std::filesystem::path cache_path_value,
        std::uint64_t size_bytes_value,
        std::string sha256_value,
        std::unique_ptr<arena::ports::AdmittedRegularFile> file_value)
        : cache_path(std::move(cache_path_value)),
          size_bytes(size_bytes_value),
          sha256(std::move(sha256_value)),
          file(std::move(file_value)) {}
};

class ArtifactAcquirer final {
  public:
    ArtifactAcquirer(
        arena::ports::FilesystemIdentity& filesystem_identity,
        arena::ports::ArtifactCachePlatform& cache_platform);

    [[nodiscard]] AcquiredArtifact acquire(
        const ArtifactAcquisitionRequest& request) const;

  private:
    arena::ports::FilesystemIdentity& filesystem_identity_;
    arena::ports::ArtifactCachePlatform& cache_platform_;
};

} // namespace arena::application::installer
