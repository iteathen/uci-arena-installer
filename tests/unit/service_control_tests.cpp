#include "arena/platform/service_control.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string_view>

namespace {

using arena::platform::ServiceAction;
using arena::platform::service_action_from_name;
using arena::platform::service_action_name;

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
    }

    REQUIRE_FALSE(service_action_from_name("enable").has_value());
    REQUIRE_FALSE(service_action_from_name("disable").has_value());
    REQUIRE_FALSE(service_action_from_name("daemon-reload").has_value());
    REQUIRE_FALSE(service_action_from_name("start --now").has_value());
    REQUIRE_FALSE(service_action_from_name("").has_value());
}

} // namespace
