#include "arena/platform/service_control.hpp"

#include "service_control_private.hpp"

#define NOMINMAX
#include <windows.h>
#include <winsvc.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace arena::platform {
namespace {

namespace ports = arena::ports;
namespace win = arena::platform::windows_detail;
using Clock = std::chrono::steady_clock;

[[nodiscard]] win::NativeServiceHandle native_handle(
    const SC_HANDLE handle) noexcept {
    return reinterpret_cast<win::NativeServiceHandle>(handle);
}

[[nodiscard]] SC_HANDLE scm_handle(
    const win::NativeServiceHandle handle) noexcept {
    return reinterpret_cast<SC_HANDLE>(handle);
}

class NativeServiceControlBackend final : public win::ServiceControlBackend {
  public:
    [[nodiscard]] win::NativeServiceHandle
    open_manager(std::uint32_t& error) noexcept override {
        const SC_HANDLE manager =
            OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (manager == nullptr) error = GetLastError();
        return native_handle(manager);
    }

    [[nodiscard]] win::NativeServiceHandle open_service(
        const win::NativeServiceHandle manager,
        const std::wstring& platform_name,
        const win::NativeServiceAccess access,
        std::uint32_t& error) noexcept override {
        DWORD desired = SERVICE_QUERY_STATUS;
        switch (access) {
        case win::NativeServiceAccess::status:
            break;
        case win::NativeServiceAccess::start:
            desired |= SERVICE_START | SERVICE_PAUSE_CONTINUE;
            break;
        case win::NativeServiceAccess::stop:
            desired |= SERVICE_STOP;
            break;
        case win::NativeServiceAccess::restart:
            desired |= SERVICE_START | SERVICE_STOP;
            break;
        }
        const SC_HANDLE service = OpenServiceW(
            scm_handle(manager), platform_name.c_str(), desired);
        if (service == nullptr) error = GetLastError();
        return native_handle(service);
    }

    [[nodiscard]] bool query(
        const win::NativeServiceHandle service,
        win::NativeServiceStatus& status,
        std::uint32_t& error) noexcept override {
        SERVICE_STATUS_PROCESS native{};
        DWORD required{};
        if (!QueryServiceStatusEx(
                scm_handle(service), SC_STATUS_PROCESS_INFO,
                reinterpret_cast<LPBYTE>(&native), sizeof(native),
                &required)) {
            error = GetLastError();
            return false;
        }
        status = {
            native.dwCurrentState,
            native.dwWin32ExitCode,
            native.dwServiceSpecificExitCode,
            native.dwWaitHint,
        };
        return true;
    }

    [[nodiscard]] bool start(
        const win::NativeServiceHandle service,
        std::uint32_t& error) noexcept override {
        if (StartServiceW(scm_handle(service), 0, nullptr)) return true;
        error = GetLastError();
        return false;
    }

    [[nodiscard]] bool command(
        const win::NativeServiceHandle service,
        const win::NativeServiceCommand command,
        std::uint32_t& error) noexcept override {
        SERVICE_STATUS ignored{};
        const DWORD native_command =
            command == win::NativeServiceCommand::stop
                ? SERVICE_CONTROL_STOP
                : SERVICE_CONTROL_CONTINUE;
        if (ControlService(scm_handle(service), native_command, &ignored)) {
            return true;
        }
        error = GetLastError();
        return false;
    }

    void close(const win::NativeServiceHandle handle) noexcept override {
        if (handle != win::invalid_service_handle) {
            CloseServiceHandle(scm_handle(handle));
        }
    }

    [[nodiscard]] Clock::time_point now() const noexcept override {
        return Clock::now();
    }

    void sleep_for(const std::chrono::milliseconds duration) override {
        std::this_thread::sleep_for(duration);
    }
};

class BackendHandle final {
  public:
    BackendHandle(
        win::ServiceControlBackend& backend,
        const win::NativeServiceHandle handle) noexcept
        : backend_(&backend), handle_(handle) {}

