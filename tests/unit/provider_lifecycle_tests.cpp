#include "arena/application/installer/provider_lifecycle_contract.hpp"
#include "arena/ports/provider_lifecycle.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
using namespace arena::ports;

ProviderLifecycleObserved ready_observed(ProviderLifecycleOperation operation) {
    switch (operation) {
    case ProviderLifecycleOperation::install:
        return {ProviderInstallationObservation::installed,
                ProviderActivityObservation::active,
                ProviderConfigurationObservation::matches,
                ProviderReplacementObservation::not_applicable};
    case ProviderLifecycleOperation::uninstall:
        return {ProviderInstallationObservation::absent,
                ProviderActivityObservation::inactive,
                ProviderConfigurationObservation::not_applicable,
                ProviderReplacementObservation::not_applicable};
    case ProviderLifecycleOperation::pre_replace:
        return {ProviderInstallationObservation::installed,
                ProviderActivityObservation::inactive,
                ProviderConfigurationObservation::matches,
                ProviderReplacementObservation::safe};
    case ProviderLifecycleOperation::post_replace:
        return {ProviderInstallationObservation::installed,
                ProviderActivityObservation::active,
                ProviderConfigurationObservation::matches,
                ProviderReplacementObservation::safe};
    }
    throw std::logic_error("operation");
}

ProviderLifecycleObserved safe_inactive_observed(ProviderLifecycleOperation operation) {
    auto observed = ready_observed(operation);
    observed.activity = ProviderActivityObservation::inactive;
    return observed;
}

ProviderLifecycleResult result(
    ProviderLifecycleOperation operation = ProviderLifecycleOperation::install,
    ProviderLifecycleDisposition disposition = ProviderLifecycleDisposition::ready) {
    ProviderLifecycleObserved observed = disposition == ProviderLifecycleDisposition::safe_inactive
        ? safe_inactive_observed(operation)
        : ready_observed(operation);
    if (disposition == ProviderLifecycleDisposition::failed) {
        observed = {ProviderInstallationObservation::unknown,
                    ProviderActivityObservation::unknown,
                    ProviderConfigurationObservation::unknown,
                    ProviderReplacementObservation::unknown};
    }
    return {"provider.core", "1.2.3", operation, disposition, observed,
            disposition == ProviderLifecycleDisposition::ready
                ? std::nullopt
                : std::optional{ProviderLifecycleReason{"provider.reason", "opaque"}}};
}

ProviderLifecycleRequest request(
    ProviderLifecycleOperation operation = ProviderLifecycleOperation::install) {
    return {"provider.core", "1.2.3", operation, "C:/provider.exe", "C:/work",
            {"--lifecycle"},
            {ProviderLifecycleWireProtocol::typed_v3,
             ProviderLegacyResultEnvelope::root, true, true},
            std::chrono::seconds{30}};
}

class FakeLifecycle final : public ProviderLifecycle {
  public:
    nlohmann::json raw = provider_lifecycle_result_to_json(result());
    int calls{};
    bool throw_runtime{};
    bool throw_allocation{};
    std::chrono::steady_clock::time_point deadline{};

  private:
    nlohmann::json invoke_raw_validated(
        const ProviderLifecycleRequest&,
        std::chrono::steady_clock::time_point supplied_deadline) override {
        ++calls;
        deadline = supplied_deadline;
        if (throw_allocation) throw std::bad_alloc{};
        if (throw_runtime) throw std::runtime_error("adapter failed");
        return raw;
    }
};

void require_failure(const ProviderLifecycleResult& actual, std::string_view id) {
    REQUIRE(actual.disposition == ProviderLifecycleDisposition::failed);
    REQUIRE(actual.reason.has_value());
    CHECK(actual.reason->id == id);
}

void require_invalid(ProviderLifecycleRequest candidate) {
    FakeLifecycle lifecycle;
    require_failure(lifecycle.invoke(candidate), "invalid_request");
    CHECK(lifecycle.calls == 0);
}

nlohmann::json ready_json(ProviderLifecycleOperation operation) {
    return provider_lifecycle_result_to_json(result(operation));
}

} // namespace

