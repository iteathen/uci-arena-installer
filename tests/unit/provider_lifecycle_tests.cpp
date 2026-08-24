#include "arena/application/installer/provider_lifecycle_contract.hpp"
#include "arena/adapters/provider_lifecycle.hpp"
#include "arena/application/process_supervisor.hpp"
#include "arena/ports/provider_lifecycle.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
using namespace arena::ports;
using arena::application::installer::parse_provider_lifecycle_control;

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

ProviderLifecycleRequest legacy_request(
    const ProviderLifecycleWireProtocol protocol,
    const ProviderLifecycleOperation operation,
    const bool allow_safe_inactive = false,
    const ProviderLegacyResultEnvelope envelope = ProviderLegacyResultEnvelope::root) {
    auto candidate = request(operation);
    candidate.policy = {protocol, envelope, true, allow_safe_inactive};
    return candidate;
}

nlohmann::json legacy_status(const bool installed, const bool active,
                             const bool replacement_ready = false,
                             const bool deferred = false) {
    return {{"installed", installed},
            {"matchesCurrentConfiguration", true},
            {"active", active},
            {"replacementReady", replacement_ready},
            {"requalificationRequired", deferred}};
}

nlohmann::json legacy_v1_control(const bool require_live = false) {
    return {
        {"schema", "arena_provider_startup_control_v1"},
        {"schema_version", 1},
        {"entrypoint", "provider_cli"},
        {"actions",
         {{"install", nlohmann::json::array({"lifecycle", "enable"})},
          {"uninstall", nlohmann::json::array({"lifecycle", "disable"})}}},
        {"workspace_argument", {{"flag", "--state_root"}, {"relative_path", "runtime"}}},
        {"configuration_argument", "--settings_file"},
        {"result",
         {{"envelope", "startup"},
          {"require_configuration_match_on_install", true},
          {"require_live_on_install", require_live}}},
    };
}

nlohmann::json legacy_v2_control(const bool require_live_post_replace = false) {
    auto value = legacy_v1_control(false);
    value["schema"] = "arena_provider_startup_control_v2";
    value["schema_version"] = 2;
    value["actions"]["pre_replace"] =
        nlohmann::json::array({"lifecycle", "prepare_replace"});
    value["actions"]["post_replace"] =
        nlohmann::json::array({"lifecycle", "complete_replace"});
    value["replacement_arguments"] = {
        {"previous_root", "--previous_root"},
        {"replacement_root", "--replacement_root"}};
    value["result"]["require_live_on_post_replace"] = require_live_post_replace;
    if (!require_live_post_replace) {
        value["result"]["allow_deferred_requalification_on_post_replace"] = true;
    }
    return value;
}

nlohmann::json typed_v3_control(const bool replacement = false) {
    nlohmann::json operations{
        {"install",
         {{"arguments", nlohmann::json::array({"lifecycle", "enable"})},
          {"success_dispositions",
           nlohmann::json::array({"ready", "safe_inactive"})}}},
        {"uninstall",
         {{"arguments", nlohmann::json::array({"lifecycle", "disable"})},
          {"success_dispositions", nlohmann::json::array({"ready"})}}},
    };
    nlohmann::json value{
        {"schema", "arena_provider_startup_control_v3"},
        {"schema_version", 3},
        {"entrypoint", "provider_cli"},
        {"operations", std::move(operations)},
        {"workspace_argument", {{"flag", "--state-root"}, {"relative_path", "runtime"}}},
        {"configuration_argument", "--settings-file"},
    };
    if (replacement) {
        value["operations"]["pre_replace"] = {
            {"arguments", nlohmann::json::array({"lifecycle", "prepare-replace"})},
            {"success_dispositions", nlohmann::json::array({"ready"})}};
        value["operations"]["post_replace"] = {
            {"arguments", nlohmann::json::array({"lifecycle", "complete-replace"})},
            {"success_dispositions",
             nlohmann::json::array({"ready", "safe_inactive"})}};
        value["replacement_arguments"] = {
            {"previous_root", "--previous-root"},
            {"replacement_root", "--replacement-root"}};
    }
    return value;
}

