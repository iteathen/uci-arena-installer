#include "arena/ports/service_control.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace ports = arena::ports;
using namespace std::chrono_literals;

enum class FakeBehavior {
    return_result,
    throw_runtime,
    throw_allocation,
};

[[nodiscard]] ports::ServiceState completed_state(
    const ports::ServiceAction action) {
    switch (action) {
    case ports::ServiceAction::start:
    case ports::ServiceAction::restart:
        return ports::ServiceState::active;
    case ports::ServiceAction::stop:
        return ports::ServiceState::inactive;
    case ports::ServiceAction::status:
        return ports::ServiceState::active;
    }
    throw std::logic_error("invalid fixture action");
}

[[nodiscard]] ports::ServiceRequest request(
    const ports::ServiceAction action = ports::ServiceAction::start) {
    return {"arena-manager", action, 30s};
}

[[nodiscard]] ports::ServiceResult completed(
    const ports::ServiceAction action = ports::ServiceAction::start) {
    return {action, ports::ServiceOutcome::completed,
            completed_state(action), std::nullopt};
}

[[nodiscard]] ports::ServiceResult failed(
    const ports::ServiceAction action = ports::ServiceAction::start,
    ports::ServiceState state = ports::ServiceState::unknown,
    std::string code = "platform_failure",
    std::string message = "bounded") {
    return {action, ports::ServiceOutcome::failed, state,
            ports::ServiceDiagnostic{std::move(code), std::move(message)}};
}

class FakeServiceControl final : public ports::ServiceControl {
  public:
    ports::ServiceResult result{completed()};
    FakeBehavior behavior{FakeBehavior::return_result};
    int calls{};
    ports::ServiceRequest observed_request{};
    std::chrono::steady_clock::time_point deadline{};

  protected:
    [[nodiscard]] ports::ServiceResult perform_validated(
        const ports::ServiceRequest& supplied_request,
        const std::chrono::steady_clock::time_point supplied_deadline) override {
        ++calls;
        observed_request = supplied_request;
        deadline = supplied_deadline;
        if (behavior == FakeBehavior::throw_runtime) {
            throw std::runtime_error("private adapter detail");
        }
        if (behavior == FakeBehavior::throw_allocation) {
            throw std::bad_alloc{};
        }
        return result;
    }
};

void require_failure(
    const ports::ServiceResult& actual,
    const ports::ServiceAction action,
    const std::string_view code) {
    REQUIRE(actual.action == action);
    REQUIRE(actual.outcome == ports::ServiceOutcome::failed);
    REQUIRE(actual.state == ports::ServiceState::unknown);
    REQUIRE(actual.diagnostic.has_value());
    REQUIRE(actual.diagnostic->code == code);
    REQUIRE(actual.diagnostic->message.empty());
}

void require_invalid_request(ports::ServiceRequest candidate) {
    FakeServiceControl control;
    require_failure(control.perform(candidate), candidate.action,
                    "invalid_request");
    REQUIRE(control.calls == 0);
}

void require_contract_violation(ports::ServiceResult candidate) {
    FakeServiceControl control;
    auto supplied_request = request();
    control.result = std::move(candidate);
    require_failure(control.perform(supplied_request), supplied_request.action,
                    "adapter_contract_violation");
    REQUIRE(control.calls == 1);
}

} // namespace

TEST_CASE("ServiceControl exposes fixed contract constants") {
    STATIC_REQUIRE(ports::service_name_max_utf8_bytes == 256);
    STATIC_REQUIRE(ports::service_diagnostic_code_max_bytes == 64);
    STATIC_REQUIRE(ports::service_diagnostic_message_max_utf8_bytes == 512);
    STATIC_REQUIRE(ports::service_timeout_minimum == 1ms);
    STATIC_REQUIRE(ports::service_timeout_maximum == 120s);
    STATIC_REQUIRE(ports::service_timeout_default == 30s);
}

TEST_CASE("ServiceControl invokes one adapter with one absolute deadline") {
    FakeServiceControl control;
    auto candidate = request(ports::ServiceAction::restart);
    candidate.timeout = 75ms;
    control.result = completed(candidate.action);

    const auto before = std::chrono::steady_clock::now();
    const auto actual = control.perform(candidate);
    const auto after = std::chrono::steady_clock::now();

    REQUIRE(actual.outcome == ports::ServiceOutcome::completed);
    REQUIRE(control.calls == 1);
    REQUIRE(control.observed_request.platform_name == candidate.platform_name);
    REQUIRE(control.observed_request.action == candidate.action);
    REQUIRE(control.observed_request.timeout == candidate.timeout);
    REQUIRE(control.deadline >= before + candidate.timeout);
    REQUIRE(control.deadline <= after + candidate.timeout);
}

TEST_CASE("ServiceControl rejects invalid service names before adapter work") {
    for (auto name : {std::string{}, std::string(257, 'x'),
                      std::string{"bad\0name", 8}, std::string{"bad\nname"},
                      std::string{"bad\rname"}, std::string{"\xc0\xaf", 2}}) {
        auto candidate = request();
        candidate.platform_name = std::move(name);
        require_invalid_request(std::move(candidate));
    }

    auto boundary = request();
    boundary.platform_name.assign(ports::service_name_max_utf8_bytes, 'x');
    FakeServiceControl control;
    REQUIRE(control.perform(boundary).outcome ==
            ports::ServiceOutcome::completed);
    REQUIRE(control.calls == 1);
}