TEST_CASE("ProviderLifecycle fixed constants match the contract") {
    CHECK(provider_lifecycle_component_id_max_utf8_bytes == 128);
    CHECK(provider_lifecycle_component_version_max_utf8_bytes == 64);
    CHECK(provider_lifecycle_arguments_max_count == 32);
    CHECK(provider_lifecycle_argument_max_utf8_bytes == 1024);
    CHECK(provider_lifecycle_raw_result_max_bytes == 65536);
    CHECK(provider_lifecycle_reason_id_max_bytes == 64);
    CHECK(provider_lifecycle_reason_message_max_utf8_bytes == 512);
    CHECK(provider_lifecycle_timeout_minimum == std::chrono::milliseconds{1});
    CHECK(provider_lifecycle_timeout_maximum == std::chrono::seconds{120});
    CHECK(provider_lifecycle_timeout_default == std::chrono::seconds{120});
}

TEST_CASE("ProviderLifecycle invokes a valid typed adapter once with an absolute deadline") {
    FakeLifecycle lifecycle;
    const auto candidate = request();
    const auto before = std::chrono::steady_clock::now();
    const auto actual = lifecycle.invoke(candidate);
    const auto after = std::chrono::steady_clock::now();
    CHECK(actual.disposition == ProviderLifecycleDisposition::ready);
    CHECK(lifecycle.calls == 1);
    CHECK(lifecycle.deadline >= before + candidate.timeout);
    CHECK(lifecycle.deadline <= after + candidate.timeout);
}

TEST_CASE("ProviderLifecycle rejects invalid requests before adapter invocation") {
    auto candidate = request();
    SECTION("id pattern") { candidate.component_id = "bad id"; }
    SECTION("id bound") {
        candidate.component_id = std::string(provider_lifecycle_component_id_max_utf8_bytes + 1, 'a');
    }
    SECTION("version") { candidate.component_version.clear(); }
    SECTION("version pattern") { candidate.component_version = "1/2"; }
    SECTION("version bound") {
        candidate.component_version =
            std::string(provider_lifecycle_component_version_max_utf8_bytes + 1, '1');
    }
    SECTION("executable") { candidate.executable.clear(); }
    SECTION("working directory") { candidate.working_directory.clear(); }
    SECTION("argument count") {
        candidate.arguments.assign(provider_lifecycle_arguments_max_count + 1, "x");
    }
    SECTION("argument bytes") {
        candidate.arguments = {std::string(provider_lifecycle_argument_max_utf8_bytes + 1, 'x')};
    }
    SECTION("argument NUL") { candidate.arguments = {std::string{"a\0b", 3}}; }
    SECTION("argument UTF-8") { candidate.arguments = {std::string{"\xC0\xAF", 2}}; }
    SECTION("timeout low") { candidate.timeout = std::chrono::milliseconds{0}; }
    SECTION("timeout high") { candidate.timeout = std::chrono::milliseconds{120001}; }
    SECTION("typed legacy envelope") {
        candidate.policy.legacy_envelope = ProviderLegacyResultEnvelope::startup;
    }
    SECTION("typed legacy requirement") {
        candidate.policy.require_configuration_match = false;
    }
    require_invalid(candidate);
}

TEST_CASE("ProviderLifecycle maps ordinary adapter exceptions and propagates allocation") {
    FakeLifecycle lifecycle;
    lifecycle.throw_runtime = true;
    require_failure(lifecycle.invoke(request()), "adapter_exception");
    CHECK(lifecycle.calls == 1);
    lifecycle.throw_runtime = false;
    lifecycle.throw_allocation = true;
    CHECK_THROWS_AS(lifecycle.invoke(request()), std::bad_alloc);
}

TEST_CASE("ProviderLifecycle accepts exact ready results for all operations") {
    for (const auto operation : {ProviderLifecycleOperation::install,
                                 ProviderLifecycleOperation::uninstall,
                                 ProviderLifecycleOperation::pre_replace,
                                 ProviderLifecycleOperation::post_replace}) {
        FakeLifecycle lifecycle;
        lifecycle.raw = ready_json(operation);
        CHECK(lifecycle.invoke(request(operation)) == result(operation));
        CHECK(lifecycle.calls == 1);
    }
}