class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        root_ = std::filesystem::temp_directory_path() /
                ("arena-provider-lifecycle-" +
                 std::to_string(std::chrono::steady_clock::now()
                                    .time_since_epoch()
                                    .count()));
        if (!std::filesystem::create_directory(root_)) {
            throw std::runtime_error("failed to create provider lifecycle test directory");
        }
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return root_; }

  private:
    std::filesystem::path root_;
};

std::string operation_argument(const ProviderLifecycleOperation operation) {
    switch (operation) {
    case ProviderLifecycleOperation::install: return "install";
    case ProviderLifecycleOperation::uninstall: return "uninstall";
    case ProviderLifecycleOperation::pre_replace: return "pre_replace";
    case ProviderLifecycleOperation::post_replace: return "post_replace";
    }
    throw std::logic_error("invalid lifecycle operation fixture");
}

ProviderLifecycleRequest cli_request(
    const TemporaryDirectory& directory,
    const ProviderLifecycleOperation operation,
    const std::string& variant,
    const std::chrono::milliseconds timeout = std::chrono::seconds{2}) {
    const auto record = directory.path() / "invocations.log";
    return {
        "provider.core",
        "1.2.3",
        operation,
        ARENA_MOCK_SERVICE_PATH,
        directory.path(),
        {"provider-lifecycle",
         "--component-id",
         "provider.core",
         "--component-version",
         "1.2.3",
         "--operation",
         operation_argument(operation),
         "--variant",
         variant,
         "--expected-working-directory",
         directory.path().string(),
         "--literal-argument",
         "opaque value & | ;",
         "--invocation-record",
         record.string()},
        {ProviderLifecycleWireProtocol::typed_v3,
         ProviderLegacyResultEnvelope::root,
         true,
         true},
        timeout};
}

std::size_t invocation_count(const TemporaryDirectory& directory) {
    std::ifstream input(directory.path() / "invocations.log", std::ios::binary);
    std::size_t count = 0;
    std::string line;
    while (std::getline(input, line)) ++count;
    return count;
}

} // namespace

TEST_CASE("CLI ProviderLifecycle factory preserves process inputs and invokes once") {
    TemporaryDirectory directory;
    arena::application::ProcessSupervisor process_supervisor;
    auto lifecycle =
        arena::adapters::make_cli_provider_lifecycle(process_supervisor);
    REQUIRE(lifecycle);

    std::size_t expected_invocations = 0;
    for (const auto operation : {ProviderLifecycleOperation::install,
                                 ProviderLifecycleOperation::uninstall,
                                 ProviderLifecycleOperation::pre_replace,
                                 ProviderLifecycleOperation::post_replace}) {
        const auto actual = lifecycle->invoke(cli_request(directory, operation, "ready"));
        CHECK(actual == result(operation));
        CHECK(invocation_count(directory) == ++expected_invocations);
    }
}

TEST_CASE("CLI ProviderLifecycle transports safe-inactive and failed typed results") {
    TemporaryDirectory directory;
    arena::application::ProcessSupervisor process_supervisor;
    auto lifecycle =
        arena::adapters::make_cli_provider_lifecycle(process_supervisor);

    for (const auto operation : {ProviderLifecycleOperation::install,
                                 ProviderLifecycleOperation::post_replace}) {
        const auto actual =
            lifecycle->invoke(cli_request(directory, operation, "safe_inactive"));
        CHECK(actual.disposition == ProviderLifecycleDisposition::safe_inactive);
        REQUIRE(actual.reason);
        CHECK(actual.reason->id == "fixture.safe_inactive");
        CHECK(actual.observed == safe_inactive_observed(operation));
    }
    const auto failed = lifecycle->invoke(
        cli_request(directory, ProviderLifecycleOperation::install, "failed"));
    CHECK(failed.disposition == ProviderLifecycleDisposition::failed);
    REQUIRE(failed.reason);
    CHECK(failed.reason->id == "fixture.failed");
    CHECK(invocation_count(directory) == 3);
}

