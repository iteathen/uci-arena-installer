#pragma once

#include "arena/ports/service_control.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace arena::platform::windows_detail {

using NativeServiceHandle = std::uintptr_t;
inline constexpr NativeServiceHandle invalid_service_handle{};

struct NativeServiceStatus {
    std::uint32_t current_state{};
    std::uint32_t win32_exit_code{};
    std::uint32_t service_specific_exit_code{};
    std::uint32_t wait_hint_ms{};
};

enum class NativeServiceAccess {
    status,
    start,
    stop,
    restart,
};

enum class NativeServiceCommand {
    stop,
    continue_service,
};

class ServiceControlBackend {
  public:
    virtual ~ServiceControlBackend() = default;

    [[nodiscard]] virtual NativeServiceHandle
    open_manager(std::uint32_t& error) noexcept = 0;
    [[nodiscard]] virtual NativeServiceHandle open_service(
        NativeServiceHandle manager,
        const std::wstring& platform_name,
        NativeServiceAccess access,
        std::uint32_t& error) noexcept = 0;
    [[nodiscard]] virtual bool query(
        NativeServiceHandle service,
        NativeServiceStatus& status,
        std::uint32_t& error) noexcept = 0;
    [[nodiscard]] virtual bool start(
        NativeServiceHandle service,
        std::uint32_t& error) noexcept = 0;
    [[nodiscard]] virtual bool command(
        NativeServiceHandle service,
        NativeServiceCommand command,
        std::uint32_t& error) noexcept = 0;
    virtual void close(NativeServiceHandle handle) noexcept = 0;
    [[nodiscard]] virtual std::chrono::steady_clock::time_point
    now() const noexcept = 0;
    virtual void sleep_for(std::chrono::milliseconds duration) = 0;
};

[[nodiscard]] std::unique_ptr<arena::ports::ServiceControl>
make_service_control_for_testing(
    std::unique_ptr<ServiceControlBackend> backend);

} // namespace arena::platform::windows_detail
