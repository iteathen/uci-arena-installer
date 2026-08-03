#include "arena/platform/service_control.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace {

using arena::platform::ServiceAction;
using arena::platform::ServiceActionResult;
using arena::platform::ServiceOutcome;
using arena::platform::ServiceState;
using arena::platform::bounded_service_diagnostic;
using arena::platform::perform_service_action;
using arena::platform::service_action_from_name;
using arena::platform::service_action_name;
using arena::platform::service_diagnostic_max_characters;
using arena::platform::service_outcome_name;
using arena::platform::service_state_from_systemd;
using arena::platform::service_state_from_windows_scm;
using arena::platform::service_state_name;
using arena::platform::valid_service_action;

TEST_CASE("service control accepts only the fixed typed action set") {
    constexpr std::array expected{
        std::pair{"status", ServiceAction::status},
        std::pair{"start", ServiceAction::start},
        std::pair{"stop", ServiceAction::stop},
        std::pair{"restart", ServiceAction::restart},
    };
    for (const auto& [name, action] : expected) {
        const auto parsed = service_action_from_name(name);
        REQUIRE(parsed.has_value());
        REQUIRE(*parsed == action);
        REQUIRE(service_action_name(action) == name);
        REQUIRE(valid_service_action(action));
    }

    REQUIRE_FALSE(service_action_from_name("enable").has_value());
    REQUIRE_FALSE(service_action_from_name("disable").has_value());
    REQUIRE_FALSE(service_action_from_name("daemon-reload").has_value());
    REQUIRE_FALSE(service_action_from_name("start --now").has_value());
    REQUIRE_FALSE(service_action_from_name("").has_value());
}

TEST_CASE("service result names form one closed platform-neutral vocabulary") {
    REQUIRE(service_outcome_name(ServiceOutcome::completed) == "completed");
    REQUIRE(service_outcome_name(ServiceOutcome::failed) == "failed");

    constexpr std::array expected{
        std::pair{"unknown", ServiceState::unknown},
        std::pair{"inactive", ServiceState::inactive},
        std::pair{"activating", ServiceState::activating},
        std::pair{"active", ServiceState::active},
        std::pair{"deactivating", ServiceState::deactivating},
        std::pair{"failed", ServiceState::failed},
    };
    for (const auto& [name, state] : expected) {
        REQUIRE(service_state_name(state) == name);
    }

    REQUIRE(service_outcome_name(static_cast<ServiceOutcome>(255)).empty());
    REQUIRE(service_state_name(static_cast<ServiceState>(255)).empty());
}

TEST_CASE("systemd active states map without exposing systemctl process results") {
    REQUIRE(service_state_from_systemd("inactive") == ServiceState::inactive);
    REQUIRE(service_state_from_systemd("activating") == ServiceState::activating);
    REQUIRE(service_state_from_systemd("active") == ServiceState::active);
    REQUIRE(service_state_from_systemd("reloading") == ServiceState::active);
    REQUIRE(service_state_from_systemd("deactivating") == ServiceState::deactivating);
    REQUIRE(service_state_from_systemd("failed") == ServiceState::failed);
    REQUIRE(service_state_from_systemd("maintenance") == ServiceState::failed);
    REQUIRE(service_state_from_systemd("not-found") == ServiceState::unknown);
    REQUIRE(service_state_from_systemd("") == ServiceState::unknown);
}

TEST_CASE("Windows SCM states map without exposing raw SCM values") {
    REQUIRE(service_state_from_windows_scm(1, 0) == ServiceState::inactive);
    REQUIRE(service_state_from_windows_scm(1, 1066) == ServiceState::failed);
    REQUIRE(service_state_from_windows_scm(2) == ServiceState::activating);
    REQUIRE(service_state_from_windows_scm(3) == ServiceState::deactivating);
    REQUIRE(service_state_from_windows_scm(4) == ServiceState::active);
    REQUIRE(service_state_from_windows_scm(5) == ServiceState::unknown);
    REQUIRE(service_state_from_windows_scm(7) == ServiceState::unknown);
    REQUIRE(service_state_from_windows_scm(0xffff'ffffU) == ServiceState::unknown);
}

TEST_CASE("service diagnostics are bounded and contain no control characters") {
    const std::string source = std::string(300, 'x') + "\r\nsecret\tline";
    const std::string diagnostic = bounded_service_diagnostic(source);
    REQUIRE(diagnostic.size() == service_diagnostic_max_characters);
    for (const unsigned char character : diagnostic) {
        REQUIRE(character >= 0x20U);
        REQUIRE(character != 0x7fU);
    }

    REQUIRE(bounded_service_diagnostic("first\nsecond") == "first second");
}

TEST_CASE("service result completion is independent of platform codes") {
    const ServiceActionResult completed{
        ServiceAction::status,
        ServiceOutcome::completed,
        ServiceState::inactive,
        7,
        "operator-only code",
    };
    REQUIRE(completed.completed());

    const ServiceActionResult failed{
        ServiceAction::start,
        ServiceOutcome::failed,
        ServiceState::unknown,
        0,
        "failed",
    };
    REQUIRE_FALSE(failed.completed());
}

TEST_CASE("invalid service actions fail before platform access") {
    const auto invalid = static_cast<ServiceAction>(255);
    REQUIRE(service_action_name(invalid).empty());
    REQUIRE_FALSE(valid_service_action(invalid));
    REQUIRE_THROWS_WITH(
        perform_service_action("unused-service", invalid),
        "unsupported service action");
    REQUIRE_THROWS_WITH(
        perform_service_action("unused-service", "enable"),
        "unsupported service action");
    REQUIRE_THROWS_WITH(
        perform_service_action("", ServiceAction::status),
        "service name is required");
}

} // namespace
