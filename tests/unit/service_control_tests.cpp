#include "arena/ports/service_control.hpp"

#ifdef _WIN32
#include "arena/platform/service_control.hpp"
#include "../../src/platform/windows/service_control_private.hpp"

#define NOMINMAX
#include <windows.h>
#include <winsvc.h>
#endif

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

#ifdef _WIN32

namespace windows_detail = arena::platform::windows_detail;
using WindowsClock = std::chrono::steady_clock;

struct WindowsBackendTrace {
    WindowsClock::time_point now{WindowsClock::now()};
    bool manager_succeeds{true};
    bool service_succeeds{true};
    bool start_succeeds{true};
    bool stop_succeeds{true};
    bool continue_succeeds{true};
    bool sleep_throws{};
    std::uint32_t manager_error{ERROR_ACCESS_DENIED};
    std::uint32_t service_error{ERROR_SERVICE_DOES_NOT_EXIST};
    std::uint32_t query_error{ERROR_ACCESS_DENIED};
    std::uint32_t start_error{ERROR_ACCESS_DENIED};
    std::uint32_t stop_error{ERROR_ACCESS_DENIED};
    std::uint32_t continue_error{ERROR_SERVICE_CANNOT_ACCEPT_CTRL};
    std::optional<std::size_t> query_failure_at;
    std::vector<windows_detail::NativeServiceStatus> statuses;
    std::chrono::milliseconds advance_on_manager{};
    std::chrono::milliseconds advance_on_service{};
    std::chrono::milliseconds advance_on_query{};
    std::chrono::milliseconds advance_on_start{};
    std::chrono::milliseconds advance_on_command{};
    std::chrono::milliseconds sleep_overshoot{};
    int manager_opens{};
    int service_opens{};
    int query_calls{};
    int start_calls{};
    int close_calls{};
    bool service_name_matches{};
    windows_detail::NativeServiceAccess access{
        windows_detail::NativeServiceAccess::status};
    std::array<windows_detail::NativeServiceCommand, 16> commands{};
    std::size_t command_count{};
    std::array<std::chrono::milliseconds, 32> sleeps{};
    std::size_t sleep_count{};
    std::array<windows_detail::NativeServiceHandle, 8> closed{};
};

