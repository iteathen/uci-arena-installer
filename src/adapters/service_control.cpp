#include "arena/ports/service_control.hpp"

#include <algorithm>
#include <array>
#include <new>
#include <string_view>

namespace arena::ports {
namespace {

[[nodiscard]] bool valid_utf8(const std::string_view value) noexcept {
    std::size_t index = 0;
    while (index < value.size()) {
        const auto first = static_cast<unsigned char>(value[index]);
        if (first <= 0x7f) {
            ++index;
            continue;
        }

        std::size_t length{};
        std::uint32_t code_point{};
        if ((first & 0xe0) == 0xc0) {
            length = 2;
            code_point = first & 0x1f;
        } else if ((first & 0xf0) == 0xe0) {
            length = 3;
            code_point = first & 0x0f;
        } else if ((first & 0xf8) == 0xf0) {
            length = 4;
            code_point = first & 0x07;
        } else {
            return false;
        }
        if (index + length > value.size()) {
            return false;
        }
        for (std::size_t offset = 1; offset < length; ++offset) {
            const auto next = static_cast<unsigned char>(value[index + offset]);
            if ((next & 0xc0) != 0x80) {
                return false;
            }
            code_point = (code_point << 6) | (next & 0x3f);
        }
        if ((length == 2 && code_point < 0x80) ||
            (length == 3 && code_point < 0x800) ||
            (length == 4 && code_point < 0x10000) ||
            code_point > 0x10ffff ||
            (code_point >= 0xd800 && code_point <= 0xdfff)) {
            return false;
        }
        index += length;
    }
    return true;
}

[[nodiscard]] bool contains_control(const std::string_view value) noexcept {
    return value.find('\0') != std::string_view::npos ||
           value.find('\r') != std::string_view::npos ||
           value.find('\n') != std::string_view::npos;
}

[[nodiscard]] bool valid_action(const ServiceAction action) noexcept {
    switch (action) {
    case ServiceAction::start:
    case ServiceAction::stop:
    case ServiceAction::restart:
    case ServiceAction::status:
        return true;
    }
    return false;
}

[[nodiscard]] bool valid_state(const ServiceState state) noexcept {
    switch (state) {
    case ServiceState::unknown:
    case ServiceState::inactive:
    case ServiceState::activating:
    case ServiceState::active:
    case ServiceState::deactivating:
    case ServiceState::failed:
        return true;
    }
    return false;
}

[[nodiscard]] bool valid_code(const std::string_view code) noexcept {
    return !code.empty() && code.size() <= service_diagnostic_code_max_bytes &&
           std::ranges::all_of(code, [](const unsigned char value) {
               return (value >= 'a' && value <= 'z') ||
                      (value >= 'A' && value <= 'Z') ||
                      (value >= '0' && value <= '9') || value == '_' ||
                      value == '.' || value == ':' || value == '-';
           });
}

[[nodiscard]] bool valid_diagnostic(const ServiceDiagnostic& diagnostic) noexcept {
    return valid_code(diagnostic.code) &&
           diagnostic.message.size() <= service_diagnostic_message_max_utf8_bytes &&
           !contains_control(diagnostic.message) && valid_utf8(diagnostic.message);
}

[[nodiscard]] bool valid_request(const ServiceRequest& request) noexcept {
    return !request.platform_name.empty() &&
           request.platform_name.size() <= service_name_max_utf8_bytes &&
           !contains_control(request.platform_name) && valid_utf8(request.platform_name) &&
           valid_action(request.action) && request.timeout >= service_timeout_minimum &&
           request.timeout <= service_timeout_maximum;
}

[[nodiscard]] bool valid_result(const ServiceRequest& request,
                                const ServiceResult& result) noexcept {
    if (result.action != request.action || !valid_state(result.state) ||
        (result.diagnostic && !valid_diagnostic(*result.diagnostic))) {
        return false;
    }
    switch (result.outcome) {
    case ServiceOutcome::failed:
        return result.diagnostic.has_value();
    case ServiceOutcome::completed:
        switch (request.action) {
        case ServiceAction::start:
        case ServiceAction::restart:
            return result.state == ServiceState::active;
        case ServiceAction::stop:
            return result.state == ServiceState::inactive;
        case ServiceAction::status:
            return result.state != ServiceState::unknown;
        }
        return false;
    }
    return false;
}

[[nodiscard]] ServiceResult failure(const ServiceAction action,
                                    std::string code) {
    return {action, ServiceOutcome::failed, ServiceState::unknown,
            ServiceDiagnostic{std::move(code), {}}};
}

} // namespace

ServiceResult ServiceControl::perform(const ServiceRequest& request) {
    if (!valid_request(request)) {
        return failure(request.action, "invalid_request");
    }

    const auto deadline = std::chrono::steady_clock::now() + request.timeout;
    ServiceResult result;
    try {
        result = perform_validated(request, deadline);
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception&) {
        return failure(request.action, "adapter_exception");
    }
    if (!valid_result(request, result)) {
        return failure(request.action, "adapter_contract_violation");
    }
    return result;
}

} // namespace arena::ports
