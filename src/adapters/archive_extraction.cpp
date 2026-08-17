#include "arena/ports/archive_extraction.hpp"

#include <algorithm>
#include <cstddef>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>

namespace arena::ports {
namespace {

constexpr std::size_t kMaximumPathUtf8Bytes = 131072U;
constexpr auto kMinimumTimeout = std::chrono::seconds{1};
constexpr auto kMaximumTimeout = std::chrono::hours{168};

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

[[nodiscard]] bool has_control(const std::string_view text) noexcept {
    return std::ranges::any_of(text, [](const unsigned char value) {
        return value < 0x20U || value == 0x7fU;
    });
}

[[nodiscard]] std::string path_as_utf8(
    const std::filesystem::path& path) {
    const auto encoded = path.generic_u8string();
    return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

void validate_destination_root(const std::filesystem::path& path) {
    if (path.empty() || !path.is_absolute() ||
        path.lexically_normal() != path || path == path.root_path()) {
        throw std::runtime_error("archive extraction destination is invalid");
    }

    std::string encoded;
    try {
        encoded = path_as_utf8(path);
    } catch (const std::bad_alloc&) {
        throw;
    } catch (...) {
        throw std::runtime_error("archive extraction destination is invalid");
    }
    if (encoded.empty() || encoded.size() > kMaximumPathUtf8Bytes ||
        has_control(encoded) || !valid_utf8(encoded)) {
        throw std::runtime_error("archive extraction destination is invalid");
    }
}

void validate_target_platform(const ArchiveTargetPlatform target) {
    switch (target) {
    case ArchiveTargetPlatform::windows_x86_64:
    case ArchiveTargetPlatform::linux_x86_64:
        return;
    }
    throw std::runtime_error("archive extraction target platform is invalid");
}

} // namespace

std::unique_ptr<ArchiveExtractionSession> ArchiveExtractionPlatform::begin(
    const std::filesystem::path& destination_root,
    const ArchiveTargetPlatform target_platform,
    const std::chrono::milliseconds timeout) {
    validate_destination_root(destination_root);
    validate_target_platform(target_platform);
    if (timeout < kMinimumTimeout || timeout > kMaximumTimeout) {
        throw std::runtime_error("archive extraction timeout is invalid");
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::unique_ptr<ArchiveExtractionSession> session;
    try {
        session = begin_validated(destination_root, target_platform, deadline);
    } catch (const std::bad_alloc&) {
        throw;
    } catch (...) {
        throw std::runtime_error("archive extraction platform begin failed");
    }
    if (!session) {
        throw std::runtime_error("archive extraction platform returned no session");
    }
    return session;
}

} // namespace arena::ports