class FakeWindowsServiceBackend final
    : public windows_detail::ServiceControlBackend {
  public:
    explicit FakeWindowsServiceBackend(
        std::shared_ptr<WindowsBackendTrace> trace)
        : trace_(std::move(trace)) {}

    [[nodiscard]] windows_detail::NativeServiceHandle
    open_manager(std::uint32_t& error) noexcept override {
        ++trace_->manager_opens;
        trace_->now += trace_->advance_on_manager;
        if (!trace_->manager_succeeds) {
            error = trace_->manager_error;
            return windows_detail::invalid_service_handle;
        }
        return 1;
    }

    [[nodiscard]] windows_detail::NativeServiceHandle open_service(
        const windows_detail::NativeServiceHandle manager,
        const std::wstring& platform_name,
        const windows_detail::NativeServiceAccess access,
        std::uint32_t& error) noexcept override {
        ++trace_->service_opens;
        trace_->service_name_matches =
            manager == 1 && platform_name == L"arena-manager";
        trace_->access = access;
        trace_->now += trace_->advance_on_service;
        if (!trace_->service_succeeds) {
            error = trace_->service_error;
            return windows_detail::invalid_service_handle;
        }
        return 2;
    }

    [[nodiscard]] bool query(
        const windows_detail::NativeServiceHandle service,
        windows_detail::NativeServiceStatus& status,
        std::uint32_t& error) noexcept override {
        const auto call = static_cast<std::size_t>(trace_->query_calls++);
        trace_->now += trace_->advance_on_query;
        if (service != 2 ||
            (trace_->query_failure_at.has_value() &&
             call == *trace_->query_failure_at) ||
            trace_->statuses.empty()) {
            error = trace_->query_error;
            return false;
        }
        status = trace_->statuses[(std::min)(
            call, trace_->statuses.size() - 1)];
        return true;
    }

    [[nodiscard]] bool start(
        const windows_detail::NativeServiceHandle service,
        std::uint32_t& error) noexcept override {
        ++trace_->start_calls;
        trace_->now += trace_->advance_on_start;
        if (service != 2 || !trace_->start_succeeds) {
            error = trace_->start_error;
            return false;
        }
        return true;
    }

    [[nodiscard]] bool command(
        const windows_detail::NativeServiceHandle service,
        const windows_detail::NativeServiceCommand command,
        std::uint32_t& error) noexcept override {
        if (trace_->command_count < trace_->commands.size()) {
            trace_->commands[trace_->command_count] = command;
        }
        ++trace_->command_count;
        trace_->now += trace_->advance_on_command;
        const bool succeeds =
            command == windows_detail::NativeServiceCommand::stop
                ? trace_->stop_succeeds
                : trace_->continue_succeeds;
        if (service != 2 || !succeeds) {
            error = command == windows_detail::NativeServiceCommand::stop
                        ? trace_->stop_error
                        : trace_->continue_error;
            return false;
        }
        return true;
    }

    void close(
        const windows_detail::NativeServiceHandle handle) noexcept override {
        if (trace_->close_calls <
            static_cast<int>(trace_->closed.size())) {
            trace_->closed[static_cast<std::size_t>(trace_->close_calls)] =
                handle;
        }
        ++trace_->close_calls;
    }

    [[nodiscard]] WindowsClock::time_point now() const noexcept override {
        return trace_->now;
    }

    void sleep_for(const std::chrono::milliseconds duration) override {
        if (trace_->sleep_count < trace_->sleeps.size()) {
            trace_->sleeps[trace_->sleep_count] = duration;
        }
        ++trace_->sleep_count;
        if (trace_->sleep_throws) {
            throw std::runtime_error("private wait failure");
        }
        trace_->now += duration + trace_->sleep_overshoot;
    }

  private:
    std::shared_ptr<WindowsBackendTrace> trace_;
};

struct WindowsControlFixture {
    WindowsControlFixture()
        : trace(std::make_shared<WindowsBackendTrace>()),
          control(windows_detail::make_service_control_for_testing(
              std::make_unique<FakeWindowsServiceBackend>(trace))) {}

    std::shared_ptr<WindowsBackendTrace> trace;
    std::unique_ptr<ports::ServiceControl> control;
};

[[nodiscard]] windows_detail::NativeServiceStatus windows_status(
    const std::uint32_t state,
    const std::uint32_t wait_hint = 0,
    const std::uint32_t win32_exit = ERROR_SUCCESS,
    const std::uint32_t service_exit = 0) {
    return {state, win32_exit, service_exit, wait_hint};
}

void require_windows_failure(
    const ports::ServiceResult& actual,
    const ports::ServiceAction action,
    const ports::ServiceState state,
    const std::string_view code) {
    REQUIRE(actual.action == action);
    REQUIRE(actual.outcome == ports::ServiceOutcome::failed);
    REQUIRE(actual.state == state);
    REQUIRE(actual.diagnostic.has_value());
    REQUIRE(actual.diagnostic->code == code);
    REQUIRE(actual.diagnostic->message.empty());
}

void require_windows_handles_closed(const WindowsBackendTrace& trace) {
    REQUIRE(trace.close_calls == 2);
    REQUIRE(trace.closed[0] == 2);
    REQUIRE(trace.closed[1] == 1);
}

#endif

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

#ifdef _WIN32

