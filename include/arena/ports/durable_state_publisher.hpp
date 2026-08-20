#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace arena::ports {

enum class PublicationPrecondition { absent, exact_sha256 };

enum class PublicationOutcome {
    activated,
    not_activated,
    activated_durability_unproven,
    indeterminate,
};

inline constexpr std::size_t publication_path_max_utf8_bytes = 131072;
inline constexpr std::size_t publication_diagnostic_code_max_bytes = 64;
inline constexpr std::size_t publication_diagnostic_message_max_utf8_bytes = 512;
inline constexpr std::size_t publication_sha256_hex_bytes = 64;
inline constexpr std::uint64_t publication_global_max_bytes = 67108864;
inline constexpr std::size_t publication_cleanup_diagnostics_max_count = 16;
inline constexpr auto publication_timeout_minimum = std::chrono::milliseconds{1};
inline constexpr auto publication_timeout_maximum = std::chrono::seconds{120};

struct PublicationDiagnostic {
    std::string code;
    std::string message;
};

struct PublicationRequest {
    std::filesystem::path target;
    std::span<const std::byte> bytes;
    std::uint64_t maximum_bytes;
    PublicationPrecondition precondition;
    std::optional<std::string> expected_prior_sha256;
    std::chrono::milliseconds timeout;
};

struct PublicationResult {
    PublicationOutcome outcome;
    std::uint64_t candidate_size_bytes;
    std::string candidate_sha256;
    std::optional<std::string> prior_sha256;
    std::optional<std::string> observed_target_sha256;
    std::optional<PublicationDiagnostic> primary_diagnostic;
    std::vector<PublicationDiagnostic> cleanup_diagnostics;
};

class DurableStatePublisher {
  public:
    virtual ~DurableStatePublisher() = default;
    [[nodiscard]] PublicationResult publish(const PublicationRequest& request);

  protected:
    [[nodiscard]] virtual PublicationResult publish_validated(
        const PublicationRequest& request,
        std::string_view candidate_sha256,
        std::string_view random_stage_id,
        std::chrono::steady_clock::time_point deadline) = 0;
};

} // namespace arena::ports
