#include "arena/ports/durable_state_publisher.hpp"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <string_view>

namespace arena::ports {
namespace {

[[nodiscard]] bool valid_utf8(const std::string_view value) noexcept {
    std::size_t index = 0;
    while (index < value.size()) {
        const auto first = static_cast<unsigned char>(value[index]);
        if (first <= 0x7f) { ++index; continue; }
        std::size_t length{};
        std::uint32_t code_point{};
        if ((first & 0xe0) == 0xc0) { length = 2; code_point = first & 0x1f; }
        else if ((first & 0xf0) == 0xe0) { length = 3; code_point = first & 0x0f; }
        else if ((first & 0xf8) == 0xf0) { length = 4; code_point = first & 0x07; }
        else return false;
        if (index + length > value.size()) return false;
        for (std::size_t offset = 1; offset < length; ++offset) {
            const auto next = static_cast<unsigned char>(value[index + offset]);
            if ((next & 0xc0) != 0x80) return false;
            code_point = (code_point << 6) | (next & 0x3f);
        }
        if ((length == 2 && code_point < 0x80) ||
            (length == 3 && code_point < 0x800) ||
            (length == 4 && code_point < 0x10000) || code_point > 0x10ffff ||
            (code_point >= 0xd800 && code_point <= 0xdfff)) return false;
        index += length;
    }
    return true;
}

[[nodiscard]] bool contains_control(const std::string_view value) noexcept {
    return value.find('\0') != std::string_view::npos ||
           value.find('\r') != std::string_view::npos ||
           value.find('\n') != std::string_view::npos;
}

[[nodiscard]] bool lowercase_hex(const std::string_view value,
                                 const std::size_t length) noexcept {
    return value.size() == length &&
           std::ranges::all_of(value, [](const unsigned char byte) {
               return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
           });
}

[[nodiscard]] bool valid_target(const std::filesystem::path& target) {
    if (target.empty() || !target.is_absolute() || target.lexically_normal() != target ||
        target.filename().empty() || target.parent_path().empty()) return false;
    try {
        const auto encoded = target.generic_u8string();
        const std::string_view value{reinterpret_cast<const char*>(encoded.data()),
                                     encoded.size()};
        return !value.empty() && value.size() <= publication_path_max_utf8_bytes &&
               !contains_control(value) && valid_utf8(value);
    } catch (const std::bad_alloc&) {
        throw;
    } catch (...) {
        return false;
    }
}

[[nodiscard]] bool valid_request(const PublicationRequest& request) {
    if (!valid_target(request.target) || request.bytes.empty() ||
        request.maximum_bytes == 0 ||
        request.maximum_bytes > publication_global_max_bytes ||
        request.bytes.size() > request.maximum_bytes ||
        request.timeout < publication_timeout_minimum ||
        request.timeout > publication_timeout_maximum) return false;
    switch (request.precondition) {
    case PublicationPrecondition::absent:
        return !request.expected_prior_sha256.has_value();
    case PublicationPrecondition::exact_sha256:
        return request.expected_prior_sha256.has_value() &&
               lowercase_hex(*request.expected_prior_sha256,
                             publication_sha256_hex_bytes);
    }
    return false;
}

[[nodiscard]] std::string candidate_digest(const std::span<const std::byte> bytes) {
    std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
    crypto_hash_sha256(digest.data(),
                       reinterpret_cast<const unsigned char*>(bytes.data()),
                       static_cast<unsigned long long>(bytes.size()));
    std::array<char, crypto_hash_sha256_BYTES * 2 + 1> encoded{};
    sodium_bin2hex(encoded.data(), encoded.size(), digest.data(), digest.size());
    return {encoded.data(), encoded.size() - 1};
}

[[nodiscard]] std::string random_stage_id() {
    std::array<unsigned char, 16> random{};
    randombytes_buf(random.data(), random.size());
    std::array<char, 33> encoded{};
    sodium_bin2hex(encoded.data(), encoded.size(), random.data(), random.size());
    return {encoded.data(), encoded.size() - 1};
}

[[nodiscard]] bool exposes_candidate(const std::string_view message,
                                     const std::span<const std::byte> bytes) noexcept {
    if (bytes.empty() || bytes.size() > message.size()) return false;
    const std::string_view candidate{
        reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    return message.find(candidate) != std::string_view::npos;
}

[[nodiscard]] bool valid_diagnostic(const PublicationDiagnostic& diagnostic,
                                    const std::span<const std::byte> bytes) noexcept {
    const bool valid_code = !diagnostic.code.empty() &&
        diagnostic.code.size() <= publication_diagnostic_code_max_bytes &&
        std::ranges::all_of(diagnostic.code, [](const unsigned char byte) {
            return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'z') ||
                   (byte >= 'A' && byte <= 'Z') || byte == '_' || byte == '.' ||
                   byte == ':' || byte == '-';
        });
    return valid_code &&
           diagnostic.message.size() <= publication_diagnostic_message_max_utf8_bytes &&
           !contains_control(diagnostic.message) && valid_utf8(diagnostic.message) &&
           !exposes_candidate(diagnostic.message, bytes);
}

[[nodiscard]] bool valid_result(const PublicationRequest& request,
                                const std::string_view digest,
                                const PublicationResult& result) noexcept {
    if (request.bytes.size() > std::numeric_limits<std::uint64_t>::max() ||
        result.candidate_size_bytes != request.bytes.size() ||
        result.candidate_sha256 != digest ||
        (result.prior_sha256 && !lowercase_hex(*result.prior_sha256, 64)) ||
        (result.observed_target_sha256 &&
         !lowercase_hex(*result.observed_target_sha256, 64)) ||
        (result.primary_diagnostic &&
         !valid_diagnostic(*result.primary_diagnostic, request.bytes)) ||
        result.cleanup_diagnostics.size() > publication_cleanup_diagnostics_max_count ||
        std::ranges::any_of(result.cleanup_diagnostics, [&](const auto& diagnostic) {
            return !valid_diagnostic(diagnostic, request.bytes);
        })) return false;

    const bool observes_candidate = result.observed_target_sha256 &&
                                     *result.observed_target_sha256 == digest;
    switch (result.outcome) {
    case PublicationOutcome::activated:
        return observes_candidate && !result.primary_diagnostic;
    case PublicationOutcome::not_activated:
        return result.primary_diagnostic.has_value() && !observes_candidate;
    case PublicationOutcome::activated_durability_unproven:
        return observes_candidate && result.primary_diagnostic.has_value();
    case PublicationOutcome::indeterminate:
        return result.primary_diagnostic.has_value();
    }
    return false;
}

[[nodiscard]] PublicationResult failure(const PublicationRequest& request,
                                        const PublicationOutcome outcome,
                                        std::string digest,
                                        std::string code) {
    return {outcome, static_cast<std::uint64_t>(request.bytes.size()),
            std::move(digest), std::nullopt, std::nullopt,
            PublicationDiagnostic{std::move(code), {}}, {}};
}

} // namespace

PublicationResult DurableStatePublisher::publish(const PublicationRequest& request) {
    if (!valid_request(request)) {
        return failure(request, PublicationOutcome::not_activated, {}, "invalid_request");
    }
    const auto digest = candidate_digest(request.bytes);
    const auto stage_id = random_stage_id();
    const auto deadline = std::chrono::steady_clock::now() + request.timeout;
    PublicationResult result;
    try {
        result = publish_validated(request, digest, stage_id, deadline);
    } catch (const std::bad_alloc&) {
        throw;
    } catch (...) {
        return failure(request, PublicationOutcome::indeterminate, digest,
                       "adapter_exception");
    }
    if (!valid_result(request, digest, result)) {
        return failure(request, PublicationOutcome::indeterminate, digest,
                       "adapter_contract_violation");
    }
    return result;
}

} // namespace arena::ports