TEST_CASE("ServiceControl rejects invalid action and timeout before adapter work") {
    auto candidate = request();
    candidate.action = static_cast<ports::ServiceAction>(99);
    require_invalid_request(candidate);

    for (const auto timeout : {0ms, 120001ms}) {
        candidate = request();
        candidate.timeout = timeout;
        require_invalid_request(candidate);
    }

    for (const auto timeout : {1ms, 120000ms}) {
        candidate = request();
        candidate.timeout = timeout;
        FakeServiceControl control;
        REQUIRE(control.perform(candidate).outcome ==
                ports::ServiceOutcome::completed);
        REQUIRE(control.calls == 1);
    }
}

TEST_CASE("ServiceControl maps ordinary adapter exceptions and propagates allocation") {
    FakeServiceControl control;
    control.behavior = FakeBehavior::throw_runtime;
    require_failure(control.perform(request()), ports::ServiceAction::start,
                    "adapter_exception");
    REQUIRE(control.calls == 1);

    control.behavior = FakeBehavior::throw_allocation;
    REQUIRE_THROWS_AS(control.perform(request()), std::bad_alloc);
    REQUIRE(control.calls == 2);
}

TEST_CASE("ServiceControl rejects mismatched and invalid result enums") {
    auto candidate = completed(ports::ServiceAction::stop);
    require_contract_violation(candidate);

    candidate = completed();
    candidate.outcome = static_cast<ports::ServiceOutcome>(99);
    require_contract_violation(candidate);

    candidate = completed();
    candidate.state = static_cast<ports::ServiceState>(99);
    require_contract_violation(candidate);
}

TEST_CASE("ServiceControl enforces completed state for every action") {
    constexpr std::array actions{
        ports::ServiceAction::start,
        ports::ServiceAction::stop,
        ports::ServiceAction::restart,
        ports::ServiceAction::status,
    };
    constexpr std::array states{
        ports::ServiceState::unknown,
        ports::ServiceState::inactive,
        ports::ServiceState::activating,
        ports::ServiceState::active,
        ports::ServiceState::deactivating,
        ports::ServiceState::failed,
    };

    for (const auto action : actions) {
        for (const auto state : states) {
            FakeServiceControl control;
            auto supplied_request = request(action);
            control.result = {action, ports::ServiceOutcome::completed, state,
                              std::nullopt};
            const bool valid =
                ((action == ports::ServiceAction::start ||
                  action == ports::ServiceAction::restart) &&
                 state == ports::ServiceState::active) ||
                (action == ports::ServiceAction::stop &&
                 state == ports::ServiceState::inactive) ||
                (action == ports::ServiceAction::status &&
                 state != ports::ServiceState::unknown);
            const auto actual = control.perform(supplied_request);
            if (valid) {
                REQUIRE(actual.outcome == ports::ServiceOutcome::completed);
                REQUIRE(actual.state == state);
            } else {
                require_failure(actual, action,
                                "adapter_contract_violation");
            }
        }
    }
}

TEST_CASE("ServiceControl requires one valid bounded failure diagnostic") {
    auto candidate = failed();
    candidate.diagnostic.reset();
    require_contract_violation(candidate);

    for (auto diagnostic : {
             ports::ServiceDiagnostic{"", "bounded"},
             ports::ServiceDiagnostic{"bad code", "bounded"},
             ports::ServiceDiagnostic{std::string(65, 'x'), "bounded"},
             ports::ServiceDiagnostic{"code", std::string(513, 'x')},
             ports::ServiceDiagnostic{"code", std::string{"bad\0text", 8}},
             ports::ServiceDiagnostic{"code", "bad\ntext"},
             ports::ServiceDiagnostic{"code", "bad\rtext"},
             ports::ServiceDiagnostic{"code", std::string{"\xc0\xaf", 2}},
         }) {
        candidate = failed();
        candidate.diagnostic = std::move(diagnostic);
        require_contract_violation(candidate);
    }

    candidate = completed();
    candidate.diagnostic = ports::ServiceDiagnostic{"bad code", {}};
    require_contract_violation(candidate);
}

TEST_CASE("ServiceControl preserves valid completed and failed results") {
    constexpr std::array actions{
        ports::ServiceAction::start,
        ports::ServiceAction::stop,
        ports::ServiceAction::restart,
        ports::ServiceAction::status,
    };
    for (const auto action : actions) {
        FakeServiceControl control;
        auto expected = completed(action);
        expected.diagnostic = ports::ServiceDiagnostic{"platform.note", "ok"};
        control.result = expected;
        const auto actual = control.perform(request(action));
        REQUIRE(actual.action == expected.action);
        REQUIRE(actual.outcome == expected.outcome);
        REQUIRE(actual.state == expected.state);
        REQUIRE(actual.diagnostic->code == expected.diagnostic->code);
        REQUIRE(actual.diagnostic->message == expected.diagnostic->message);
    }

    for (const auto state : {ports::ServiceState::unknown,
                             ports::ServiceState::inactive,
                             ports::ServiceState::activating,
                             ports::ServiceState::active,
                             ports::ServiceState::deactivating,
                             ports::ServiceState::failed}) {
        FakeServiceControl control;
        auto expected = failed(ports::ServiceAction::status, state);
        control.result = expected;
        const auto actual = control.perform(request(ports::ServiceAction::status));
        REQUIRE(actual.outcome == ports::ServiceOutcome::failed);
        REQUIRE(actual.state == state);
        REQUIRE(actual.diagnostic->code == expected.diagnostic->code);
        REQUIRE(actual.diagnostic->message == expected.diagnostic->message);
    }
}
