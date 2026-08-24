#pragma once

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace arena::ports {

enum class ProviderLifecycleOperation { install, uninstall, pre_replace, post_replace };
enum class ProviderLifecycleDisposition { ready, safe_inactive, failed };
enum class ProviderInstallationObservation { installed, absent, unknown };
enum class ProviderActivityObservation { active, inactive, unknown };
enum class ProviderConfigurationObservation { matches, mismatch, not_applicable, unknown };
enum class ProviderReplacementObservation { safe, unsafe, not_applicable, unknown };
enum class ProviderLifecycleWireProtocol { legacy_v1, legacy_v2, typed_v3 };
enum class ProviderLegacyResultEnvelope { root, startup };

struct ProviderLifecycleReason {
    std::string id;
    std::string message;
    bool operator==(const ProviderLifecycleReason&) const = default;
};

struct ProviderLifecycleObserved {
    ProviderInstallationObservation installation;
    ProviderActivityObservation activity;
    ProviderConfigurationObservation configuration;
    ProviderReplacementObservation replacement;
    bool operator==(const ProviderLifecycleObserved&) const = default;
};

struct ProviderLifecycleResult {
    std::string component_id;
    std::string component_version;
    ProviderLifecycleOperation operation;
    ProviderLifecycleDisposition disposition;
    ProviderLifecycleObserved observed;
    std::optional<ProviderLifecycleReason> reason;
    bool operator==(const ProviderLifecycleResult&) const = default;
};

struct ProviderLifecyclePolicy {
    ProviderLifecycleWireProtocol wire_protocol;
    ProviderLegacyResultEnvelope legacy_envelope;
    bool require_configuration_match;
    bool allow_safe_inactive;
};

struct ProviderLifecycleRequest {
    std::string component_id;
    std::string component_version;
    ProviderLifecycleOperation operation;
    std::filesystem::path executable;
    std::filesystem::path working_directory;
    std::vector<std::string> arguments;
    ProviderLifecyclePolicy policy;
    std::chrono::milliseconds timeout;
};

class ProviderLifecycle {
  public:
    virtual ~ProviderLifecycle() = default;
    [[nodiscard]] ProviderLifecycleResult invoke(
        const ProviderLifecycleRequest& request);

  protected:
    [[nodiscard]] virtual nlohmann::json invoke_raw_validated(
        const ProviderLifecycleRequest& request,
        std::chrono::steady_clock::time_point deadline) = 0;
};

[[nodiscard]] nlohmann::json provider_lifecycle_result_to_json(
    const ProviderLifecycleResult& result);

} // namespace arena::ports
