#pragma once

#include <memory>

namespace arena::ports {
class FilesystemIdentity;
}

namespace arena::platform {

[[nodiscard]] std::unique_ptr<arena::ports::FilesystemIdentity>
make_filesystem_identity();

} // namespace arena::platform
