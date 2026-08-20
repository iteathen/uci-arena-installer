#pragma once

#include <memory>

namespace arena::ports {
class ArtifactCachePlatform;
class FilesystemIdentity;
}

namespace arena::platform {

[[nodiscard]] std::unique_ptr<arena::ports::ArtifactCachePlatform>
make_artifact_cache_platform(
    arena::ports::FilesystemIdentity& filesystem_identity);

} // namespace arena::platform