TEST_CASE("ProviderLifecycle gates safe inactive by operation and policy") {
    for (const auto operation : {ProviderLifecycleOperation::install,
                                 ProviderLifecycleOperation::post_replace}) {
        FakeLifecycle lifecycle;
        const auto expected = result(operation, ProviderLifecycleDisposition::safe_inactive);
        lifecycle.raw = provider_lifecycle_result_to_json(expected);
        CHECK(lifecycle.invoke(request(operation)) == expected);
    }

    SECTION("policy denies") {
        FakeLifecycle lifecycle;
        lifecycle.raw = provider_lifecycle_result_to_json(
            result(ProviderLifecycleOperation::install,
                   ProviderLifecycleDisposition::safe_inactive));
        auto candidate = request();
        candidate.policy.allow_safe_inactive = false;
        require_failure(lifecycle.invoke(candidate), "disposition_not_allowed");
    }

    for (const auto operation : {ProviderLifecycleOperation::uninstall,
                                 ProviderLifecycleOperation::pre_replace}) {
        FakeLifecycle lifecycle;
        lifecycle.raw = provider_lifecycle_result_to_json(
            result(ProviderLifecycleOperation::install,
                   ProviderLifecycleDisposition::safe_inactive));
        lifecycle.raw["operation"] = operation == ProviderLifecycleOperation::uninstall
            ? "uninstall"
            : "pre_replace";
        lifecycle.raw["observed"] = ready_json(operation).at("observed");
        require_failure(lifecycle.invoke(request(operation)), "disposition_not_allowed");
    }
}

TEST_CASE("ProviderLifecycle requires bounded reasons and preserves opaque provider reasons") {
    FakeLifecycle lifecycle;
    auto expected = result(ProviderLifecycleOperation::install,
                           ProviderLifecycleDisposition::safe_inactive);
    lifecycle.raw = provider_lifecycle_result_to_json(expected);
    const auto actual = lifecycle.invoke(request());
    REQUIRE(actual.reason);
    CHECK(actual.reason->id == "provider.reason");
    CHECK(actual.reason->message == "opaque");

    SECTION("safe inactive missing reason") {
        lifecycle.raw.erase("reason");
        require_failure(lifecycle.invoke(request()), "malformed_result");
    }
    SECTION("failed missing reason") {
        lifecycle.raw = provider_lifecycle_result_to_json(
            result(ProviderLifecycleOperation::install,
                   ProviderLifecycleDisposition::failed));
        lifecycle.raw.erase("reason");
        require_failure(lifecycle.invoke(request()), "malformed_result");
    }
    SECTION("reason id oversized") {
        lifecycle.raw["reason"]["id"] =
            std::string(provider_lifecycle_reason_id_max_bytes + 1, 'a');
        require_failure(lifecycle.invoke(request()), "malformed_result");
    }
    SECTION("reason message oversized") {
        lifecycle.raw["reason"]["message"] =
            std::string(provider_lifecycle_reason_message_max_utf8_bytes + 1, 'a');
        require_failure(lifecycle.invoke(request()), "malformed_result");
    }
}

TEST_CASE("ProviderLifecycle fails closed on schema identity version and operation mismatch") {
    SECTION("schema") {
        FakeLifecycle lifecycle;
        lifecycle.raw["schema"] = "wrong";
        require_failure(lifecycle.invoke(request()), "malformed_result");
    }
    SECTION("schema version") {
        FakeLifecycle lifecycle;
        lifecycle.raw["schema_version"] = 2;
        require_failure(lifecycle.invoke(request()), "malformed_result");
    }
    SECTION("component id") {
        FakeLifecycle lifecycle;
        lifecycle.raw["component_id"] = "other.core";
        require_failure(lifecycle.invoke(request()), "identity_mismatch");
    }
    SECTION("component version") {
        FakeLifecycle lifecycle;
        lifecycle.raw["component_version"] = "9.9.9";
        require_failure(lifecycle.invoke(request()), "identity_mismatch");
    }
    SECTION("operation") {
        FakeLifecycle lifecycle;
        lifecycle.raw = ready_json(ProviderLifecycleOperation::uninstall);
        require_failure(lifecycle.invoke(request()), "operation_mismatch");
    }
}