TEST_CASE("CLI ProviderLifecycle leaves raw semantic failures to the neutral base") {
    TemporaryDirectory directory;
    arena::application::ProcessSupervisor process_supervisor;
    auto lifecycle =
        arena::adapters::make_cli_provider_lifecycle(process_supervisor);

    struct ExpectedFailure {
        const char* variant;
        const char* reason;
    };
    const std::array failures{
        ExpectedFailure{"identity_mismatch", "identity_mismatch"},
        ExpectedFailure{"operation_mismatch", "operation_mismatch"},
        ExpectedFailure{"observation_mismatch", "observation_mismatch"},
        ExpectedFailure{"malformed", "malformed_result"},
        ExpectedFailure{"invalid_json", "adapter_exception"},
        ExpectedFailure{"nonzero", "adapter_exception"},
    };
    for (const auto& expected : failures) {
        require_failure(
            lifecycle->invoke(cli_request(directory,
                                          ProviderLifecycleOperation::install,
                                          expected.variant)),
            expected.reason);
    }
    CHECK(invocation_count(directory) == failures.size());
}

TEST_CASE("CLI ProviderLifecycle never extends or launches beyond its deadline") {
    SECTION("remaining timeout") {
        TemporaryDirectory directory;
        arena::application::ProcessSupervisor process_supervisor;
        auto lifecycle =
            arena::adapters::make_cli_provider_lifecycle(process_supervisor);
        const auto before = std::chrono::steady_clock::now();
        require_failure(
            lifecycle->invoke(cli_request(directory,
                                          ProviderLifecycleOperation::install,
                                          "timeout",
                                          std::chrono::milliseconds{200})),
            "adapter_exception");
        const auto elapsed = std::chrono::steady_clock::now() - before;
        CHECK(elapsed < std::chrono::seconds{2});
        CHECK(invocation_count(directory) == 1);
    }
    SECTION("no whole millisecond remains") {
        TemporaryDirectory directory;
        arena::application::ProcessSupervisor process_supervisor;
        auto lifecycle =
            arena::adapters::make_cli_provider_lifecycle(process_supervisor);
        require_failure(
            lifecycle->invoke(cli_request(directory,
                                          ProviderLifecycleOperation::install,
                                          "ready",
                                          provider_lifecycle_timeout_minimum)),
            "adapter_exception");
        CHECK(invocation_count(directory) == 0);
    }
}

TEST_CASE("ProviderLifecycle contract parser normalizes exact legacy v1 control") {
    const auto parsed = parse_provider_lifecycle_control(legacy_v1_control(false));
    CHECK(parsed.wire_protocol == ProviderLifecycleWireProtocol::legacy_v1);
    CHECK(parsed.entrypoint == "provider_cli");
    REQUIRE(parsed.operations.size() == 2);
    CHECK(parsed.operations.at(ProviderLifecycleOperation::install).arguments ==
          std::vector<std::string>{"lifecycle", "enable"});
    CHECK(parsed.operations.at(ProviderLifecycleOperation::install).allow_safe_inactive);
    CHECK_FALSE(
        parsed.operations.at(ProviderLifecycleOperation::uninstall).allow_safe_inactive);
    CHECK(parsed.workspace_flag == "--state_root");
    CHECK(parsed.workspace_relative_path == std::filesystem::path{"runtime"});
    CHECK(parsed.configuration_flag == "--settings_file");
    CHECK_FALSE(parsed.previous_root_flag);
    CHECK_FALSE(parsed.replacement_root_flag);
    CHECK(parsed.legacy_envelope == ProviderLegacyResultEnvelope::startup);
    CHECK(parsed.legacy_require_configuration_match);
}