TEST_CASE("Windows ServiceControl maps every SCM status without mutation") {
    struct Mapping {
        std::uint32_t native;
        ports::ServiceState neutral;
    };
    constexpr std::array mappings{
        Mapping{SERVICE_STOPPED, ports::ServiceState::inactive},
        Mapping{SERVICE_START_PENDING, ports::ServiceState::activating},
        Mapping{SERVICE_STOP_PENDING, ports::ServiceState::deactivating},
        Mapping{SERVICE_RUNNING, ports::ServiceState::active},
        Mapping{SERVICE_CONTINUE_PENDING, ports::ServiceState::activating},
        Mapping{SERVICE_PAUSE_PENDING, ports::ServiceState::deactivating},
        Mapping{SERVICE_PAUSED, ports::ServiceState::inactive},
    };

    for (const auto& mapping : mappings) {
        WindowsControlFixture fixture;
        fixture.trace->statuses = {windows_status(mapping.native)};

        const auto actual = fixture.control->perform(
            request(ports::ServiceAction::status));

        REQUIRE(actual.outcome == ports::ServiceOutcome::completed);
        REQUIRE(actual.state == mapping.neutral);
        REQUIRE(actual.diagnostic.has_value() ==
                (mapping.native == SERVICE_PAUSED));
        if (mapping.native == SERVICE_PAUSED) {
            REQUIRE(actual.diagnostic->code == "windows_service_paused");
        }
        REQUIRE(fixture.trace->query_calls == 1);
        REQUIRE(fixture.trace->start_calls == 0);
        REQUIRE(fixture.trace->command_count == 0);
        REQUIRE(fixture.trace->service_name_matches);
        REQUIRE(fixture.trace->access ==
                windows_detail::NativeServiceAccess::status);
        require_windows_handles_closed(*fixture.trace);
    }
}

TEST_CASE("Windows ServiceControl reports trustworthy stopped failures") {
    SECTION("Win32 exit") {
        WindowsControlFixture fixture;
        fixture.trace->statuses = {
            windows_status(SERVICE_STOPPED, 0, ERROR_ACCESS_DENIED)};
        const auto actual = fixture.control->perform(
            request(ports::ServiceAction::status));
        require_windows_failure(
            actual, ports::ServiceAction::status,
            ports::ServiceState::failed, "windows_service_exit:5");
    }

    SECTION("service-specific exit") {
        WindowsControlFixture fixture;
        fixture.trace->statuses = {windows_status(
            SERVICE_STOPPED, 0, ERROR_SERVICE_SPECIFIC_ERROR, 73)};
        const auto actual = fixture.control->perform(
            request(ports::ServiceAction::status));
        require_windows_failure(
            actual, ports::ServiceAction::status,
            ports::ServiceState::failed,
            "windows_service_exit_specific:73");
    }

    SECTION("untrustworthy service-specific field is ignored") {
        WindowsControlFixture fixture;
        fixture.trace->statuses = {
            windows_status(SERVICE_STOPPED, 0, ERROR_SUCCESS, 73)};
        const auto actual = fixture.control->perform(
            request(ports::ServiceAction::status));
        REQUIRE(actual.outcome == ports::ServiceOutcome::completed);
        REQUIRE(actual.state == ports::ServiceState::inactive);
    }
}