TEST_CASE("ProviderLifecycle rejects malformed typed-v3 document shapes") {
    FakeLifecycle lifecycle;
    SECTION("extra field") { lifecycle.raw["provider"] = "forbidden"; }
    SECTION("missing field") { lifecycle.raw.erase("observed"); }
    SECTION("wrong type") { lifecycle.raw["component_id"] = 7; }
    SECTION("unknown disposition") { lifecycle.raw["disposition"] = "deferred"; }
    SECTION("observed extra field") { lifecycle.raw["observed"]["extra"] = true; }
    SECTION("reason wrong type") {
        lifecycle.raw = provider_lifecycle_result_to_json(
            result(ProviderLifecycleOperation::install,
                   ProviderLifecycleDisposition::failed));
        lifecycle.raw["reason"] = "bad";
    }
    SECTION("reason malformed id") {
        lifecycle.raw = provider_lifecycle_result_to_json(
            result(ProviderLifecycleOperation::install,
                   ProviderLifecycleDisposition::failed));
        lifecycle.raw["reason"]["id"] = "bad/id";
    }
    require_failure(lifecycle.invoke(request()), "malformed_result");
}

TEST_CASE("ProviderLifecycle classifies every observation mismatch") {
    for (const auto operation : {ProviderLifecycleOperation::install,
                                 ProviderLifecycleOperation::uninstall,
                                 ProviderLifecycleOperation::pre_replace,
                                 ProviderLifecycleOperation::post_replace}) {
        const auto expected = ready_json(operation).at("observed");
        const std::array<std::pair<std::string, std::string>, 4> mismatches{{
            {"installation", expected.at("installation") == "installed" ? "absent" : "installed"},
            {"activity", expected.at("activity") == "active" ? "inactive" : "active"},
            {"configuration", expected.at("configuration") == "matches" ? "mismatch" : "matches"},
            {"replacement", expected.at("replacement") == "safe" ? "unsafe" : "safe"},
        }};
        for (const auto& [field, value] : mismatches) {
            FakeLifecycle lifecycle;
            lifecycle.raw = ready_json(operation);
            lifecycle.raw["observed"][field] = value;
            require_failure(lifecycle.invoke(request(operation)), "observation_mismatch");
        }
    }
}

TEST_CASE("ProviderLifecycle bounds raw JSON and does not interpret legacy protocols") {
    SECTION("raw size") {
        FakeLifecycle lifecycle;
        lifecycle.raw["padding"] =
            std::string(provider_lifecycle_raw_result_max_bytes + 1, 'x');
        require_failure(lifecycle.invoke(request()), "malformed_result");
    }
    for (const auto protocol : {ProviderLifecycleWireProtocol::legacy_v1,
                                ProviderLifecycleWireProtocol::legacy_v2}) {
        FakeLifecycle lifecycle;
        auto candidate = request();
        candidate.policy.wire_protocol = protocol;
        require_failure(lifecycle.invoke(candidate), "legacy_contract_not_satisfied");
        CHECK(lifecycle.calls == 0);
    }
}

TEST_CASE("provider lifecycle serializer emits exact ready JSON for all operations") {
    struct Expected {
        ProviderLifecycleOperation operation;
        const char* operation_text;
        const char* installation;
        const char* activity;
        const char* configuration;
        const char* replacement;
    };
    const std::array<Expected, 4> expected{{
        Expected{ProviderLifecycleOperation::install, "install", "installed", "active",
                 "matches", "not_applicable"},
        Expected{ProviderLifecycleOperation::uninstall, "uninstall", "absent", "inactive",
                 "not_applicable", "not_applicable"},
        Expected{ProviderLifecycleOperation::pre_replace, "pre_replace", "installed", "inactive",
                 "matches", "safe"},
        Expected{ProviderLifecycleOperation::post_replace, "post_replace", "installed", "active",
                 "matches", "safe"},
    }};
    for (const auto& item : expected) {
        const auto actual = provider_lifecycle_result_to_json(result(item.operation));
        const nlohmann::json exact = {
            {"schema", "arena_provider_lifecycle_result_v1"},
            {"schema_version", 1}, {"component_id", "provider.core"},
            {"component_version", "1.2.3"}, {"operation", item.operation_text},
            {"disposition", "ready"},
            {"observed", {{"installation", item.installation}, {"activity", item.activity},
                          {"configuration", item.configuration}, {"replacement", item.replacement}}}};
        CHECK(actual == exact);
        CHECK_FALSE(actual.contains("reason"));
    }
}

