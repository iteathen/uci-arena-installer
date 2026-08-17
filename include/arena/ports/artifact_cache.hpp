#pragma once

#include "arena/ports/filesystem_identity.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace arena::ports {

enum class ArtifactCacheActivationOutcome {
    activated,
    target_occupied,
    activated_durability_unproven,
    indeterminate,
};

struct ArtifactCacheDiagnostic {
    std::string code;
    std::string message;
};

struct ArtifactCacheActivationResult {
    ArtifactCacheActivationOutcome outcome{};
    std::optional<ArtifactCacheDiagnostic> diagnostic;
};

class ArtifactCacheCandidate {
  public:
    virtual ~ArtifactCacheCandidate() = default;

    [[nodiscard]] virtual const std::filesystem::path& path() const noexcept = 0;
    virtual void write(std::span<const std::byte> bytes) = 0;
    virtual void finish() = 0;
    [[nodiscard]] virtual std::optional<ArtifactCacheDiagnostic>
    cleanup() noexcept = 0;
};

class ArtifactCacheLease {
  public:
    virtual ~ArtifactCacheLease() = default;

    [[nodiscard]] virtual const std::filesystem::path&
    target_path() const noexcept = 0;
    [[nodiscard]] virtual std::unique_ptr<ArtifactCacheCandidate>
    create_candidate(
        std::string_view random_id,
        std::uint64_t expected_size_bytes) = 0;
    virtual void discard_exact_target(
        const FilesystemIdentityToken& expected_object_identity,
        std::string_view random_quarantine_id) = 0;
    [[nodiscard]] virtual ArtifactCacheActivationResult activate_candidate(
        ArtifactCacheCandidate& candidate,
        const FilesystemIdentityToken& expected_candidate_identity) = 0;
};

class ArtifactCachePlatform {
  public:
    virtual ~ArtifactCachePlatform() = default;

    [[nodiscard]] std::unique_ptr<ArtifactCacheLease> acquire_digest_lease(
        const std::filesystem::path& cache_root,
        std::string_view expected_sha256,
        std::chrono::milliseconds timeout);

  protected:
    [[nodiscard]] virtual std::unique_ptr<ArtifactCacheLease>
    acquire_digest_lease_validated(
        const std::filesystem::path& cache_root,
        std::string_view expected_sha256,
        std::chrono::steady_clock::time_point deadline) = 0;
};

} // namespace arena::ports
