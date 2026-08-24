#pragma once

#include "arena/ports/provider_lifecycle.hpp"

#include <memory>

namespace arena::application {
class ProcessSupervisor;
}

namespace arena::adapters {

// The supplied ProcessSupervisor is not owned and must outlive the returned port.
[[nodiscard]] std::unique_ptr<arena::ports::ProviderLifecycle>
make_cli_provider_lifecycle(
    arena::application::ProcessSupervisor& process_supervisor);

} // namespace arena::adapters