    ~BackendHandle() {
        if (backend_ != nullptr) backend_->close(handle_);
    }

    BackendHandle(const BackendHandle&) = delete;
    BackendHandle& operator=(const BackendHandle&) = delete;

    [[nodiscard]] win::NativeServiceHandle get() const noexcept {
        return handle_;
    }

  private:
    win::ServiceControlBackend* backend_{};
    win::NativeServiceHandle handle_{win::invalid_service_handle};
};

[[nodiscard]] ports::ServiceDiagnostic diagnostic(std::string code) {
    return {std::move(code), {}};
}

[[nodiscard]] std::string error_code(
    std::string category,
    const std::uint32_t error) {
    category += ':';
    category += std::to_string(error);
    return category;
}

[[nodiscard]] ports::ServiceResult failed(
    const ports::ServiceAction action,
    const ports::ServiceState state,
    std::string code) {
    return {action, ports::ServiceOutcome::failed, state,
            diagnostic(std::move(code))};
}

[[nodiscard]] ports::ServiceResult completed(
    const ports::ServiceAction action,
    const ports::ServiceState state,
    std::optional<ports::ServiceDiagnostic> detail = std::nullopt) {
    return {action, ports::ServiceOutcome::completed, state,
            std::move(detail)};
}

[[nodiscard]] std::optional<ports::ServiceState> neutral_state(
    const std::uint32_t state) noexcept {
    switch (state) {
    case SERVICE_STOPPED:
    case SERVICE_PAUSED:
        return ports::ServiceState::inactive;
    case SERVICE_START_PENDING:
    case SERVICE_CONTINUE_PENDING:
        return ports::ServiceState::activating;
    case SERVICE_RUNNING:
        return ports::ServiceState::active;
    case SERVICE_STOP_PENDING:
    case SERVICE_PAUSE_PENDING:
        return ports::ServiceState::deactivating;
    default:
        return std::nullopt;
    }
}

[[nodiscard]] win::NativeServiceAccess required_access(
    const ports::ServiceAction action) noexcept {
    switch (action) {
    case ports::ServiceAction::start:
        return win::NativeServiceAccess::start;
    case ports::ServiceAction::stop:
        return win::NativeServiceAccess::stop;
    case ports::ServiceAction::restart:
        return win::NativeServiceAccess::restart;
    case ports::ServiceAction::status:
        return win::NativeServiceAccess::status;
    }
    return win::NativeServiceAccess::status;
}

[[nodiscard]] std::optional<std::wstring> wide_service_name(
    const std::string& value,
    std::uint32_t& error) {
    const int count = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) {
        error = GetLastError();
        return std::nullopt;
    }
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), result.data(), count) != count) {
        error = GetLastError();
        return std::nullopt;
    }
    return result;
}

class WindowsServiceControl final : public ports::ServiceControl {
  public:
    explicit WindowsServiceControl(
        std::unique_ptr<win::ServiceControlBackend> backend)
        : backend_(std::move(backend)) {
        if (!backend_) {
            throw std::invalid_argument("Windows service backend is required");
        }
    }