TEST_CASE("ProviderLifecycle contract parser normalizes exact legacy v2 control") {
    const auto parsed = parse_provider_lifecycle_control(legacy_v2_control(false));
    CHECK(parsed.wire_protocol == ProviderLifecycleWireProtocol::legacy_v2);
    REQUIRE(parsed.operations.size() == 4);
    CHECK(parsed.operations.at(ProviderLifecycleOperation::install).allow_safe_inactive);
    CHECK_FALSE(
        parsed.operations.at(ProviderLifecycleOperation::uninstall).allow_safe_inactive);
    CHECK_FALSE(
        parsed.operations.at(ProviderLifecycleOperation::pre_replace).allow_safe_inactive);
    CHECK(parsed.operations.at(ProviderLifecycleOperation::post_replace)
              .allow_safe_inactive);
    REQUIRE(parsed.previous_root_flag);
    REQUIRE(parsed.replacement_root_flag);
    CHECK(*parsed.previous_root_flag == "--previous_root");
    CHECK(*parsed.replacement_root_flag == "--replacement_root");

    const auto live = parse_provider_lifecycle_control(legacy_v2_control(true));
    CHECK_FALSE(live.operations.at(ProviderLifecycleOperation::post_replace)
                    .allow_safe_inactive);
}

TEST_CASE("ProviderLifecycle contract parser normalizes typed v3 operation policy") {
    const auto parsed = parse_provider_lifecycle_control(typed_v3_control(false));
    CHECK(parsed.wire_protocol == ProviderLifecycleWireProtocol::typed_v3);
    REQUIRE(parsed.operations.size() == 2);
    CHECK(parsed.operations.at(ProviderLifecycleOperation::install).allow_safe_inactive);
    CHECK_FALSE(
        parsed.operations.at(ProviderLifecycleOperation::uninstall).allow_safe_inactive);
    CHECK(parsed.workspace_flag == "--state-root");
    CHECK(parsed.configuration_flag == "--settings-file");
    CHECK_FALSE(parsed.previous_root_flag);
    CHECK_FALSE(parsed.replacement_root_flag);
    CHECK(parsed.legacy_envelope == ProviderLegacyResultEnvelope::root);
    CHECK(parsed.legacy_require_configuration_match);
}

TEST_CASE("ProviderLifecycle contract parser normalizes the typed replacement pair") {
    const auto parsed = parse_provider_lifecycle_control(typed_v3_control(true));
    REQUIRE(parsed.operations.size() == 4);
    CHECK_FALSE(
        parsed.operations.at(ProviderLifecycleOperation::pre_replace).allow_safe_inactive);
    CHECK(parsed.operations.at(ProviderLifecycleOperation::post_replace)
              .allow_safe_inactive);
    REQUIRE(parsed.previous_root_flag);
    REQUIRE(parsed.replacement_root_flag);
    CHECK(*parsed.previous_root_flag == "--previous-root");
    CHECK(*parsed.replacement_root_flag == "--replacement-root");
}

