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
using arena::platform::service_name_max_characters;
using arena::platform::service_outcome_name;
using arena::platform::service_result_from_observation;
using arena::platform::service_state_from_systemd;
using arena::platform::service_state_from_windows_error;
using arena::platform::service_state_from_windows_scm;
using arena::platform::service_state_name;
using arena::platform::valid_service_action;
using arena::platform::valid_service_name;

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
        std::pair{"unavailable", ServiceState::unavailable},
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

TEST_CASE("service names are bounded printable ASCII before platform access") {
    REQUIRE(valid_service_name("uci-arena-manager.service"));
    REQUIRE(valid_service_name("UCI Arena Manager"));
    REQUIRE(valid_service_name(std::string(service_name_max_characters, 'x')));
    REQUIRE_FALSE(valid_service_name(""));
    REQUIRE_FALSE(valid_service_name(std::string(service_name_max_characters + 1, 'x')));
    REQUIRE_FALSE(valid_service_name("service\nname"));
    REQUIRE_FALSE(valid_service_name(std::string("service\xc2\xa0name", 13)));
}

TEST_CASE("systemd load and active states map without exposing process results") {
    REQUIRE(service_state_from_systemd("inactive") == ServiceState::inactive);
    REQUIRE(service_state_from_systemd("activating") == ServiceState::activating);
    REQUIRE(service_state_from_systemd("active") == ServiceState::active);
    REQUIRE(service_state_from_systemd("reloading") == ServiceState::active);
    REQUIRE(service_state_from_systemd("deactivating") == ServiceState::deactivating);
    REQUIRE(service_state_from_systemd("failed") == ServiceState::failed);
    REQUIRE(service_state_from_systemd("maintenance") == ServiceState::failed);
    REQUIRE(service_state_from_systemd("inactive", "not-found") == ServiceState::unavailable);
    REQUIRE(service_state_from_systemd("inactive", "error") == ServiceState::failed);
    REQUIRE(service_state_from_systemd("inactive", "bad-setting") == ServiceState::failed);
    REQUIRE(service_state_from_systemd("not-found") == ServiceState::unknown);
    REQUIRE(service_state_from_systemd("") == ServiceState::unknown);
}

TEST_CASE("Windows SCM states and errors map without exposing raw values") {
    REQUIRE(service_state_from_windows_scm(1, 0) == ServiceState::inactive);
    REQUIRE(service_state_from_windows_scm(1, 1066) == ServiceState::failed);
    REQUIRE(service_state_from_windows_scm(2) == ServiceState::activating);
    REQUIRE(service_state_from_windows_scm(3) == ServiceState::deactivating);
    REQUIRE(service_state_from_windows_scm(4) == ServiceState::active);
    REQUIRE(service_state_from_windows_scm(5) == ServiceState::unknown);
    REQUIRE(service_state_from_windows_scm(7) == ServiceState::unknown);
    REQUIRE(service_state_from_windows_scm(0xffff'ffffU) == ServiceState::unknown);
    REQUIRE(service_state_from_windows_error(1060) == ServiceState::unavailable);
    REQUIRE(service_state_from_windows_error(1072) == ServiceState::unavailable);
    REQUIRE(service_state_from_windows_error(1722) == ServiceState::unavailable);
    REQUIRE(service_state_from_windows_error(5) == ServiceState::unknown);
}

TEST_CASE("service observation outcome semantics are identical across platforms") {
    constexpr std::array classified_states{
        ServiceState::inactive,
        ServiceState::activating,
        ServiceState::active,
        ServiceState::deactivating,
        ServiceState::failed,
    };
    for (const ServiceState state : classified_states) {
        REQUIRE(service_result_from_observation(
            ServiceAction::status,
            state,
            true).completed());
    }
    REQUIRE_FALSE(service_result_from_observation(
        ServiceAction::status,
        ServiceState::unknown,
        true).completed());
    REQUIRE_FALSE(service_result_from_observation(
        ServiceAction::status,
        ServiceState::unavailable,
        true).completed());

    for (const ServiceState state : classified_states) {
        REQUIRE(service_result_from_observation(
            ServiceAction::start,
            state,
            true).completed() == (state == ServiceState::active));
        REQUIRE(service_result_from_observation(
            ServiceAction::stop,
            state,
            true).completed() == (state == ServiceState::inactive));
        REQUIRE(service_result_from_observation(
            ServiceAction::restart,
            state,
            true).completed() == (state == ServiceState::active));
    }

    for (const ServiceAction action : {
        ServiceAction::status,
        ServiceAction::start,
        ServiceAction::stop,
        ServiceAction::restart,
    }) {
        REQUIRE_FALSE(service_result_from_observation(
            action,
            ServiceState::active,
            false,
            5,
            "platform failure").completed());
    }
}

TEST_CASE("service diagnostics are bounded printable ASCII") {
    const std::string source = std::string(300, 'x') + "\r\nsecret\tline";
    const std::string diagnostic = bounded_service_diagnostic(source);
    REQUIRE(diagnostic.size() == service_diagnostic_max_characters);
    for (const unsigned char character : diagnostic) {
        REQUIRE(character >= 0x20U);
        REQUIRE(character <= 0x7eU);
    }

    REQUIRE(bounded_service_diagnostic("first\nsecond") == "first?second");
    REQUIRE(bounded_service_diagnostic(std::string("x\xc2\xa0y", 4)) == "x??y");
}

TEST_CASE("service result retains bounded operator codes and diagnostics") {
    const ServiceActionResult completed = service_result_from_observation(
        ServiceAction::status,
        ServiceState::failed,
        true,
        7,
        "operator-only code");
    REQUIRE(completed.completed());
    REQUIRE(completed.platform_code == 7);
    REQUIRE(completed.diagnostic == "operator-only code");

    const ServiceActionResult failed = service_result_from_observation(
        ServiceAction::start,
        ServiceState::unknown,
        false,
        5,
        "failed");
    REQUIRE_FALSE(failed.completed());
    REQUIRE(failed.platform_code == 5);
}

TEST_CASE("invalid actions and service names fail before platform access") {
    const auto invalid = static_cast<ServiceAction>(255);
    REQUIRE(service_action_name(invalid).empty());
    REQUIRE_FALSE(valid_service_action(invalid));
    REQUIRE_THROWS_WITH(
        service_result_from_observation(invalid, ServiceState::unknown, false),
        "unsupported service action");
    REQUIRE_THROWS_WITH(
        perform_service_action("unused-service", invalid),
        "unsupported service action");
    REQUIRE_THROWS_WITH(
        perform_service_action("unused-service", "enable"),
        "unsupported service action");
    REQUIRE_THROWS_WITH(
        perform_service_action("", ServiceAction::status),
        "service name is required");
    REQUIRE_THROWS_WITH(
        perform_service_action("bad\nservice", ServiceAction::status),
        "service name is invalid");
    REQUIRE_THROWS_WITH(
        perform_service_action(
            std::string(service_name_max_characters + 1, 'x'),
            ServiceAction::status),
        "service name is invalid");
}

} // namespace