  protected:
    [[nodiscard]] ports::ServiceResult perform_validated(
        const ports::ServiceRequest& request,
        const Clock::time_point deadline) override {
        if (expired(deadline)) return timeout(request.action);

        std::uint32_t error{};
        auto platform_name = wide_service_name(request.platform_name, error);
        if (!platform_name.has_value()) {
            return failed(request.action, ports::ServiceState::unknown,
                          error_code("windows_service_name", error));
        }

        const auto manager_value = backend_->open_manager(error);
        if (manager_value == win::invalid_service_handle) {
            return failed(request.action, ports::ServiceState::unknown,
                          error_code("windows_scm_open", error));
        }
        BackendHandle manager{*backend_, manager_value};
        if (expired(deadline)) return timeout(request.action);

        const auto service_value = backend_->open_service(
            manager.get(), *platform_name, required_access(request.action),
            error);
        if (service_value == win::invalid_service_handle) {
            return failed(request.action, ports::ServiceState::unknown,
                          error_code("windows_service_open", error));
        }
        BackendHandle service{*backend_, service_value};
        if (expired(deadline)) return timeout(request.action);

        switch (request.action) {
        case ports::ServiceAction::start:
            return ensure_active(service.get(), request.action, deadline);
        case ports::ServiceAction::stop:
            return ensure_inactive(service.get(), request.action, deadline);
        case ports::ServiceAction::restart: {
            const auto stopped =
                ensure_inactive(service.get(), request.action, deadline);
            if (stopped.outcome != ports::ServiceOutcome::completed) {
                return stopped;
            }
            return ensure_active(service.get(), request.action, deadline);
        }
        case ports::ServiceAction::status:
            return status(service.get(), request.action, deadline);
        }
        return failed(request.action, ports::ServiceState::unknown,
                      "windows_action_unsupported");
    }

  private:
    [[nodiscard]] bool expired(const Clock::time_point deadline) const noexcept {
        return backend_->now() >= deadline;
    }

    [[nodiscard]] ports::ServiceResult timeout(
        const ports::ServiceAction action,
        const ports::ServiceState state =
            ports::ServiceState::unknown) const {
        return failed(action, state, "windows_service_timeout");
    }

    [[nodiscard]] std::optional<win::NativeServiceStatus> query(
        const win::NativeServiceHandle service,
        const ports::ServiceAction action,
        const Clock::time_point deadline,
        ports::ServiceResult& failure,
        const ports::ServiceState last_state =
            ports::ServiceState::unknown) {
        if (expired(deadline)) {
            failure = timeout(action, last_state);
            return std::nullopt;
        }
        win::NativeServiceStatus status{};
        std::uint32_t error{};
        if (!backend_->query(service, status, error)) {
            failure = failed(action, last_state,
                             error_code("windows_service_query", error));
            return std::nullopt;
        }
        if (expired(deadline)) {
            failure = timeout(
                action,
                neutral_state(status.current_state)
                    .value_or(ports::ServiceState::unknown));
            return std::nullopt;
        }
        return status;
    }

    [[nodiscard]] bool poll(
        const win::NativeServiceStatus& status,
        const ports::ServiceAction action,
        const Clock::time_point deadline,
        ports::ServiceResult& failure) {
        const auto now = backend_->now();
        if (now >= deadline) {
            failure = timeout(
                action,
                neutral_state(status.current_state)
                    .value_or(ports::ServiceState::unknown));
            return false;
        }
        const auto hinted = std::chrono::milliseconds{std::clamp(
            status.wait_hint_ms / 10U, UINT32_C(50), UINT32_C(250))};
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        if (remaining <= std::chrono::milliseconds::zero()) {
            failure = timeout(
                action,
                neutral_state(status.current_state)
                    .value_or(ports::ServiceState::unknown));
            return false;
        }
        try {
            backend_->sleep_for((std::min)(hinted, remaining));
        } catch (const std::bad_alloc&) {
            throw;
        } catch (...) {
            failure = failed(action,
                             neutral_state(status.current_state)
                                 .value_or(ports::ServiceState::unknown),
                             "windows_service_wait");
            return false;
        }
        if (expired(deadline)) {
            failure = timeout(
                action,
                neutral_state(status.current_state)
                    .value_or(ports::ServiceState::unknown));
            return false;
        }
        return true;
    }

    [[nodiscard]] bool issue_start(
        const win::NativeServiceHandle service,
        const ports::ServiceAction action,
        const Clock::time_point deadline,
        ports::ServiceResult& failure) {
        if (expired(deadline)) {
            failure = timeout(action, ports::ServiceState::inactive);
            return false;
        }
        std::uint32_t error{};
        if (!backend_->start(service, error) &&
            error != ERROR_SERVICE_ALREADY_RUNNING) {
            failure = failed(action, ports::ServiceState::inactive,
                             error_code("windows_service_start", error));
            return false;
        }
        if (expired(deadline)) {
            failure = timeout(action, ports::ServiceState::unknown);
            return false;
        }
        return true;
    }

