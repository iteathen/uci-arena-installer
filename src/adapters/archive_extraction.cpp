#include "arena/ports/archive_extraction.hpp"

#include <stdexcept>

namespace arena::ports {

std::unique_ptr<ArchiveExtractionSession> ArchiveExtractionPlatform::begin(
    const std::filesystem::path& destination_root,
    const ArchiveTargetPlatform target_platform,
    const std::chrono::milliseconds timeout) {
    if (destination_root.empty() || !destination_root.is_absolute() ||
        destination_root.lexically_normal() != destination_root) {
        throw std::invalid_argument("archive destination root is invalid");
    }
    if (timeout < std::chrono::seconds{1} ||
        timeout > std::chrono::hours{168}) {
        throw std::invalid_argument("archive timeout is outside allowed bounds");
    }
    auto session = begin_validated(destination_root, target_platform,
                                   std::chrono::steady_clock::now() + timeout);
    if (!session) {
        throw std::runtime_error("archive extraction platform returned no session");
    }
    return session;
}

} // namespace arena::ports