TEST_CASE("Windows ServiceControl contains open query and unknown-state failures") {
    SECTION("manager permission") {
        WindowsControlFixture fixture;
        fixture.trace->manager_succeeds = false;
        const auto actual = fixture.control->perform(
            request(ports::ServiceAction::status));
        require_windows_failure(
            actual, ports::ServiceAction::status,
            ports::ServiceState::unknown, "windows_scm_open:5");
        REQUIRE(fixture.trace->close_calls == 0);
    }

    SECTION("missing service") {
        WindowsControlFixture fixture;
        fixture.trace->service_succeeds = false;
        const auto actual = fixture.control->perform(
            request(ports::ServiceAction::status));
        require_windows_failure(
            actual, ports::ServiceAction::status,
            ports::ServiceState::unknown, "windows_service_open:1060");
        REQUIRE(fixture.trace->close_calls == 1);
        REQUIRE(fixture.trace->closed[0] == 1);
    }

    SECTION("query permission") {
        WindowsControlFixture fixture;
        fixture.trace->statuses = {windows_status(SERVICE_RUNNING)};
        fixture.trace->query_failure_at = 0;
        const auto actual = fixture.control->perform(
            request(ports::ServiceAction::status));
        require_windows_failure(
            actual, ports::ServiceAction::status,
            ports::ServiceState::unknown, "windows_service_query:5");
        require_windows_handles_closed(*fixture.trace);
    }

    SECTION("unrecognized SCM state") {
        WindowsControlFixture fixture;
        fixture.trace->statuses = {windows_status(999)};
        const auto actual = fixture.control->perform(
            request(ports::ServiceAction::status));
        require_windows_failure(
            actual, ports::ServiceAction::status,
            ports::ServiceState::unknown, "windows_service_state:999");
        require_windows_handles_closed(*fixture.trace);
    }
}

TEST_CASE("Windows ServiceControl preserves idempotent start and stop") {
    SECTION("active start") {
        WindowsControlFixture fixture;
        fixture.trace->statuses = {windows_status(SERVICE_RUNNING)};
        const auto actual = fixture.control->perform(
            request(ports::ServiceAction::start));
        REQUIRE(actual.outcome == ports::ServiceOutcome::completed);
        REQUIRE(actual.state == ports::ServiceState::active);
        REQUIRE(fixture.trace->start_calls == 0);
        REQUIRE(fixture.trace->command_count == 0);
        REQUIRE(fixture.trace->access ==
                windows_detail::NativeServiceAccess::start);
        require_windows_handles_closed(*fixture.trace);
    }

    SECTION("inactive stop") {
        WindowsControlFixture fixture;
        fixture.trace->statuses = {windows_status(SERVICE_STOPPED)};
        const auto actual = fixture.control->perform(
            request(ports::ServiceAction::stop));
        REQUIRE(actual.outcome == ports::ServiceOutcome::completed);
        REQUIRE(actual.state == ports::ServiceState::inactive);
        REQUIRE(fixture.trace->start_calls == 0);
        REQUIRE(fixture.trace->command_count == 0);
        REQUIRE(fixture.trace->access ==
                windows_detail::NativeServiceAccess::stop);
        require_windows_handles_closed(*fixture.trace);
    }
}

TEST_CASE("Windows ServiceControl starts stopped and paused services") {
    SECTION("stopped") {
        WindowsControlFixture fixture;
        fixture.trace->statuses = {
            windows_status(SERVICE_STOPPED),
            windows_status(SERVICE_START_PENDING, 1000),
            windows_status(SERVICE_RUNNING),
        };
        const auto actual = fixture.control->perform(
            request(ports::ServiceAction::start));
        REQUIRE(actual.outcome == ports::ServiceOutcome::completed);
        REQUIRE(actual.state == ports::ServiceState::active);
        REQUIRE(fixture.trace->start_calls == 1);
        REQUIRE(fixture.trace->command_count == 0);
        REQUIRE(fixture.trace->sleep_count == 1);
        REQUIRE(fixture.trace->sleeps[0] == 100ms);
        require_windows_handles_closed(*fixture.trace);
    }

    SECTION("paused") {
        WindowsControlFixture fixture;
        fixture.trace->statuses = {
            windows_status(SERVICE_PAUSED),
            windows_status(SERVICE_CONTINUE_PENDING, 500),
            windows_status(SERVICE_RUNNING),
        };
        const auto actual = fixture.control->perform(
            request(ports::ServiceAction::start));
        REQUIRE(actual.outcome == ports::ServiceOutcome::completed);
        REQUIRE(actual.state == ports::ServiceState::active);
        REQUIRE(fixture.trace->start_calls == 0);
        REQUIRE(fixture.trace->command_count == 1);
        REQUIRE(fixture.trace->commands[0] ==
                windows_detail::NativeServiceCommand::continue_service);
        REQUIRE(fixture.trace->sleep_count == 1);
        require_windows_handles_closed(*fixture.trace);
    }
}