    [[nodiscard]] bool issue_command(
        const win::NativeServiceHandle service,
        const win::NativeServiceCommand command,
        const ports::ServiceAction action,
        const ports::ServiceState state,
        const Clock::time_point deadline,
        ports::ServiceResult& failure) {
        if (expired(deadline)) {
            failure = timeout(action, state);
            return false;
        }
        std::uint32_t error{};
        const bool accepted = backend_->command(service, command, error);
        const bool inactive_stop =
            command == win::NativeServiceCommand::stop &&
            error == ERROR_SERVICE_NOT_ACTIVE;
        if (!accepted && !inactive_stop) {
            failure = failed(
                action, state,
                error_code(
                    command == win::NativeServiceCommand::stop
                        ? "windows_service_stop"
                        : "windows_service_continue",
                    error));
            return false;
        }
        if (expired(deadline)) {
            failure = timeout(action, state);
            return false;
        }
        return true;
    }

    [[nodiscard]] ports::ServiceResult status(
        const win::NativeServiceHandle service,
        const ports::ServiceAction action,
        const Clock::time_point deadline) {
        auto failure = timeout(action);
        const auto observed = query(service, action, deadline, failure);
        if (!observed.has_value()) return failure;

        if (observed->current_state == SERVICE_STOPPED &&
            observed->win32_exit_code != ERROR_SUCCESS) {
            if (observed->win32_exit_code == ERROR_SERVICE_SPECIFIC_ERROR &&
                observed->service_specific_exit_code != 0) {
                return failed(
                    action, ports::ServiceState::failed,
                    error_code("windows_service_exit_specific",
                               observed->service_specific_exit_code));
            }
            return failed(
                action, ports::ServiceState::failed,
                error_code("windows_service_exit",
                           observed->win32_exit_code));
        }

        const auto state = neutral_state(observed->current_state);
        if (!state.has_value()) {
            return failed(
                action, ports::ServiceState::unknown,
                error_code("windows_service_state",
                           observed->current_state));
        }
        if (observed->current_state == SERVICE_PAUSED) {
            return completed(action, *state,
                             diagnostic("windows_service_paused"));
        }
        return completed(action, *state);
    }

    [[nodiscard]] ports::ServiceResult ensure_active(
        const win::NativeServiceHandle service,
        const ports::ServiceAction action,
        const Clock::time_point deadline) {
        bool start_requested{};
        bool continue_requested{};
        auto last_state = ports::ServiceState::unknown;
        while (true) {
            auto failure = timeout(action);
            const auto observed =
                query(service, action, deadline, failure, last_state);
            if (!observed.has_value()) return failure;
            last_state = neutral_state(observed->current_state)
                             .value_or(ports::ServiceState::unknown);

            switch (observed->current_state) {
            case SERVICE_RUNNING:
                return completed(action, ports::ServiceState::active);
            case SERVICE_STOPPED:
                if (!start_requested) {
                    if (!issue_start(service, action, deadline, failure)) {
                        return failure;
                    }
                    start_requested = true;
                    continue;
                }
                break;
            case SERVICE_PAUSED:
                if (!continue_requested) {
                    if (!issue_command(
                            service,
                            win::NativeServiceCommand::continue_service,
                            action, ports::ServiceState::inactive, deadline,
                            failure)) {
                        return failure;
                    }
                    continue_requested = true;
                    continue;
                }
                break;
            case SERVICE_START_PENDING:
            case SERVICE_CONTINUE_PENDING:
            case SERVICE_STOP_PENDING:
            case SERVICE_PAUSE_PENDING:
                break;
            default:
                return failed(
                    action, ports::ServiceState::unknown,
                    error_code("windows_service_state",
                               observed->current_state));
            }
            if (!poll(*observed, action, deadline, failure)) return failure;
        }
    }