TEST_CASE("provider lifecycle serializer emits exact safe-inactive and failed JSON") {
    for (const auto operation : {ProviderLifecycleOperation::install,
                                 ProviderLifecycleOperation::post_replace}) {
        const auto actual = provider_lifecycle_result_to_json(
            result(operation, ProviderLifecycleDisposition::safe_inactive));
        const bool install = operation == ProviderLifecycleOperation::install;
        const nlohmann::json exact = {
            {"schema", "arena_provider_lifecycle_result_v1"},
            {"schema_version", 1}, {"component_id", "provider.core"},
            {"component_version", "1.2.3"},
            {"operation", install ? "install" : "post_replace"},
            {"disposition", "safe_inactive"},
            {"observed", {{"installation", "installed"}, {"activity", "inactive"},
                          {"configuration", "matches"},
                          {"replacement", install ? "not_applicable" : "safe"}}},
            {"reason", {{"id", "provider.reason"}, {"message", "opaque"}}}};
        CHECK(actual == exact);
    }
    const auto failed = provider_lifecycle_result_to_json(
        result(ProviderLifecycleOperation::install, ProviderLifecycleDisposition::failed));
    const nlohmann::json exact_failed = {
        {"schema", "arena_provider_lifecycle_result_v1"},
        {"schema_version", 1}, {"component_id", "provider.core"},
        {"component_version", "1.2.3"}, {"operation", "install"},
        {"disposition", "failed"},
        {"observed", {{"installation", "unknown"}, {"activity", "unknown"},
                      {"configuration", "unknown"}, {"replacement", "unknown"}}},
        {"reason", {{"id", "provider.reason"}, {"message", "opaque"}}}};
    CHECK(failed == exact_failed);
}

TEST_CASE("provider lifecycle serializer rejects invalid constructed results") {
    auto invalid = result();
    SECTION("operation") { invalid.operation = static_cast<ProviderLifecycleOperation>(999); }
    SECTION("disposition") {
        invalid.disposition = static_cast<ProviderLifecycleDisposition>(999);
    }
    SECTION("observation enum") {
        invalid.observed.activity = static_cast<ProviderActivityObservation>(999);
    }
    SECTION("observation tuple") {
        invalid.observed.activity = ProviderActivityObservation::inactive;
    }
    SECTION("safe inactive operation") {
        invalid = result(ProviderLifecycleOperation::uninstall);
        invalid.disposition = ProviderLifecycleDisposition::safe_inactive;
        invalid.reason = ProviderLifecycleReason{"provider.reason", "opaque"};
    }
    SECTION("missing reason") {
        invalid = result(ProviderLifecycleOperation::install,
                         ProviderLifecycleDisposition::failed);
        invalid.reason.reset();
    }
    SECTION("reason") {
        invalid.reason = ProviderLifecycleReason{"bad/id", "opaque"};
    }
    CHECK_THROWS_AS(provider_lifecycle_result_to_json(invalid), std::invalid_argument);
}

TEST_CASE("provider lifecycle serializer is deterministic pure neutral projection") {
    FakeLifecycle lifecycle;
    const auto value = result(ProviderLifecycleOperation::install,
                              ProviderLifecycleDisposition::safe_inactive);
    const auto first = provider_lifecycle_result_to_json(value);
    const auto second = provider_lifecycle_result_to_json(value);
    CHECK(first == second);
    CHECK(lifecycle.calls == 0);
    CHECK_FALSE(first.contains("legacy_envelope"));
    CHECK_FALSE(first.contains("provider"));
    CHECK_FALSE(first.contains("receipt"));
    CHECK_FALSE(first.contains("transaction"));
    CHECK_FALSE(first.contains("timestamp"));
    CHECK_FALSE(first.contains("path"));
}