TEST_CASE("ProviderLifecycle contract parser rejects malformed typed v3 control") {
    auto invalid = typed_v3_control(true);
    SECTION("safe inactive uninstall") {
        invalid["operations"]["uninstall"]["success_dispositions"] =
            nlohmann::json::array({"ready", "safe_inactive"});
    }
    SECTION("safe inactive pre replace") {
        invalid["operations"]["pre_replace"]["success_dispositions"] =
            nlohmann::json::array({"ready", "safe_inactive"});
    }
    SECTION("post replace without pre replace") {
        invalid["operations"].erase("pre_replace");
    }
    SECTION("replacement operation without arguments") {
        invalid.erase("replacement_arguments");
    }
    SECTION("replacement arguments without operations") {
        invalid = typed_v3_control(false);
        invalid["replacement_arguments"] = {
            {"previous_root", "--previous-root"},
            {"replacement_root", "--replacement-root"}};
    }
    SECTION("legacy result envelope") {
        invalid = typed_v3_control(false);
        invalid["result"] = {{"envelope", "root"}};
    }
    SECTION("legacy live requirement") {
        invalid = typed_v3_control(false);
        invalid["require_live_on_install"] = true;
    }
    SECTION("legacy deferred requirement") {
        invalid = typed_v3_control(false);
        invalid["allow_deferred_requalification_on_post_replace"] = true;
    }
    SECTION("unknown field") { invalid["provider_selector"] = "opaque"; }
    SECTION("unsafe flag") { invalid["configuration_argument"] = "--bad_flag"; }
    SECTION("unsafe absolute path") {
        invalid["workspace_argument"]["relative_path"] = "C:/outside";
    }
    SECTION("empty arguments") {
        invalid["operations"]["install"]["arguments"] = nlohmann::json::array();
    }
    SECTION("oversized argument") {
        invalid["operations"]["install"]["arguments"] =
            nlohmann::json::array({std::string(129, 'x')});
    }
    SECTION("duplicate arguments") {
        invalid["operations"]["install"]["arguments"] =
            nlohmann::json::array({"lifecycle", "lifecycle"});
    }
    SECTION("shell command field") {
        invalid = typed_v3_control(false);
        invalid["command"] = "lifecycle enable && launch";
    }
    CHECK_THROWS_AS(parse_provider_lifecycle_control(invalid), std::invalid_argument);
}

TEST_CASE("ProviderLifecycle contract parser rejects legacy contradictions and version mixing") {
    auto invalid = legacy_v2_control(false);
    SECTION("live and deferred") {
        invalid["result"]["require_live_on_post_replace"] = true;
    }
    SECTION("inactive without deferred permission") {
        invalid["result"].erase("allow_deferred_requalification_on_post_replace");
    }
    SECTION("duplicate legacy argument") {
        invalid["actions"]["install"] =
            nlohmann::json::array({"lifecycle", "lifecycle"});
    }
    SECTION("shell-like concatenated legacy argument") {
        invalid["actions"]["install"] =
            nlohmann::json::array({"lifecycle && launch"});
    }
    SECTION("one sided legacy replacement") { invalid["actions"].erase("pre_replace"); }
    SECTION("unknown major") {
        invalid["schema"] = "arena_provider_startup_control_v4";
        invalid["schema_version"] = 4;
    }
    SECTION("mixed schema version") { invalid["schema_version"] = 3; }
    SECTION("extra field") { invalid["requalification"] = true; }
    CHECK_THROWS_AS(parse_provider_lifecycle_control(invalid), std::invalid_argument);
}

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

TEST_CASE("ProviderLifecycle bounds raw JSON") {
    SECTION("raw size") {
        FakeLifecycle lifecycle;
        lifecycle.raw["padding"] =
            std::string(provider_lifecycle_raw_result_max_bytes + 1, 'x');
        require_failure(lifecycle.invoke(request()), "malformed_result");
    }
}

TEST_CASE("ProviderLifecycle maps legacy v1 lifecycle output to neutral evidence") {
    SECTION("install ready") {
        FakeLifecycle lifecycle;
        lifecycle.raw = legacy_status(true, true);
        CHECK(lifecycle.invoke(legacy_request(
                  ProviderLifecycleWireProtocol::legacy_v1,
                  ProviderLifecycleOperation::install)) == result());
    }
    SECTION("install explicitly safe inactive") {
        FakeLifecycle lifecycle;
        lifecycle.raw = {{"startup", legacy_status(true, false)}};
        const auto actual = lifecycle.invoke(legacy_request(
            ProviderLifecycleWireProtocol::legacy_v1,
            ProviderLifecycleOperation::install, true,
            ProviderLegacyResultEnvelope::startup));
        CHECK(actual.observed == safe_inactive_observed(
                                     ProviderLifecycleOperation::install));
        CHECK(actual.disposition == ProviderLifecycleDisposition::safe_inactive);
        REQUIRE(actual.reason);
        CHECK(actual.reason->id == "legacy.safe_inactive");
        const auto serialized = provider_lifecycle_result_to_json(actual).dump();
        CHECK(serialized.find("matchesCurrentConfiguration") == std::string::npos);
        CHECK(serialized.find("requalificationRequired") == std::string::npos);
    }
    SECTION("uninstall ready") {
        FakeLifecycle lifecycle;
        lifecycle.raw = legacy_status(false, false);
        CHECK(lifecycle.invoke(legacy_request(
                  ProviderLifecycleWireProtocol::legacy_v1,
                  ProviderLifecycleOperation::uninstall)) ==
              result(ProviderLifecycleOperation::uninstall));
    }
}