TEST_CASE("Windows ServiceControl resolves pending states within one action") {
    SECTION("start already pending") {
        WindowsControlFixture fixture;
        fixture.trace->statuses = {
            windows_status(SERVICE_START_PENDING, 500),
            windows_status(SERVICE_RUNNING),
        };
        const auto actual = fixture.control->perform(
            request(ports::ServiceAction::start));
        REQUIRE(actual.outcome == ports::ServiceOutcome::completed);
        REQUIRE(fixture.trace->start_calls == 0);
        REQUIRE(fixture.trace->sleep_count == 1);
    }

    SECTION("stop pending before start") {
        WindowsControlFixture fixture;
        fixture.trace->statuses = {
            windows_status(SERVICE_STOP_PENDING, 500),
            windows_status(SERVICE_STOPPED),
            windows_status(SERVICE_START_PENDING, 500),
            windows_status(SERVICE_RUNNING),
        };
        const auto actual = fixture.control->perform(
            request(ports::ServiceAction::start));
        REQUIRE(actual.outcome == ports::ServiceOutcome::completed);
        REQUIRE(fixture.trace->start_calls == 1);
        REQUIRE(fixture.trace->sleep_count == 2);
    }

    SECTION("start pending before stop") {
        WindowsControlFixture fixture;
        fixture.trace->statuses = {
            windows_status(SERVICE_START_PENDING, 500),
            windows_status(SERVICE_RUNNING),
            windows_status(SERVICE_STOP_PENDING, 500),
            windows_status(SERVICE_STOPPED),
        };
        const auto actual = fixture.control->perform(
            request(ports::ServiceAction::stop));
        REQUIRE(actual.outcome == ports::ServiceOutcome::completed);
        REQUIRE(actual.state == ports::ServiceState::inactive);
        REQUIRE(fixture.trace->command_count == 1);
        REQUIRE(fixture.trace->commands[0] ==
                windows_detail::NativeServiceCommand::stop);
        REQUIRE(fixture.trace->sleep_count == 2);
    }
}

TEST_CASE("Windows ServiceControl stops active and paused services") {
    for (const auto initial : {SERVICE_RUNNING, SERVICE_PAUSED}) {
        WindowsControlFixture fixture;
        fixture.trace->statuses = {
            windows_status(initial),
            windows_status(SERVICE_STOP_PENDING, 800),
            windows_status(SERVICE_STOPPED),
        };
        const auto actual = fixture.control->perform(
            request(ports::ServiceAction::stop));
        REQUIRE(actual.outcome == ports::ServiceOutcome::completed);
        REQUIRE(actual.state == ports::ServiceState::inactive);
        REQUIRE(fixture.trace->command_count == 1);
        REQUIRE(fixture.trace->commands[0] ==
                windows_detail::NativeServiceCommand::stop);
        REQUIRE(fixture.trace->sleep_count == 1);
        require_windows_handles_closed(*fixture.trace);
    }
}