namespace {

using arena::application::installer::ProviderLifecycleControl;
using arena::application::installer::parse_provider_lifecycle_control;

nlohmann::json startup_control_v1() {
    return {
        {"schema", "arena_provider_startup_control_v1"},
        {"schema_version", 1},
        {"entrypoint", "cli"},
        {"actions",
         {{"install", nlohmann::json::array({"lifecycle", "enable"})},
          {"uninstall", nlohmann::json::array({"lifecycle", "disable"})}}},
        {"workspace_argument", {{"flag", "--workspace"}, {"relative_path", "."}}},
        {"configuration_argument", "--configuration"},
        {"result",
         {{"envelope", "startup"},
          {"require_configuration_match_on_install", false},
          {"require_live_on_install", true}}},
    };
}

nlohmann::json startup_control_v2(const bool deferred = false) {
    auto control = startup_control_v1();
    control["schema"] = "arena_provider_startup_control_v2";
    control["schema_version"] = 2;
    control["actions"]["pre_replace"] =
        nlohmann::json::array({"lifecycle", "prepare_replace"});
    control["actions"]["post_replace"] =
        nlohmann::json::array({"lifecycle", "complete_replace"});
    control["replacement_arguments"] = {
        {"previous_root", "--previous-root"},
        {"replacement_root", "--replacement-root"},
    };
    control["result"]["require_live_on_post_replace"] = !deferred;
    if (deferred) {
        control["result"]["allow_deferred_requalification_on_post_replace"] = true;
    }
    return control;
}

nlohmann::json startup_control_v3(const bool replacement = false) {
    nlohmann::json control = {
        {"schema", "arena_provider_startup_control_v3"},
        {"schema_version", 3},
        {"entrypoint", "cli"},
        {"operations",
         {{"install",
           {{"arguments", nlohmann::json::array({"lifecycle", "enable"})},
            {"success_dispositions",
             nlohmann::json::array({"ready", "safe_inactive"})}}},
          {"uninstall",
           {{"arguments", nlohmann::json::array({"lifecycle", "disable"})},
            {"success_dispositions", nlohmann::json::array({"ready"})}}}}},
        {"workspace_argument",
         {{"flag", "--workspace"}, {"relative_path", "state/control"}}},
        {"configuration_argument", "--configuration"},
    };
    if (replacement) {
        control["operations"]["pre_replace"] = {
            {"arguments", nlohmann::json::array({"lifecycle", "prepare_replace"})},
            {"success_dispositions", nlohmann::json::array({"ready"})},
        };
        control["operations"]["post_replace"] = {
            {"arguments", nlohmann::json::array({"lifecycle", "complete_replace"})},
            {"success_dispositions",
             nlohmann::json::array({"ready", "safe_inactive"})},
        };
        control["replacement_arguments"] = {
            {"previous_root", "--previous-root"},
            {"replacement_root", "--replacement-root"},
        };
    }
    return control;
}

void require_contract_invalid(const nlohmann::json& control) {
    CHECK_THROWS_AS(parse_provider_lifecycle_control(control), std::invalid_argument);
}

} // namespace

TEST_CASE("ProviderLifecycle contract parser normalizes exact legacy v1") {
    auto control = startup_control_v1();
    const ProviderLifecycleControl parsed = parse_provider_lifecycle_control(control);
    CHECK(parsed.wire_protocol == ProviderLifecycleWireProtocol::legacy_v1);
    CHECK(parsed.entrypoint == "cli");
    REQUIRE(parsed.operations.size() == 2);
    CHECK(parsed.operations.at(ProviderLifecycleOperation::install).arguments ==
          std::vector<std::string>{"lifecycle", "enable"});
    CHECK_FALSE(parsed.operations.at(ProviderLifecycleOperation::install).allow_safe_inactive);
    CHECK_FALSE(parsed.operations.at(ProviderLifecycleOperation::uninstall).allow_safe_inactive);
    CHECK(parsed.workspace_flag == "--workspace");
    CHECK(parsed.workspace_relative_path == std::filesystem::path{"."});
    CHECK(parsed.configuration_flag == "--configuration");
    CHECK_FALSE(parsed.previous_root_flag);
    CHECK_FALSE(parsed.replacement_root_flag);
    CHECK(parsed.legacy_envelope == ProviderLegacyResultEnvelope::startup);
    CHECK_FALSE(parsed.legacy_require_configuration_match);

    control["result"]["require_live_on_install"] = false;
    CHECK(parse_provider_lifecycle_control(control)
              .operations.at(ProviderLifecycleOperation::install)
              .allow_safe_inactive);
}