TEST_CASE("ProviderLifecycle maps legacy v2 replacement output to neutral evidence") {
    SECTION("install and uninstall retain v1 mapping") {
        FakeLifecycle install;
        install.raw = legacy_status(true, true);
        CHECK(install.invoke(legacy_request(
                  ProviderLifecycleWireProtocol::legacy_v2,
                  ProviderLifecycleOperation::install)) == result());
        FakeLifecycle uninstall;
        uninstall.raw = legacy_status(false, false);
        CHECK(uninstall.invoke(legacy_request(
                  ProviderLifecycleWireProtocol::legacy_v2,
                  ProviderLifecycleOperation::uninstall)) ==
              result(ProviderLifecycleOperation::uninstall));
    }
    SECTION("pre replace ready") {
        FakeLifecycle lifecycle;
        lifecycle.raw = legacy_status(true, false, true);
        CHECK(lifecycle.invoke(legacy_request(
                  ProviderLifecycleWireProtocol::legacy_v2,
                  ProviderLifecycleOperation::pre_replace)) ==
              result(ProviderLifecycleOperation::pre_replace));
    }
    SECTION("post replace ready") {
        FakeLifecycle lifecycle;
        lifecycle.raw = legacy_status(true, true);
        CHECK(lifecycle.invoke(legacy_request(
                  ProviderLifecycleWireProtocol::legacy_v2,
                  ProviderLifecycleOperation::post_replace)) ==
              result(ProviderLifecycleOperation::post_replace));
    }
    SECTION("post replace explicitly safe inactive") {
        FakeLifecycle lifecycle;
        lifecycle.raw = legacy_status(true, false, false, true);
        const auto actual = lifecycle.invoke(legacy_request(
            ProviderLifecycleWireProtocol::legacy_v2,
            ProviderLifecycleOperation::post_replace, true));
        CHECK(actual.disposition == ProviderLifecycleDisposition::safe_inactive);
        CHECK(actual.observed == safe_inactive_observed(
                                     ProviderLifecycleOperation::post_replace));
        REQUIRE(actual.reason);
        CHECK(actual.reason->id == "legacy.safe_inactive");
    }
}

TEST_CASE("ProviderLifecycle contains every unsatisfied legacy result") {
    const std::array malformed{
        nlohmann::json::object(),
        nlohmann::json{{"installed", "yes"}},
        legacy_status(true, false),
        nlohmann::json{{"installed", true},
                       {"active", true},
                       {"matchesCurrentConfiguration", false}},
        nlohmann::json{{"installed", true},
                       {"active", true},
                       {"matchesCurrentConfiguration", true},
                       {"component_id", "other.provider"}},
    };
    for (const auto& raw : malformed) {
        FakeLifecycle lifecycle;
        lifecycle.raw = raw;
        require_failure(
            lifecycle.invoke(legacy_request(
                ProviderLifecycleWireProtocol::legacy_v1,
                ProviderLifecycleOperation::install)),
            "legacy_contract_not_satisfied");
        CHECK(lifecycle.calls == 1);
    }
    FakeLifecycle oversized;
    oversized.raw = legacy_status(true, true);
    oversized.raw["padding"] =
        std::string(provider_lifecycle_raw_result_max_bytes + 1, 'x');
    require_failure(
        oversized.invoke(legacy_request(
            ProviderLifecycleWireProtocol::legacy_v1,
            ProviderLifecycleOperation::install)),
        "legacy_contract_not_satisfied");
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
