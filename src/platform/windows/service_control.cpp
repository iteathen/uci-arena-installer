#include "arena/platform/service_control.hpp"

#define NOMINMAX
#include <windows.h>
#include <winsvc.h>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace arena::platform {
namespace {

constexpr DWORD transition_timeout_ms = 30'000;

class ScHandle final {
public:
    explicit ScHandle(SC_HANDLE value = nullptr) noexcept : value_(value) {}
    ~ScHandle() {
        if (value_ != nullptr) {
            CloseServiceHandle(value_);
        }
    }

    ScHandle(const ScHandle&) = delete;
    ScHandle& operator=(const ScHandle&) = delete;

    ScHandle(ScHandle&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
    ScHandle& operator=(ScHandle&& other) noexcept {
        if (this != &other) {
            if (value_ != nullptr) {
                CloseServiceHandle(value_);
            }
            value_ = std::exchange(other.value_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] SC_HANDLE get() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept { return value_ != nullptr; }

private:
    SC_HANDLE value_;
};

class ServiceControlFailure final : public std::runtime_error {
public:
    ServiceControlFailure(
        std::string message,
        const DWORD code,
        const ServiceState state = ServiceState::unknown)
        : std::runtime_error(std::move(message)), code_(code), state_(state) {}

    [[nodiscard]] DWORD code() const noexcept { return code_; }
    [[nodiscard]] ServiceState state() const noexcept { return state_; }

private:
    DWORD code_;
    ServiceState state_;
};

std::wstring wide(const std::string& value) {
    const int count = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0);
    if (count <= 0) {
        throw ServiceControlFailure(
            "service name is not valid UTF-8",
            GetLastError());
    }
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            result.data(),
            count) != count) {
        throw ServiceControlFailure(
            "service name conversion failed",
            GetLastError());
    }
    return result;
}

SERVICE_STATUS_PROCESS query_service_status(const SC_HANDLE service) {
    SERVICE_STATUS_PROCESS status{};
    DWORD required = 0;
    if (!QueryServiceStatusEx(
            service,
            SC_STATUS_PROCESS_INFO,
            reinterpret_cast<LPBYTE>(&status),
            sizeof(status),
            &required)) {
        const DWORD code = GetLastError();
        throw ServiceControlFailure(
            "Windows service status query failed",
            code,
            service_state_from_windows_error(code));
    }
    return status;
}

[[nodiscard]] DWORD effective_exit_code(
    const SERVICE_STATUS_PROCESS& status) noexcept {
    return status.dwWin32ExitCode == ERROR_SERVICE_SPECIFIC_ERROR
        && status.dwServiceSpecificExitCode != 0
        ? status.dwServiceSpecificExitCode
        : status.dwWin32ExitCode;
}

[[nodiscard]] ServiceState neutral_state(
    const SERVICE_STATUS_PROCESS& status) noexcept {
    return service_state_from_windows_scm(
        status.dwCurrentState,
        effective_exit_code(status));
}

void wait_for_service_state(
    const SC_HANDLE service,
    const DWORD expected_state,
    const std::string_view action) {
    const ULONGLONG deadline = GetTickCount64() + transition_timeout_ms;
    SERVICE_STATUS_PROCESS status = query_service_status(service);
    while (status.dwCurrentState != expected_state) {
        if (expected_state == SERVICE_RUNNING
            && status.dwCurrentState == SERVICE_STOPPED
            && effective_exit_code(status) != ERROR_SUCCESS) {
            throw ServiceControlFailure(
                "Windows service failed while starting",
                effective_exit_code(status),
                ServiceState::failed);
        }
        if (GetTickCount64() >= deadline) {
            throw ServiceControlFailure(
                "Windows service transition timed out during " + std::string(action),
                ERROR_TIMEOUT,
                neutral_state(status));
        }
        const DWORD delay = std::clamp(status.dwWaitHint / 10, 50UL, 250UL);
        Sleep(delay);
        status = query_service_status(service);
    }
}

SERVICE_STATUS_PROCESS wait_for_start_resolution(const SC_HANDLE service) {
    const ULONGLONG deadline = GetTickCount64() + transition_timeout_ms;
    SERVICE_STATUS_PROCESS status = query_service_status(service);
    while (status.dwCurrentState == SERVICE_START_PENDING) {
        if (GetTickCount64() >= deadline) {
            throw ServiceControlFailure(
                "Windows service start transition timed out",
                ERROR_TIMEOUT,
                neutral_state(status));
        }
        const DWORD delay = std::clamp(status.dwWaitHint / 10, 50UL, 250UL);
        Sleep(delay);
        status = query_service_status(service);
    }
    return status;
}

void start_service(const SC_HANDLE service) {
    SERVICE_STATUS_PROCESS status = query_service_status(service);
    if (status.dwCurrentState == SERVICE_RUNNING) {
        return;
    }
    if (status.dwCurrentState == SERVICE_START_PENDING) {
        status = wait_for_start_resolution(service);
        if (status.dwCurrentState == SERVICE_RUNNING) {
            return;
        }
    }
    if (status.dwCurrentState == SERVICE_STOP_PENDING) {
        wait_for_service_state(service, SERVICE_STOPPED, "start");
        status = query_service_status(service);
    }
    if (status.dwCurrentState != SERVICE_STOPPED) {
        throw ServiceControlFailure(
            "Windows service cannot start from its current state",
            ERROR_SERVICE_CANNOT_ACCEPT_CTRL,
            neutral_state(status));
    }
    if (!StartServiceW(service, 0, nullptr)) {
        const DWORD code = GetLastError();
        if (code != ERROR_SERVICE_ALREADY_RUNNING) {
            throw ServiceControlFailure(
                "Windows service start failed",
                code,
                service_state_from_windows_error(code));
        }
    }
    wait_for_service_state(service, SERVICE_RUNNING, "start");
}

void stop_service(const SC_HANDLE service) {
    SERVICE_STATUS_PROCESS status = wait_for_start_resolution(service);
    if (status.dwCurrentState == SERVICE_STOPPED) {
        return;
    }
    if (status.dwCurrentState == SERVICE_STOP_PENDING) {
        wait_for_service_state(service, SERVICE_STOPPED, "stop");
        return;
    }
    SERVICE_STATUS control_status{};
    if (!ControlService(service, SERVICE_CONTROL_STOP, &control_status)) {
        const DWORD code = GetLastError();
        if (code != ERROR_SERVICE_NOT_ACTIVE) {
            throw ServiceControlFailure(
                "Windows service stop failed",
                code,
                service_state_from_windows_error(code));
        }
    }
    wait_for_service_state(service, SERVICE_STOPPED, "stop");
}

ServiceActionResult failed_result(
    const ServiceAction action,
    const DWORD code,
    const ServiceState state,
    const std::string_view diagnostic) {
    return service_result_from_observation(
        action,
        state,
        false,
        static_cast<std::uint32_t>(code),
        diagnostic);
}

} // namespace