TEST_CASE("ProviderLifecycle contract parser contains legacy v2 policy while normalizing operations") {
    SECTION("live post replace") {
        const auto parsed = parse_provider_lifecycle_control(startup_control_v2());
        CHECK(parsed.wire_protocol == ProviderLifecycleWireProtocol::legacy_v2);
        REQUIRE(parsed.operations.size() == 4);
        CHECK_FALSE(parsed.operations.at(ProviderLifecycleOperation::pre_replace)
                        .allow_safe_inactive);
        CHECK_FALSE(parsed.operations.at(ProviderLifecycleOperation::post_replace)
                        .allow_safe_inactive);
        REQUIRE(parsed.previous_root_flag);
        REQUIRE(parsed.replacement_root_flag);
        CHECK(*parsed.previous_root_flag == "--previous-root");
        CHECK(*parsed.replacement_root_flag == "--replacement-root");
    }
    SECTION("deferred legacy result becomes neutral safe inactive permission") {
        const auto parsed = parse_provider_lifecycle_control(startup_control_v2(true));
        CHECK(parsed.operations.at(ProviderLifecycleOperation::post_replace)
                  .allow_safe_inactive);
        CHECK_FALSE(parsed.operations.at(ProviderLifecycleOperation::pre_replace)
                        .allow_safe_inactive);
    }
}

TEST_CASE("ProviderLifecycle contract parser normalizes typed v3 without legacy influence") {
    const auto parsed = parse_provider_lifecycle_control(startup_control_v3());
    CHECK(parsed.wire_protocol == ProviderLifecycleWireProtocol::typed_v3);
    CHECK(parsed.entrypoint == "cli");
    REQUIRE(parsed.operations.size() == 2);
    CHECK(parsed.operations.at(ProviderLifecycleOperation::install).allow_safe_inactive);
    CHECK_FALSE(parsed.operations.at(ProviderLifecycleOperation::uninstall).allow_safe_inactive);
    CHECK(parsed.workspace_flag == "--workspace");
    CHECK(parsed.workspace_relative_path == std::filesystem::path{"state/control"});
    CHECK(parsed.configuration_flag == "--configuration");
    CHECK_FALSE(parsed.previous_root_flag);
    CHECK_FALSE(parsed.replacement_root_flag);
    CHECK(parsed.legacy_envelope == ProviderLegacyResultEnvelope::root);
    CHECK(parsed.legacy_require_configuration_match);
}

TEST_CASE("ProviderLifecycle contract parser normalizes the exact typed v3 replacement pair") {
    const auto parsed = parse_provider_lifecycle_control(startup_control_v3(true));
    REQUIRE(parsed.operations.size() == 4);
    CHECK_FALSE(parsed.operations.at(ProviderLifecycleOperation::pre_replace)
                    .allow_safe_inactive);
    CHECK(parsed.operations.at(ProviderLifecycleOperation::post_replace)
              .allow_safe_inactive);
    REQUIRE(parsed.previous_root_flag);
    REQUIRE(parsed.replacement_root_flag);
    CHECK(*parsed.previous_root_flag == "--previous-root");
    CHECK(*parsed.replacement_root_flag == "--replacement-root");
}

TEST_CASE("ProviderLifecycle contract parser rejects unknown mixed and contradictory legacy contracts") {
    SECTION("unknown schema") {
        auto control = startup_control_v1();
        control["schema"] = "arena_provider_startup_control_v4";
        control["schema_version"] = 4;
        require_contract_invalid(control);
    }
    SECTION("mixed schema version") {
        auto control = startup_control_v1();
        control["schema_version"] = 2;
        require_contract_invalid(control);
    }
    SECTION("v1 extra operation") {
        auto control = startup_control_v1();
        control["actions"]["pre_replace"] = nlohmann::json::array({"lifecycle"});
        require_contract_invalid(control);
    }
    SECTION("v1 extra result field") {
        auto control = startup_control_v1();
        control["result"]["provider_policy"] = true;
        require_contract_invalid(control);
    }
    SECTION("v2 live and deferred contradict") {
        auto control = startup_control_v2();
        control["result"]["allow_deferred_requalification_on_post_replace"] = true;
        require_contract_invalid(control);
    }
    SECTION("v2 inactive without explicit deferred permission") {
        auto control = startup_control_v2();
        control["result"]["require_live_on_post_replace"] = false;
        require_contract_invalid(control);
    }
    SECTION("v2 extra result field") {
        auto control = startup_control_v2();
        control["result"]["provider_selector"] = "forbidden";
        require_contract_invalid(control);
    }
}

