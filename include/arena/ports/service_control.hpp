#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>

namespace arena::ports {

enum class ServiceAction {
    start,
    stop,
    restart,
    status,
};

enum class ServiceOutcome {
    completed,
    failed,
};

enum class ServiceState {
    unknown,
    inactive,
    activating,
    active,
    deactivating,
    failed,
};

inline constexpr std::size_t service_name_max_utf8_bytes = 256;
inline constexpr std::size_t service_diagnostic_code_max_bytes = 64;
inline constexpr std::size_t service_diagnostic_message_max_utf8_bytes = 512;
inline constexpr auto service_timeout_minimum = std::chrono::milliseconds{1};
inline constexpr auto service_timeout_maximum = std::chrono::seconds{120};
inline constexpr auto service_timeout_default = std::chrono::seconds{30};

struct ServiceDiagnostic {
    std::string code;
    std::string message;
};

struct ServiceRequest {
    std::string platform_name;
    ServiceAction action;
    std::chrono::milliseconds timeout{service_timeout_default};
};

struct ServiceResult {
    ServiceAction action;
    ServiceOutcome outcome;
    ServiceState state;
    std::optional<ServiceDiagnostic> diagnostic;
};

class ServiceControl {
  public:
    virtual ~ServiceControl() = default;

    [[nodiscard]] ServiceResult perform(const ServiceRequest& request);

  protected:
    [[nodiscard]] virtual ServiceResult perform_validated(
        const ServiceRequest& request,
        std::chrono::steady_clock::time_point deadline) = 0;
};

} // namespace arena::ports
