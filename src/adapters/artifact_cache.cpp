#include "arena/ports/artifact_cache.hpp"

#include <algorithm>
#include <cstddef>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>

namespace arena::ports {
namespace {

constexpr std::size_t kMaximumCachePathUtf8Bytes = 131072U;
constexpr auto kMinimumCacheLockTimeout = std::chrono::milliseconds{1};
constexpr auto kMaximumCacheLockTimeout = std::chrono::seconds{120};

[[nodiscard]] bool is_lower_hex_digest(
    const std::string_view digest) noexcept {
    if (digest.size() != 64U) {
        return false;
    }
    return std::ranges::all_of(digest, [](const char value) {
        return (value >= '0' && value <= '9') ||
               (value >= 'a' && value <= 'f');
    });
}

[[nodiscard]] bool has_control(const std::string_view text) noexcept {
    return std::ranges::any_of(text, [](const unsigned char value) {
        return value < 0x20U || value == 0x7fU;
    });
}

[[nodiscard]] bool valid_utf8(const std::string_view text) noexcept {
    const auto* data = reinterpret_cast<const unsigned char*>(text.data());
    std::size_t index = 0;
    while (index < text.size()) {
        const auto first = data[index];
        if (first <= 0x7fU) {
            ++index;
            continue;
        }
        if (first >= 0xc2U && first <= 0xdfU) {
            if (index + 1U >= text.size() ||
                (data[index + 1U] & 0xc0U) != 0x80U) {
                return false;
            }
            index += 2U;
            continue;
        }
        if (first >= 0xe0U && first <= 0xefU) {
            if (index + 2U >= text.size()) {
                return false;
            }
            const auto second = data[index + 1U];
            if ((data[index + 2U] & 0xc0U) != 0x80U) {
                return false;
            }
            if (first == 0xe0U) {
                if (second < 0xa0U || second > 0xbfU) {
                    return false;
                }
            } else if (first == 0xedU) {
                if (second < 0x80U || second > 0x9fU) {
                    return false;
                }
            } else if ((second & 0xc0U) != 0x80U) {
                return false;
            }
            index += 3U;
            continue;
        }
        if (first >= 0xf0U && first <= 0xf4U) {
            if (index + 3U >= text.size()) {
                return false;
            }
            const auto second = data[index + 1U];
            if ((data[index + 2U] & 0xc0U) != 0x80U ||
                (data[index + 3U] & 0xc0U) != 0x80U) {
                return false;
            }
            if (first == 0xf0U) {
                if (second < 0x90U || second > 0xbfU) {
                    return false;
                }
            } else if (first == 0xf4U) {
                if (second < 0x80U || second > 0x8fU) {
                    return false;
                }
            } else if ((second & 0xc0U) != 0x80U) {
                return false;
            }
            index += 4U;
            continue;
        }
        return false;
    }
    return true;
}

[[nodiscard]] std::string path_as_utf8(
    const std::filesystem::path& path) {
    const auto encoded = path.generic_u8string();
    return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

void validate_cache_root(const std::filesystem::path& cache_root) {
    if (cache_root.empty() || !cache_root.is_absolute() ||
        cache_root.lexically_normal() != cache_root ||
        cache_root == cache_root.root_path()) {
        throw std::runtime_error("artifact cache root is invalid");
    }

    std::string encoded;
    try {
        encoded = path_as_utf8(cache_root);
    } catch (const std::bad_alloc&) {
        throw;
    } catch (...) {
        throw std::runtime_error("artifact cache root is invalid");
    }

    if (encoded.empty() || encoded.size() > kMaximumCachePathUtf8Bytes ||
        has_control(encoded) || !valid_utf8(encoded)) {
        throw std::runtime_error("artifact cache root is invalid");
    }
}

} // namespace

std::unique_ptr<ArtifactCacheLease>
ArtifactCachePlatform::acquire_digest_lease(
    const std::filesystem::path& cache_root,
    const std::string_view expected_sha256,
    const std::chrono::milliseconds timeout) {
    validate_cache_root(cache_root);
    if (!is_lower_hex_digest(expected_sha256)) {
        throw std::runtime_error("artifact cache digest is invalid");
    }
    if (timeout < kMinimumCacheLockTimeout ||
        timeout > kMaximumCacheLockTimeout) {
        throw std::runtime_error("artifact cache lock timeout is invalid");
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::unique_ptr<ArtifactCacheLease> lease;
    try {
        lease = acquire_digest_lease_validated(
            cache_root, expected_sha256, deadline);
    } catch (const std::bad_alloc&) {
        throw;
    } catch (...) {
        throw std::runtime_error(
            "artifact cache platform lease acquisition failed");
    }
    if (!lease) {
        throw std::runtime_error(
            "artifact cache platform returned no digest lease");
    }
    return lease;
}

} // namespace arena::ports