    [[nodiscard]] ports::ServiceResult ensure_inactive(
        const win::NativeServiceHandle service,
        const ports::ServiceAction action,
        const Clock::time_point deadline) {
        bool stop_requested{};
        auto last_state = ports::ServiceState::unknown;
        while (true) {
            auto failure = timeout(action);
            const auto observed =
                query(service, action, deadline, failure, last_state);
            if (!observed.has_value()) return failure;
            last_state = neutral_state(observed->current_state)
                             .value_or(ports::ServiceState::unknown);

            switch (observed->current_state) {
            case SERVICE_STOPPED:
                return completed(action, ports::ServiceState::inactive);
            case SERVICE_RUNNING:
            case SERVICE_PAUSED:
                if (!stop_requested) {
                    const auto state = neutral_state(observed->current_state)
                                           .value_or(ports::ServiceState::unknown);
                    if (!issue_command(
                            service, win::NativeServiceCommand::stop, action,
                            state, deadline, failure)) {
                        return failure;
                    }
                    stop_requested = true;
                    continue;
                }
                break;
            case SERVICE_START_PENDING:
            case SERVICE_CONTINUE_PENDING:
            case SERVICE_STOP_PENDING:
            case SERVICE_PAUSE_PENDING:
                break;
            default:
                return failed(
                    action, ports::ServiceState::unknown,
                    error_code("windows_service_state",
                               observed->current_state));
            }
            if (!poll(*observed, action, deadline, failure)) return failure;
        }
    }

    std::unique_ptr<win::ServiceControlBackend> backend_;
};

// Transitional legacy implementation retained until SC-04.
[[nodiscard]] std::wstring legacy_wide(const std::string& value) {
    const int count = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) {
        throw std::runtime_error("service name is not valid UTF-8");
    }
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), count);
    return result;
}

constexpr DWORD legacy_transition_timeout_ms = 30'000;

[[nodiscard]] SERVICE_STATUS_PROCESS legacy_query_service_status(
    const SC_HANDLE service) {
    SERVICE_STATUS_PROCESS status{};
    DWORD required{};
    if (!QueryServiceStatusEx(
            service, SC_STATUS_PROCESS_INFO,
            reinterpret_cast<LPBYTE>(&status), sizeof(status), &required)) {
        throw std::runtime_error(
            "Windows service status query failed with error " +
            std::to_string(GetLastError()));
    }
    return status;
}

void legacy_wait_for_service_state(
    const SC_HANDLE service,
    const DWORD expected_state,
    const std::string& action) {
    const ULONGLONG deadline =
        GetTickCount64() + legacy_transition_timeout_ms;
    auto status = legacy_query_service_status(service);
    while (status.dwCurrentState != expected_state) {
        if (GetTickCount64() >= deadline) {
            throw std::runtime_error(
                "Windows service " + action + " timed out in SCM state " +
                std::to_string(status.dwCurrentState));
        }
        const DWORD delay =
            std::clamp(status.dwWaitHint / 10, 50UL, 250UL);
        Sleep(delay);
        status = legacy_query_service_status(service);
    }
}

[[nodiscard]] SERVICE_STATUS_PROCESS legacy_wait_for_start_resolution(
    const SC_HANDLE service) {
    const ULONGLONG deadline =
        GetTickCount64() + legacy_transition_timeout_ms;
    auto status = legacy_query_service_status(service);
    while (status.dwCurrentState == SERVICE_START_PENDING) {
        if (GetTickCount64() >= deadline) {
            throw std::runtime_error(
                "Windows service start timed out in SCM state " +
                std::to_string(status.dwCurrentState));
        }
        const DWORD delay =
            std::clamp(status.dwWaitHint / 10, 50UL, 250UL);
        Sleep(delay);
        status = legacy_query_service_status(service);
    }
    return status;
}

