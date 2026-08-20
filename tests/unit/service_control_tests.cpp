#include "arena/ports/service_control.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

namespace ports = arena::ports;
using namespace std::chrono_literals;

enum class FakeBehavior { return_result, throw_runtime, throw_allocation };

class FakeServiceControl final : public ports::ServiceControl {
  public:
    ports::ServiceResult result{ports::ServiceAction::status,
                                ports::ServiceOutcome::completed,
                                ports::ServiceState::active,
                                std::nullopt};
    FakeBehavior behavior{FakeBehavior::return_result};
    int calls{};
    std::chrono::steady_clock::time_point deadline{};

  protected:
    ports::ServiceResult perform_validated(
        const ports::ServiceRequest&,
        const std::chrono::steady_clock::time_point supplied_deadline) override {
        ++calls;
        deadline = supplied_deadline;
        if (behavior == FakeBehavior::throw_runtime) {
            throw std::runtime_error("private platform diagnostic");
        }
        if (behavior == FakeBehavior::throw_allocation) {
            throw std::bad_alloc{};
        }
        return result;
    }
};

[[nodiscard]] ports::ServiceRequest request(
    const ports::ServiceAction action = ports::ServiceAction::status,
    const std::chrono::milliseconds timeout = 50ms) {
    return {"arena.test", action, timeout};
}

void require_failure(const ports::ServiceResult& result,
                     const ports::ServiceAction action,
                     const std::string& code) {
    REQUIRE(result.action == action);
    REQUIRE(result.outcome == ports::ServiceOutcome::failed);
    REQUIRE(result.state == ports::ServiceState::unknown);
    REQUIRE(result.diagnostic.has_value());
    REQUIRE(result.diagnostic->code == code);
    REQUIRE(result.diagnostic->message.empty());
}

} // namespace

TEST_CASE("ServiceControl exposes fixed contract bounds") {
    STATIC_REQUIRE(ports::service_name_max_utf8_bytes == 256);
    STATIC_REQUIRE(ports::service_diagnostic_code_max_bytes == 64);
    STATIC_REQUIRE(ports::service_diagnostic_message_max_utf8_bytes == 512);
    STATIC_REQUIRE(ports::service_timeout_minimum == 1ms);
    STATIC_REQUIRE(ports::service_timeout_maximum == 120s);
    STATIC_REQUIRE(ports::service_timeout_default == 30s);
}

TEST_CASE("ServiceControl invokes a valid request once with one absolute deadline") {
    FakeServiceControl control;
    const auto before = std::chrono::steady_clock::now();
    const auto result = control.perform(request());
    const auto after = std::chrono::steady_clock::now();
    REQUIRE(result.outcome == ports::ServiceOutcome::completed);
    REQUIRE(control.calls == 1);
    REQUIRE(control.deadline >= before + 50ms);
    REQUIRE(control.deadline <= after + 50ms);
}

TEST_CASE("ServiceControl rejects invalid requests before adapter invocation") {
    FakeServiceControl control;
    auto candidate = request();

    for (const auto& name : {std::string{}, std::string(257, 'a'),
                             std::string{"bad\0name", 8}, std::string{"bad\nname"},
                             std::string{"bad\rname"}, std::string{"\xc0\x80", 2}}) {
        candidate.platform_name = name;
        require_failure(control.perform(candidate), candidate.action, "invalid_request");
    }
    candidate = request();
    candidate.timeout = 0ms;
    require_failure(control.perform(candidate), candidate.action, "invalid_request");
    candidate.timeout = 120s + 1ms;
    require_failure(control.perform(candidate), candidate.action, "invalid_request");
    candidate = request(static_cast<ports::ServiceAction>(99));
    require_failure(control.perform(candidate), candidate.action, "invalid_request");
    REQUIRE(control.calls == 0);
}

TEST_CASE("ServiceControl converts ordinary exceptions and propagates allocation failure") {
    FakeServiceControl control;
    control.behavior = FakeBehavior::throw_runtime;
    require_failure(control.perform(request()), ports::ServiceAction::status,
                    "adapter_exception");
    control.behavior = FakeBehavior::throw_allocation;
    REQUIRE_THROWS_AS(control.perform(request()), std::bad_alloc);
}

TEST_CASE("ServiceControl accepts valid completed and failed results unchanged") {
    const std::pair<ports::ServiceAction, ports::ServiceState> completed[] = {
        {ports::ServiceAction::start, ports::ServiceState::active},
        {ports::ServiceAction::stop, ports::ServiceState::inactive},
        {ports::ServiceAction::restart, ports::ServiceState::active},
        {ports::ServiceAction::status, ports::ServiceState::failed},
    };
    for (const auto& [action, state] : completed) {
        FakeServiceControl control;
        control.result = {action, ports::ServiceOutcome::completed, state,
                          ports::ServiceDiagnostic{"platform.note", "bounded"}};
        const auto actual = control.perform(request(action));
        REQUIRE(actual.outcome == ports::ServiceOutcome::completed);
        REQUIRE(actual.state == state);
        REQUIRE(actual.diagnostic->code == "platform.note");
    }

    FakeServiceControl control;
    control.result = {ports::ServiceAction::status, ports::ServiceOutcome::failed,
                      ports::ServiceState::inactive,
                      ports::ServiceDiagnostic{"not_found", "missing"}};
    const auto failed = control.perform(request());
    REQUIRE(failed.outcome == ports::ServiceOutcome::failed);
    REQUIRE(failed.state == ports::ServiceState::inactive);
    REQUIRE(failed.diagnostic->code == "not_found");
}

TEST_CASE("ServiceControl rejects mismatched action and every invalid completed state") {
    FakeServiceControl control;
    control.result.action = ports::ServiceAction::start;
    require_failure(control.perform(request()), ports::ServiceAction::status,
                    "adapter_contract_violation");

    const std::pair<ports::ServiceAction, ports::ServiceState> invalid[] = {
        {ports::ServiceAction::start, ports::ServiceState::inactive},
        {ports::ServiceAction::stop, ports::ServiceState::active},
        {ports::ServiceAction::restart, ports::ServiceState::inactive},
        {ports::ServiceAction::status, ports::ServiceState::unknown},
    };
    for (const auto& [action, state] : invalid) {
        FakeServiceControl candidate;
        candidate.result = {action, ports::ServiceOutcome::completed, state, std::nullopt};
        require_failure(candidate.perform(request(action)), action,
                        "adapter_contract_violation");
    }
}

TEST_CASE("ServiceControl rejects malformed diagnostics") {
    const ports::ServiceDiagnostic invalid[] = {
        {"", "message"}, {std::string(65, 'x'), "message"}, {"bad code", "message"},
        {"code", std::string(513, 'x')}, {"code", "line\nbreak"},
        {"code", std::string{"\xc0\x80", 2}},
    };
    for (const auto& diagnostic : invalid) {
        FakeServiceControl control;
        control.result = {ports::ServiceAction::status, ports::ServiceOutcome::failed,
                          ports::ServiceState::unknown, diagnostic};
        require_failure(control.perform(request()), ports::ServiceAction::status,
                        "adapter_contract_violation");
    }

    FakeServiceControl missing;
    missing.result = {ports::ServiceAction::status, ports::ServiceOutcome::failed,
                      ports::ServiceState::unknown, std::nullopt};
    require_failure(missing.perform(request()), ports::ServiceAction::status,
                    "adapter_contract_violation");
}
