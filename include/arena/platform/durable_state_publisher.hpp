#pragma once

#include "arena/ports/durable_state_publisher.hpp"
#include "arena/ports/filesystem_identity.hpp"

#include <memory>

namespace arena::platform {

[[nodiscard]] std::unique_ptr<arena::ports::DurableStatePublisher>
make_durable_state_publisher(arena::ports::FilesystemIdentity& identity);

} // namespace arena::platform