TEST_CASE("Windows ServiceControl restarts active and inactive services") {
    SECTION("active") {
        WindowsControlFixture fixture;
        fixture.trace->statuses = {
            windows_status(SERVICE_RUNNING),
            windows_status(SERVICE_STOP_PENDING, 500),
            windows_status(SERVICE_STOPPED),
            windows_status(SERVICE_STOPPED),
            windows_status(SERVICE_START_PENDING, 500),
            windows_status(SERVICE_RUNNING),
        };
        const auto actual = fixture.control->perform(
            request(ports::ServiceAction::restart));
        REQUIRE(actual.outcome == ports::ServiceOutcome::completed);
        REQUIRE(actual.state == ports::ServiceState::active);
        REQUIRE(fixture.trace->command_count == 1);
        REQUIRE(fixture.trace->commands[0] ==
                windows_detail::NativeServiceCommand::stop);
        REQUIRE(fixture.trace->start_calls == 1);
        REQUIRE(fixture.trace->access ==
                windows_detail::NativeServiceAccess::restart);
        require_windows_handles_closed(*fixture.trace);
    }

    SECTION("inactive") {
        WindowsControlFixture fixture;
        fixture.trace->statuses = {
            windows_status(SERVICE_STOPPED),
            windows_status(SERVICE_STOPPED),
            windows_status(SERVICE_RUNNING),
        };
        const auto actual = fixture.control->perform(
            request(ports::ServiceAction::restart));
        REQUIRE(actual.outcome == ports::ServiceOutcome::completed);
        REQUIRE(actual.state == ports::ServiceState::active);
        REQUIRE(fixture.trace->command_count == 0);
        REQUIRE(fixture.trace->start_calls == 1);
        require_windows_handles_closed(*fixture.trace);
    }
}

TEST_CASE("Windows ServiceControl contains command failures neutrally") {
    SECTION("start") {
        WindowsControlFixture fixture;
        fixture.trace->statuses = {windows_status(SERVICE_STOPPED)};
        fixture.trace->start_succeeds = false;
        const auto actual = fixture.control->perform(
            request(ports::ServiceAction::start));
        require_windows_failure(
            actual, ports::ServiceAction::start,
            ports::ServiceState::inactive, "windows_service_start:5");
        require_windows_handles_closed(*fixture.trace);
    }

    SECTION("stop") {
        WindowsControlFixture fixture;
        fixture.trace->statuses = {windows_status(SERVICE_RUNNING)};
        fixture.trace->stop_succeeds = false;
        const auto actual = fixture.control->perform(
            request(ports::ServiceAction::stop));
        require_windows_failure(
            actual, ports::ServiceAction::stop,
            ports::ServiceState::active, "windows_service_stop:5");
        require_windows_handles_closed(*fixture.trace);
    }

    SECTION("continue") {
        WindowsControlFixture fixture;
        fixture.trace->statuses = {windows_status(SERVICE_PAUSED)};
        fixture.trace->continue_succeeds = false;
        const auto actual = fixture.control->perform(
            request(ports::ServiceAction::start));
        require_windows_failure(
            actual, ports::ServiceAction::start,
            ports::ServiceState::inactive,
            "windows_service_continue:1061");
        require_windows_handles_closed(*fixture.trace);
    }

    SECTION("poll query retains last trustworthy state") {
        WindowsControlFixture fixture;
        fixture.trace->statuses = {
            windows_status(SERVICE_START_PENDING, 500)};
        fixture.trace->query_failure_at = 1;
        const auto actual = fixture.control->perform(
            request(ports::ServiceAction::start));
        require_windows_failure(
            actual, ports::ServiceAction::start,
            ports::ServiceState::activating, "windows_service_query:5");
        require_windows_handles_closed(*fixture.trace);
    }

    SECTION("wait failure retains last trustworthy state") {
        WindowsControlFixture fixture;
        fixture.trace->statuses = {
            windows_status(SERVICE_STOP_PENDING, 500)};
        fixture.trace->sleep_throws = true;
        const auto actual = fixture.control->perform(
            request(ports::ServiceAction::stop));
        require_windows_failure(
            actual, ports::ServiceAction::stop,
            ports::ServiceState::deactivating, "windows_service_wait");
        require_windows_handles_closed(*fixture.trace);
    }
}

