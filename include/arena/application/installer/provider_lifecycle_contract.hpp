#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "arena/ports/provider_lifecycle.hpp"

namespace arena::application::installer {

struct ProviderLifecycleOperationControl {
    std::vector<std::string> arguments;
    bool allow_safe_inactive;
};

struct ProviderLifecycleControl {
    arena::ports::ProviderLifecycleWireProtocol wire_protocol;
    std::string entrypoint;
    std::map<arena::ports::ProviderLifecycleOperation,
             ProviderLifecycleOperationControl>
        operations;
    std::string workspace_flag;
    std::filesystem::path workspace_relative_path;
    std::string configuration_flag;
    std::optional<std::string> previous_root_flag;
    std::optional<std::string> replacement_root_flag;
    arena::ports::ProviderLegacyResultEnvelope legacy_envelope;
    bool legacy_require_configuration_match;
};

[[nodiscard]] ProviderLifecycleControl
parse_provider_lifecycle_control(const nlohmann::json& startup_control);

} // namespace arena::application::installer
