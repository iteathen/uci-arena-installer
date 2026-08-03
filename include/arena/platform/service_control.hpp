#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace arena::platform {

inline constexpr std::size_t service_diagnostic_max_characters = 256;

enum class ServiceAction {
    status,
    start,
    stop,
    restart,
};

enum class ServiceOutcome {
    completed,
    failed,
};

enum class ServiceState {
    unknown,
    inactive,
    activating,
    active,
    deactivating,
    failed,
};

struct ServiceActionResult final {
    ServiceAction action{ServiceAction::status};
    ServiceOutcome outcome{ServiceOutcome::failed};
    ServiceState state{ServiceState::unknown};
    std::uint32_t platform_code{0};
    std::string diagnostic;

    [[nodiscard]] constexpr bool completed() const noexcept {
        return outcome == ServiceOutcome::completed;
    }
};

[[nodiscard]] constexpr std::string_view service_action_name(
    const ServiceAction action) noexcept {
    switch (action) {
    case ServiceAction::status:
        return "status";
    case ServiceAction::start:
        return "start";
    case ServiceAction::stop:
        return "stop";
    case ServiceAction::restart:
        return "restart";
    }
    return "";
}

[[nodiscard]] constexpr std::string_view service_outcome_name(
    const ServiceOutcome outcome) noexcept {
    switch (outcome) {
    case ServiceOutcome::completed:
        return "completed";
    case ServiceOutcome::failed:
        return "failed";
    }
    return "";
}

[[nodiscard]] constexpr std::string_view service_state_name(
    const ServiceState state) noexcept {
    switch (state) {
    case ServiceState::unknown:
        return "unknown";
    case ServiceState::inactive:
        return "inactive";
    case ServiceState::activating:
        return "activating";
    case ServiceState::active:
        return "active";
    case ServiceState::deactivating:
        return "deactivating";
    case ServiceState::failed:
        return "failed";
    }
    return "";
}

[[nodiscard]] constexpr bool valid_service_action(
    const ServiceAction action) noexcept {
    return !service_action_name(action).empty();
}

[[nodiscard]] constexpr std::optional<ServiceAction> service_action_from_name(
    const std::string_view action) noexcept {
    if (action == "status") {
        return ServiceAction::status;
    }
    if (action == "start") {
        return ServiceAction::start;
    }
    if (action == "stop") {
        return ServiceAction::stop;
    }
    if (action == "restart") {
        return ServiceAction::restart;
    }
    return std::nullopt;
}

[[nodiscard]] inline std::string bounded_service_diagnostic(
    const std::string_view diagnostic) {
    std::string result;
    result.reserve(std::min(diagnostic.size(), service_diagnostic_max_characters));
    for (const unsigned char character : diagnostic) {
        if (result.size() >= service_diagnostic_max_characters) {
            break;
        }
        result.push_back(
            character < 0x20U || character == 0x7fU
                ? ' '
                : static_cast<char>(character));
    }
    return result;
}

[[nodiscard]] constexpr ServiceState service_state_from_systemd(
    const std::string_view active_state) noexcept {
    if (active_state == "inactive") {
        return ServiceState::inactive;
    }
    if (active_state == "activating") {
        return ServiceState::activating;
    }
    if (active_state == "active" || active_state == "reloading") {
        return ServiceState::active;
    }
    if (active_state == "deactivating") {
        return ServiceState::deactivating;
    }
    if (active_state == "failed" || active_state == "maintenance") {
        return ServiceState::failed;
    }
    return ServiceState::unknown;
}

[[nodiscard]] constexpr ServiceState service_state_from_windows_scm(
    const std::uint32_t current_state,
    const std::uint32_t service_exit_code = 0) noexcept {
    // Stable Win32 SERVICE_* values from winsvc.h. Keeping the mapping in the
    // platform-neutral header permits pure tests without including Windows SDK types.
    switch (current_state) {
    case 1: // SERVICE_STOPPED
        return service_exit_code == 0
            ? ServiceState::inactive
            : ServiceState::failed;
    case 2: // SERVICE_START_PENDING
        return ServiceState::activating;
    case 3: // SERVICE_STOP_PENDING
        return ServiceState::deactivating;
    case 4: // SERVICE_RUNNING
        return ServiceState::active;
    default:
        return ServiceState::unknown;
    }
}

[[nodiscard]] ServiceActionResult perform_service_action(
    const std::string& platform_name,
    ServiceAction action);

[[nodiscard]] inline ServiceActionResult perform_service_action(
    const std::string& platform_name,
    const std::string_view action) {
    const auto parsed = service_action_from_name(action);
    if (!parsed.has_value()) {
        throw std::runtime_error("unsupported service action");
    }
    return perform_service_action(platform_name, *parsed);
}

} // namespace arena::platform