TEST_CASE("Windows ServiceControl obeys timeout before command and during polling") {
    SECTION("query consumes deadline before start") {
        WindowsControlFixture fixture;
        fixture.trace->statuses = {windows_status(SERVICE_STOPPED)};
        fixture.trace->advance_on_query = 5ms;
        auto candidate = request(ports::ServiceAction::start);
        candidate.timeout = 1ms;
        const auto actual = fixture.control->perform(candidate);
        require_windows_failure(
            actual, ports::ServiceAction::start,
            ports::ServiceState::inactive, "windows_service_timeout");
        REQUIRE(fixture.trace->start_calls == 0);
        require_windows_handles_closed(*fixture.trace);
    }

    SECTION("pending wait reaches deadline") {
        WindowsControlFixture fixture;
        fixture.trace->statuses = {
            windows_status(SERVICE_START_PENDING, 5000),
            windows_status(SERVICE_RUNNING),
        };
        fixture.trace->sleep_overshoot = 1ms;
        auto candidate = request(ports::ServiceAction::start);
        candidate.timeout = 20ms;
        const auto actual = fixture.control->perform(candidate);
        require_windows_failure(
            actual, ports::ServiceAction::start,
            ports::ServiceState::activating, "windows_service_timeout");
        REQUIRE(fixture.trace->query_calls == 1);
        REQUIRE(fixture.trace->sleep_count == 1);
        REQUIRE(fixture.trace->sleeps[0] <= 20ms);
        require_windows_handles_closed(*fixture.trace);
    }
}

TEST_CASE("Windows ServiceControl restart never resets the supplied deadline") {
    WindowsControlFixture fixture;
    fixture.trace->statuses = {
        windows_status(SERVICE_RUNNING),
        windows_status(SERVICE_STOP_PENDING, 400),
        windows_status(SERVICE_STOPPED),
        windows_status(SERVICE_STOPPED),
        windows_status(SERVICE_START_PENDING, 5000),
        windows_status(SERVICE_RUNNING),
    };
    fixture.trace->sleep_overshoot = 1ms;
    auto candidate = request(ports::ServiceAction::restart);
    candidate.timeout = 60ms;

    const auto actual = fixture.control->perform(candidate);

    require_windows_failure(
        actual, ports::ServiceAction::restart,
        ports::ServiceState::activating, "windows_service_timeout");
    REQUIRE(fixture.trace->command_count == 1);
    REQUIRE(fixture.trace->start_calls == 1);
    REQUIRE(fixture.trace->sleep_count == 2);
    REQUIRE(fixture.trace->sleeps[0] == 50ms);
    REQUIRE(fixture.trace->sleeps[1] <= 10ms);
    require_windows_handles_closed(*fixture.trace);
}

TEST_CASE("Windows ServiceControl clamps wait hints inside the deadline") {
    for (const auto [hint, expected] :
         {std::pair{UINT32_C(1), 50ms},
          std::pair{UINT32_C(5000), 250ms}}) {
        WindowsControlFixture fixture;
        fixture.trace->statuses = {
            windows_status(SERVICE_START_PENDING, hint),
            windows_status(SERVICE_RUNNING),
        };
        auto candidate = request(ports::ServiceAction::start);
        candidate.timeout = 1s;

        const auto actual = fixture.control->perform(candidate);

        REQUIRE(actual.outcome == ports::ServiceOutcome::completed);
        REQUIRE(fixture.trace->sleep_count == 1);
        REQUIRE(fixture.trace->sleeps[0] == expected);
        REQUIRE(fixture.trace->sleeps[0] < candidate.timeout);
    }
}

TEST_CASE("Windows ServiceControl bounds native diagnostics through the neutral base") {
    WindowsControlFixture fixture;
    fixture.trace->service_succeeds = false;
    fixture.trace->service_error = UINT32_MAX;

    const auto actual = fixture.control->perform(
        request(ports::ServiceAction::status));

    require_windows_failure(
        actual, ports::ServiceAction::status,
        ports::ServiceState::unknown,
        "windows_service_open:4294967295");
    REQUIRE(actual.diagnostic->code.size() <=
            ports::service_diagnostic_code_max_bytes);
}

TEST_CASE("Windows ServiceControl factory yields the neutral port") {
    auto control = arena::platform::make_service_control();
    REQUIRE(control != nullptr);
}

#endif
