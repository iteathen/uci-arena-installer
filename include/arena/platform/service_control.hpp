#pragma once

#include "arena/ports/service_control.hpp"

#include <memory>
#include <string>

namespace arena::platform {

[[nodiscard]] std::unique_ptr<arena::ports::ServiceControl>
make_service_control();

[[nodiscard]] int perform_service_action(
    const std::string& platform_name,
    const std::string& action);

} // namespace arena::platform
