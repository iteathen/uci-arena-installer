#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace arena::platform {

inline constexpr std::size_t service_name_max_characters = 256;
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
    unavailable,
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
    case ServiceState::unavailable:
        return "unavailable";
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

[[nodiscard]] constexpr bool valid_service_name(
    const std::string_view service_name) noexcept {
    if (service_name.empty()
        || service_name.size() > service_name_max_characters) {
        return false;
    }
    for (const unsigned char character : service_name) {
        if (character < 0x20U || character > 0x7eU) {
            return false;
        }
    }
    return true;
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
            character < 0x20U || character > 0x7eU
                ? '?'
                : static_cast<char>(character));
    }
    return result;
}

[[nodiscard]] constexpr ServiceState service_state_from_systemd(
    const std::string_view active_state,
    const std::string_view load_state = "loaded") noexcept {
    if (load_state == "not-found") {
        return ServiceState::unavailable;
    }
    if (load_state == "error" || load_state == "bad-setting") {
        return ServiceState::failed;
    }
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

[[nodiscard]] constexpr ServiceState service_state_from_windows_error(
    const std::uint32_t error_code) noexcept {
    return error_code == 1060U // ERROR_SERVICE_DOES_NOT_EXIST
        || error_code == 1072U // ERROR_SERVICE_MARKED_FOR_DELETE
        || error_code == 1722U // RPC_S_SERVER_UNAVAILABLE
        ? ServiceState::unavailable
        : ServiceState::unknown;
}

[[nodiscard]] inline ServiceActionResult service_result_from_observation(
    const ServiceAction action,
    const ServiceState state,
    const bool platform_operation_succeeded,
    const std::uint32_t platform_code = 0,
    const std::string_view diagnostic = {}) {
    if (!valid_service_action(action)) {
        throw std::runtime_error("unsupported service action");
    }
    bool completed = platform_operation_succeeded
        && state != ServiceState::unknown
        && state != ServiceState::unavailable;
    if (completed && action == ServiceAction::start) {
        completed = state == ServiceState::active;
    } else if (completed && action == ServiceAction::stop) {
        completed = state == ServiceState::inactive;
    } else if (completed && action == ServiceAction::restart) {
        completed = state == ServiceState::active;
    }
    return ServiceActionResult{
        action,
        completed ? ServiceOutcome::completed : ServiceOutcome::failed,
        state,
        platform_code,
        bounded_service_diagnostic(diagnostic),
    };
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
