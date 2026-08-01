#pragma once

#include <string>

namespace arena::platform {

[[nodiscard]] int perform_service_action(
    const std::string& platform_name,
    const std::string& action);

} // namespace arena::platform
