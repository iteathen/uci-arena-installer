#pragma once

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace arena::platform {

enum class ServiceAction {
    status,
    start,
    stop,
    restart,
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

[[nodiscard]] int perform_service_action(
    const std::string& platform_name,
    ServiceAction action);

[[nodiscard]] inline int perform_service_action(
    const std::string& platform_name,
    const std::string& action) {
    const auto parsed = service_action_from_name(action);
    if (!parsed.has_value()) {
        throw std::runtime_error("unsupported service action: " + action);
    }
    return perform_service_action(platform_name, *parsed);
}

} // namespace arena::platform
