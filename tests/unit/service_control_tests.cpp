#include "arena/platform/service_control.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string_view>

namespace {

using arena::platform::ServiceAction;
using arena::platform::perform_service_action;
using arena::platform::service_action_from_name;
using arena::platform::service_action_name;
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

TEST_CASE("invalid service actions fail before platform access") {
    const auto invalid = static_cast<ServiceAction>(255);
    REQUIRE(service_action_name(invalid).empty());
    REQUIRE_FALSE(valid_service_action(invalid));
    REQUIRE_THROWS_WITH(
        perform_service_action("unused-service", invalid),
        Catch::Matchers::ContainsSubstring("unsupported"));
    REQUIRE_THROWS_WITH(
        perform_service_action("unused-service", "enable"),
        "unsupported service action");
}

} // namespace