TEST_CASE("ProviderLifecycle contract parser rejects malformed typed v3 shapes") {
    SECTION("one sided pre replace") {
        auto control = startup_control_v3();
        control["operations"]["pre_replace"] = {
            {"arguments", nlohmann::json::array({"lifecycle"})},
            {"success_dispositions", nlohmann::json::array({"ready"})},
        };
        require_contract_invalid(control);
    }
    SECTION("replacement arguments without operations") {
        auto control = startup_control_v3();
        control["replacement_arguments"] = {
            {"previous_root", "--previous-root"},
            {"replacement_root", "--replacement-root"},
        };
        require_contract_invalid(control);
    }
    SECTION("replacement operations without arguments") {
        auto control = startup_control_v3(true);
        control.erase("replacement_arguments");
        require_contract_invalid(control);
    }
    SECTION("safe inactive uninstall") {
        auto control = startup_control_v3();
        control["operations"]["uninstall"]["success_dispositions"] =
            nlohmann::json::array({"ready", "safe_inactive"});
        require_contract_invalid(control);
    }
    SECTION("safe inactive pre replace") {
        auto control = startup_control_v3(true);
        control["operations"]["pre_replace"]["success_dispositions"] =
            nlohmann::json::array({"ready", "safe_inactive"});
        require_contract_invalid(control);
    }
    SECTION("wrong disposition order") {
        auto control = startup_control_v3();
        control["operations"]["install"]["success_dispositions"] =
            nlohmann::json::array({"safe_inactive", "ready"});
        require_contract_invalid(control);
    }
    SECTION("legacy result envelope") {
        auto control = startup_control_v3();
        control["result"] = {{"envelope", "root"}};
        require_contract_invalid(control);
    }
    SECTION("legacy live policy") {
        auto control = startup_control_v3();
        control["require_live_on_install"] = false;
        require_contract_invalid(control);
    }
    SECTION("provider selector") {
        auto control = startup_control_v3();
        control["provider_selector"] = "forbidden";
        require_contract_invalid(control);
    }
    SECTION("shell command field") {
        auto control = startup_control_v3();
        control["operations"]["install"]["command"] = "lifecycle enable";
        require_contract_invalid(control);
    }
    SECTION("duplicate argument") {
        auto control = startup_control_v3();
        control["operations"]["install"]["arguments"] =
            nlohmann::json::array({"lifecycle", "lifecycle"});
        require_contract_invalid(control);
    }
    SECTION("empty argument") {
        auto control = startup_control_v3();
        control["operations"]["install"]["arguments"] = nlohmann::json::array({""});
        require_contract_invalid(control);
    }
    SECTION("oversized argument") {
        auto control = startup_control_v3();
        control["operations"]["install"]["arguments"] =
            nlohmann::json::array({std::string(129, 'x')});
        require_contract_invalid(control);
    }
    SECTION("unsafe workspace flag") {
        auto control = startup_control_v3();
        control["workspace_argument"]["flag"] = "--bad_flag";
        require_contract_invalid(control);
    }
    SECTION("absolute workspace path") {
        auto control = startup_control_v3();
        control["workspace_argument"]["relative_path"] = "/absolute";
        require_contract_invalid(control);
    }
    SECTION("drive workspace path") {
        auto control = startup_control_v3();
        control["workspace_argument"]["relative_path"] = "C:/absolute";
        require_contract_invalid(control);
    }
    SECTION("parent workspace path") {
        auto control = startup_control_v3();
        control["workspace_argument"]["relative_path"] = "state/../escape";
        require_contract_invalid(control);
    }
    SECTION("unknown operation") {
        auto control = startup_control_v3();
        control["operations"]["restart"] = control["operations"]["install"];
        require_contract_invalid(control);
    }
}