ServiceActionResult perform_service_action(
    const std::string& platform_name,
    const ServiceAction action) {
    if (platform_name.empty()) {
        throw std::runtime_error("service name is required");
    }
    if (!valid_service_name(platform_name)) {
        throw std::runtime_error("service name is invalid");
    }
    if (!valid_service_action(action)) {
        throw std::runtime_error("unsupported service action");
    }

    try {
        const ScHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
        if (!manager) {
            const DWORD code = GetLastError();
            return failed_result(
                action,
                code,
                service_state_from_windows_error(code),
                "cannot open Windows Service Control Manager");
        }

        const DWORD access =
            SERVICE_QUERY_STATUS
            | (action == ServiceAction::start || action == ServiceAction::restart
                ? SERVICE_START
                : 0)
            | (action == ServiceAction::stop || action == ServiceAction::restart
                ? SERVICE_STOP
                : 0);
        const std::wstring service_name = wide(platform_name);
        const ScHandle service(OpenServiceW(manager.get(), service_name.c_str(), access));
        if (!service) {
            const DWORD code = GetLastError();
            return failed_result(
                action,
                code,
                service_state_from_windows_error(code),
                "Windows service cannot be opened");
        }

        if (action == ServiceAction::start) {
            start_service(service.get());
        } else if (action == ServiceAction::stop) {
            stop_service(service.get());
        } else if (action == ServiceAction::restart) {
            stop_service(service.get());
            start_service(service.get());
        }

        const SERVICE_STATUS_PROCESS observed = query_service_status(service.get());
        return service_result_from_observation(
            action,
            neutral_state(observed),
            true,
            effective_exit_code(observed));
    } catch (const ServiceControlFailure& failure) {
        return failed_result(
            action,
            failure.code(),
            failure.state(),
            failure.what());
    } catch (const std::exception& failure) {
        return failed_result(
            action,
            ERROR_GEN_FAILURE,
            ServiceState::unknown,
            failure.what());
    }
}

} // namespace arena::platform
