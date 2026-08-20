#include "arena/ports/provider_lifecycle.hpp"

#include <algorithm>
#include <array>
#include <new>
#include <stdexcept>
#include <string_view>

namespace arena::ports {
namespace {
using namespace std::literals;

constexpr std::size_t kComponentIdBytes = 128;
constexpr std::size_t kComponentVersionBytes = 64;
constexpr std::size_t kArguments = 32;
constexpr std::size_t kArgumentBytes = 1024;
constexpr std::size_t kRawResultBytes = 65536;
constexpr std::size_t kReasonIdBytes = 64;
constexpr std::size_t kReasonMessageBytes = 512;

const ProviderLifecycleObserved kUnknown{
    ProviderInstallationObservation::unknown,
    ProviderActivityObservation::unknown,
    ProviderConfigurationObservation::unknown,
    ProviderReplacementObservation::unknown};

bool valid_utf8(const std::string_view value) noexcept {
    std::size_t i = 0;
    while (i < value.size()) {
        const auto c = static_cast<unsigned char>(value[i]);
        if (c <= 0x7f) { ++i; continue; }
        std::size_t length{};
        std::uint32_t code{};
        if ((c & 0xe0) == 0xc0) { length = 2; code = c & 0x1f; }
        else if ((c & 0xf0) == 0xe0) { length = 3; code = c & 0x0f; }
        else if ((c & 0xf8) == 0xf0) { length = 4; code = c & 0x07; }
        else return false;
        if (i + length > value.size()) return false;
        for (std::size_t j = 1; j < length; ++j) {
            const auto next = static_cast<unsigned char>(value[i + j]);
            if ((next & 0xc0) != 0x80) return false;
            code = (code << 6) | (next & 0x3f);
        }
        if ((length == 2 && code < 0x80) || (length == 3 && code < 0x800) ||
            (length == 4 && code < 0x10000) || code > 0x10ffff ||
            (code >= 0xd800 && code <= 0xdfff)) return false;
        i += length;
    }
    return true;
}

bool valid_identifier(const std::string_view value, const std::size_t maximum) {
    return !value.empty() && value.size() <= maximum &&
           std::ranges::all_of(value, [](const unsigned char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
           });
}

bool valid_reason_id(const std::string_view value) {
    return !value.empty() && value.size() <= kReasonIdBytes &&
           std::ranges::all_of(value, [](const unsigned char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == ':';
           });
}

bool valid_reason(const ProviderLifecycleReason& reason) {
    return valid_reason_id(reason.id) && reason.message.size() <= kReasonMessageBytes &&
           valid_utf8(reason.message) && reason.message.find('\0') == std::string::npos &&
           reason.message.find('\r') == std::string::npos &&
           reason.message.find('\n') == std::string::npos;
}

std::string trusted_identity(const std::string& value, const std::size_t maximum) {
    return valid_identifier(value, maximum) ? value : "unknown";
}

ProviderLifecycleResult failed(const ProviderLifecycleRequest& request,
                               std::string id, std::string message = {}) {
    if (message.size() > kReasonMessageBytes) message.resize(kReasonMessageBytes);
    message.erase(std::remove(message.begin(), message.end(), '\r'), message.end());
    message.erase(std::remove(message.begin(), message.end(), '\n'), message.end());
    message.erase(std::remove(message.begin(), message.end(), '\0'), message.end());
    return {trusted_identity(request.component_id, kComponentIdBytes),
            trusted_identity(request.component_version, kComponentVersionBytes),
            request.operation, ProviderLifecycleDisposition::failed, kUnknown,
            ProviderLifecycleReason{std::move(id), std::move(message)}};
}

bool valid_path(const std::filesystem::path& path) {
    if (path.empty()) return false;
    try {
        const auto encoded = path.generic_u8string();
        const std::string bytes{reinterpret_cast<const char*>(encoded.data()), encoded.size()};
        return !bytes.empty() && bytes.find('\0') == std::string::npos && valid_utf8(bytes);
    } catch (const std::bad_alloc&) { throw; }
    catch (...) { return false; }
}

bool valid_request(const ProviderLifecycleRequest& request) {
    if (!valid_identifier(request.component_id, kComponentIdBytes) ||
        !valid_identifier(request.component_version, kComponentVersionBytes) ||
        !valid_path(request.executable) || !valid_path(request.working_directory) ||
        request.arguments.size() > kArguments || request.timeout < std::chrono::milliseconds{1} ||
        request.timeout > std::chrono::seconds{120}) return false;
    for (const auto& argument : request.arguments)
        if (argument.size() > kArgumentBytes || argument.find('\0') != std::string::npos ||
            !valid_utf8(argument)) return false;
    if (request.policy.wire_protocol == ProviderLifecycleWireProtocol::typed_v3 &&
        (request.policy.legacy_envelope != ProviderLegacyResultEnvelope::root ||
         !request.policy.require_configuration_match)) return false;
    return true;
}

template <typename Enum, std::size_t N>
Enum parse_enum(const nlohmann::json& value,
                const std::array<std::pair<std::string_view, Enum>, N>& entries) {
    if (!value.is_string()) throw std::invalid_argument("lifecycle enum is not a string");
    const auto text = value.get<std::string>();
    for (const auto& [name, item] : entries) if (text == name) return item;
    throw std::invalid_argument("lifecycle enum value is invalid");
}

std::string_view operation_text(const ProviderLifecycleOperation value) {
    switch (value) {
    case ProviderLifecycleOperation::install: return "install";
    case ProviderLifecycleOperation::uninstall: return "uninstall";
    case ProviderLifecycleOperation::pre_replace: return "pre_replace";
    case ProviderLifecycleOperation::post_replace: return "post_replace";
    }
    throw std::invalid_argument("invalid lifecycle operation");
}

std::string_view disposition_text(const ProviderLifecycleDisposition value) {
    switch (value) {
    case ProviderLifecycleDisposition::ready: return "ready";
    case ProviderLifecycleDisposition::safe_inactive: return "safe_inactive";
    case ProviderLifecycleDisposition::failed: return "failed";
    }
    throw std::invalid_argument("invalid lifecycle disposition");
}

std::string_view installation_text(const ProviderInstallationObservation value) {
    switch (value) {
    case ProviderInstallationObservation::installed: return "installed";
    case ProviderInstallationObservation::absent: return "absent";
    case ProviderInstallationObservation::unknown: return "unknown";
    }
    throw std::invalid_argument("invalid installation observation");
}

std::string_view activity_text(const ProviderActivityObservation value) {
    switch (value) {
    case ProviderActivityObservation::active: return "active";
    case ProviderActivityObservation::inactive: return "inactive";
    case ProviderActivityObservation::unknown: return "unknown";
    }
    throw std::invalid_argument("invalid activity observation");
}

std::string_view configuration_text(const ProviderConfigurationObservation value) {
    switch (value) {
    case ProviderConfigurationObservation::matches: return "matches";
    case ProviderConfigurationObservation::mismatch: return "mismatch";
    case ProviderConfigurationObservation::not_applicable: return "not_applicable";
    case ProviderConfigurationObservation::unknown: return "unknown";
    }
    throw std::invalid_argument("invalid configuration observation");
}

std::string_view replacement_text(const ProviderReplacementObservation value) {
    switch (value) {
    case ProviderReplacementObservation::safe: return "safe";
    case ProviderReplacementObservation::unsafe: return "unsafe";
    case ProviderReplacementObservation::not_applicable: return "not_applicable";
    case ProviderReplacementObservation::unknown: return "unknown";
    }
    throw std::invalid_argument("invalid replacement observation");
}

ProviderLifecycleObserved expected_observed(const ProviderLifecycleOperation operation,
                                            const ProviderLifecycleDisposition disposition) {
    if (disposition == ProviderLifecycleDisposition::failed) return kUnknown;
    switch (operation) {
    case ProviderLifecycleOperation::install:
        return {ProviderInstallationObservation::installed,
                disposition == ProviderLifecycleDisposition::ready
                    ? ProviderActivityObservation::active
                    : ProviderActivityObservation::inactive,
                ProviderConfigurationObservation::matches,
                ProviderReplacementObservation::not_applicable};
    case ProviderLifecycleOperation::uninstall:
        if (disposition != ProviderLifecycleDisposition::ready)
            throw std::invalid_argument("safe inactive is forbidden for uninstall");
        return {ProviderInstallationObservation::absent,
                ProviderActivityObservation::inactive,
                ProviderConfigurationObservation::not_applicable,
                ProviderReplacementObservation::not_applicable};
    case ProviderLifecycleOperation::pre_replace:
        if (disposition != ProviderLifecycleDisposition::ready)
            throw std::invalid_argument("safe inactive is forbidden before replacement");
        return {ProviderInstallationObservation::installed,
                ProviderActivityObservation::inactive,
                ProviderConfigurationObservation::matches,
                ProviderReplacementObservation::safe};
    case ProviderLifecycleOperation::post_replace:
        return {ProviderInstallationObservation::installed,
                disposition == ProviderLifecycleDisposition::ready
                    ? ProviderActivityObservation::active
                    : ProviderActivityObservation::inactive,
                ProviderConfigurationObservation::matches,
                ProviderReplacementObservation::safe};
    }
    throw std::invalid_argument("invalid lifecycle operation");
}

void validate_result(const ProviderLifecycleResult& result,
                     const bool enforce_observations = true) {
    if (!valid_identifier(result.component_id, kComponentIdBytes) ||
        !valid_identifier(result.component_version, kComponentVersionBytes))
        throw std::invalid_argument("invalid lifecycle result identity");
    (void)operation_text(result.operation);
    (void)disposition_text(result.disposition);
    (void)installation_text(result.observed.installation);
    (void)activity_text(result.observed.activity);
    (void)configuration_text(result.observed.configuration);
    (void)replacement_text(result.observed.replacement);
    if ((result.disposition == ProviderLifecycleDisposition::safe_inactive ||
         result.disposition == ProviderLifecycleDisposition::failed) && !result.reason)
        throw std::invalid_argument("lifecycle result requires a reason");
    if (result.reason && !valid_reason(*result.reason))
        throw std::invalid_argument("lifecycle result reason is invalid");
    if (enforce_observations && result.disposition != ProviderLifecycleDisposition::failed &&
        result.observed != expected_observed(result.operation, result.disposition))
        throw std::invalid_argument("lifecycle result observations do not match operation");
}

ProviderLifecycleResult decode(const nlohmann::json& raw) {
    static constexpr std::array root_keys{"schema", "schema_version", "component_id",
        "component_version", "operation", "disposition", "observed"};
    if (!raw.is_object() || (raw.size() != 7 && raw.size() != 8))
        throw std::invalid_argument("malformed lifecycle result");
    for (const auto* key : root_keys) if (!raw.contains(key))
        throw std::invalid_argument("missing lifecycle result field");
    if (raw.size() == 8 && !raw.contains("reason"))
        throw std::invalid_argument("unknown lifecycle result field");
    for (auto it = raw.begin(); it != raw.end(); ++it)
        if (std::ranges::none_of(root_keys, [&](const char* key) { return it.key() == key; }) &&
            it.key() != "reason") throw std::invalid_argument("unknown lifecycle result field");
    if (!raw.at("schema").is_string() ||
        raw.at("schema") != "arena_provider_lifecycle_result_v1" ||
        !raw.at("schema_version").is_number_integer() || raw.at("schema_version") != 1 ||
        !raw.at("component_id").is_string() ||
        !raw.at("component_version").is_string())
        throw std::invalid_argument("lifecycle result schema is invalid");

    const auto operation = parse_enum(raw.at("operation"), std::array{
        std::pair{"install"sv, ProviderLifecycleOperation::install},
        std::pair{"uninstall"sv, ProviderLifecycleOperation::uninstall},
        std::pair{"pre_replace"sv, ProviderLifecycleOperation::pre_replace},
        std::pair{"post_replace"sv, ProviderLifecycleOperation::post_replace}});
    const auto disposition = parse_enum(raw.at("disposition"), std::array{
        std::pair{"ready"sv, ProviderLifecycleDisposition::ready},
        std::pair{"safe_inactive"sv, ProviderLifecycleDisposition::safe_inactive},
        std::pair{"failed"sv, ProviderLifecycleDisposition::failed}});
    const auto& observed = raw.at("observed");
    if (!observed.is_object() || observed.size() != 4 ||
        !observed.contains("installation") || !observed.contains("activity") ||
        !observed.contains("configuration") || !observed.contains("replacement"))
        throw std::invalid_argument("lifecycle observations are malformed");
    ProviderLifecycleObserved parsed{
        parse_enum(observed.at("installation"), std::array{
            std::pair{"installed"sv, ProviderInstallationObservation::installed},
            std::pair{"absent"sv, ProviderInstallationObservation::absent},
            std::pair{"unknown"sv, ProviderInstallationObservation::unknown}}),
        parse_enum(observed.at("activity"), std::array{
            std::pair{"active"sv, ProviderActivityObservation::active},
            std::pair{"inactive"sv, ProviderActivityObservation::inactive},
            std::pair{"unknown"sv, ProviderActivityObservation::unknown}}),
        parse_enum(observed.at("configuration"), std::array{
            std::pair{"matches"sv, ProviderConfigurationObservation::matches},
            std::pair{"mismatch"sv, ProviderConfigurationObservation::mismatch},
            std::pair{"not_applicable"sv, ProviderConfigurationObservation::not_applicable},
            std::pair{"unknown"sv, ProviderConfigurationObservation::unknown}}),
        parse_enum(observed.at("replacement"), std::array{
            std::pair{"safe"sv, ProviderReplacementObservation::safe},
            std::pair{"unsafe"sv, ProviderReplacementObservation::unsafe},
            std::pair{"not_applicable"sv, ProviderReplacementObservation::not_applicable},
            std::pair{"unknown"sv, ProviderReplacementObservation::unknown}})};
    std::optional<ProviderLifecycleReason> reason;
    if (raw.contains("reason")) {
        const auto& value = raw.at("reason");
        if (!value.is_object() || (value.size() != 1 && value.size() != 2) ||
            !value.contains("id") || !value.at("id").is_string() ||
            (value.size() == 2 && (!value.contains("message") || !value.at("message").is_string())))
            throw std::invalid_argument("lifecycle reason is malformed");
        reason = ProviderLifecycleReason{value.at("id").get<std::string>(),
            value.value("message", std::string{})};
    }
    ProviderLifecycleResult result{raw.at("component_id").get<std::string>(),
        raw.at("component_version").get<std::string>(), operation, disposition,
        parsed, std::move(reason)};
    validate_result(result, false);
    return result;
}

} // namespace

ProviderLifecycleResult ProviderLifecycle::invoke(const ProviderLifecycleRequest& request) {
    try {
        if (!valid_request(request)) return failed(request, "invalid_request");
    } catch (const std::bad_alloc&) { throw; }
    if (request.policy.wire_protocol != ProviderLifecycleWireProtocol::typed_v3)
        return failed(request, "legacy_contract_not_satisfied");
    const auto deadline = std::chrono::steady_clock::now() + request.timeout;
    nlohmann::json raw;
    try { raw = invoke_raw_validated(request, deadline); }
    catch (const std::bad_alloc&) { throw; }
    catch (const std::exception&) { return failed(request, "adapter_exception"); }
    try {
        if (raw.dump().size() > kRawResultBytes) return failed(request, "malformed_result");
        auto result = decode(raw);
        if (result.component_id != request.component_id ||
            result.component_version != request.component_version)
            return failed(request, "identity_mismatch");
        if (result.operation != request.operation)
            return failed(request, "operation_mismatch");
        if (result.disposition == ProviderLifecycleDisposition::safe_inactive) {
            if (!request.policy.allow_safe_inactive ||
                (request.operation != ProviderLifecycleOperation::install &&
                 request.operation != ProviderLifecycleOperation::post_replace))
                return failed(request, "disposition_not_allowed");
        }
        if (result.disposition != ProviderLifecycleDisposition::failed &&
            result.observed != expected_observed(result.operation, result.disposition))
            return failed(request, "observation_mismatch");
        return result;
    } catch (const std::bad_alloc&) { throw; }
    catch (const std::exception&) { return failed(request, "malformed_result"); }
}

nlohmann::json provider_lifecycle_result_to_json(const ProviderLifecycleResult& result) {
    validate_result(result);
    nlohmann::json value = {
        {"schema", "arena_provider_lifecycle_result_v1"}, {"schema_version", 1},
        {"component_id", result.component_id}, {"component_version", result.component_version},
        {"operation", operation_text(result.operation)},
        {"disposition", disposition_text(result.disposition)},
        {"observed", {{"installation", installation_text(result.observed.installation)},
                      {"activity", activity_text(result.observed.activity)},
                      {"configuration", configuration_text(result.observed.configuration)},
                      {"replacement", replacement_text(result.observed.replacement)}}}};
    if (result.reason)
        value["reason"] = {{"id", result.reason->id}, {"message", result.reason->message}};
    return value;
}

} // namespace arena::ports