void legacy_start_service(const SC_HANDLE service) {
    auto status = legacy_query_service_status(service);
    if (status.dwCurrentState == SERVICE_RUNNING) return;
    if (status.dwCurrentState == SERVICE_START_PENDING) {
        status = legacy_wait_for_start_resolution(service);
        if (status.dwCurrentState == SERVICE_RUNNING) return;
    }
    if (status.dwCurrentState == SERVICE_STOP_PENDING) {
        legacy_wait_for_service_state(service, SERVICE_STOPPED, "stop");
        status = legacy_query_service_status(service);
    }
    if (status.dwCurrentState != SERVICE_STOPPED) {
        throw std::runtime_error(
            "Windows service cannot start from SCM state " +
            std::to_string(status.dwCurrentState));
    }
    if (!StartServiceW(service, 0, nullptr)) {
        throw std::runtime_error(
            "Windows service start failed with error " +
            std::to_string(GetLastError()));
    }
    legacy_wait_for_service_state(service, SERVICE_RUNNING, "start");
}

void legacy_stop_service(const SC_HANDLE service) {
    auto status = legacy_wait_for_start_resolution(service);
    if (status.dwCurrentState == SERVICE_STOPPED) return;
    if (status.dwCurrentState == SERVICE_STOP_PENDING) {
        legacy_wait_for_service_state(service, SERVICE_STOPPED, "stop");
        return;
    }
    SERVICE_STATUS control_status{};
    if (!ControlService(service, SERVICE_CONTROL_STOP, &control_status) &&
        GetLastError() != ERROR_SERVICE_NOT_ACTIVE) {
        throw std::runtime_error(
            "Windows service stop failed with error " +
            std::to_string(GetLastError()));
    }
    legacy_wait_for_service_state(service, SERVICE_STOPPED, "stop");
}

} // namespace

std::unique_ptr<arena::ports::ServiceControl> make_service_control() {
    return std::make_unique<WindowsServiceControl>(
        std::make_unique<NativeServiceControlBackend>());
}

int perform_service_action(
    const std::string& platform_name,
    const std::string& action) {
    SC_HANDLE manager =
        OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr) {
        throw std::runtime_error(
            "cannot open Windows Service Control Manager");
    }
    const DWORD access =
        SERVICE_QUERY_STATUS |
        (action == "start" || action == "restart" ? SERVICE_START : 0) |
        (action == "stop" || action == "restart" ? SERVICE_STOP : 0);
    const std::wstring service_name = legacy_wide(platform_name);
    SC_HANDLE service =
        OpenServiceW(manager, service_name.c_str(), access);
    if (service == nullptr) {
        CloseServiceHandle(manager);
        throw std::runtime_error(
            "allowlisted Windows service cannot be opened");
    }

    try {
        if (action == "start") {
            legacy_start_service(service);
        } else if (action == "stop") {
            legacy_stop_service(service);
        } else if (action == "restart") {
            legacy_stop_service(service);
            legacy_start_service(service);
        } else if (action == "status") {
            const int state = static_cast<int>(
                legacy_query_service_status(service).dwCurrentState);
            CloseServiceHandle(service);
            CloseServiceHandle(manager);
            return state;
        } else {
            throw std::runtime_error(
                "unsupported Windows service action: " + action);
        }
        CloseServiceHandle(service);
        CloseServiceHandle(manager);
        return 0;
    } catch (...) {
        CloseServiceHandle(service);
        CloseServiceHandle(manager);
        throw;
    }
}

namespace windows_detail {

std::unique_ptr<arena::ports::ServiceControl>
make_service_control_for_testing(
    std::unique_ptr<ServiceControlBackend> backend) {
    return std::make_unique<WindowsServiceControl>(std::move(backend));
}

} // namespace windows_detail

} // namespace arena::platform
