#pragma once

#include "arena/ports/archive_extraction.hpp"

#include <memory>

namespace arena::platform {

[[nodiscard]] std::unique_ptr<arena::ports::ArchiveExtractionPlatform>
make_archive_extraction_platform(
    arena::ports::FilesystemIdentity& filesystem_identity);

} // namespace arena::platform
