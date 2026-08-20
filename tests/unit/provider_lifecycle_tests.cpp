#include "arena/ports/provider_lifecycle.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <new>
#include <stdexcept>

namespace {
using namespace arena::ports;

ProviderLifecycleObserved observations(ProviderLifecycleOperation operation,
                                       bool inactive = false) {
    switch (operation) {
    case ProviderLifecycleOperation::install:
        return {ProviderInstallationObservation::installed,
                inactive ? ProviderActivityObservation::inactive
                         : ProviderActivityObservation::active,
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
                inactive ? ProviderActivityObservation::inactive
                         : ProviderActivityObservation::active,
                ProviderConfigurationObservation::matches,
                ProviderReplacementObservation::safe};
    }
    throw std::logic_error("operation");
}

ProviderLifecycleResult result(ProviderLifecycleOperation operation,
                               ProviderLifecycleDisposition disposition =
                                   ProviderLifecycleDisposition::ready) {
    const bool inactive = disposition == ProviderLifecycleDisposition::safe_inactive;
    return {"provider.core", "1.2.3", operation, disposition,
            observations(operation, inactive),
            disposition == ProviderLifecycleDisposition::ready
                ? std::nullopt
                : std::optional{ProviderLifecycleReason{"provider.wait", "bounded"}}};
}

ProviderLifecycleRequest request(ProviderLifecycleOperation operation =
                                     ProviderLifecycleOperation::install) {
    return {"provider.core", "1.2.3", operation, "C:/provider.exe", "C:/work",
            {"--lifecycle"},
            {ProviderLifecycleWireProtocol::typed_v3,
             ProviderLegacyResultEnvelope::root, true, true},
            std::chrono::seconds{30}};
}

class FakeLifecycle final : public ProviderLifecycle {
  public:
    nlohmann::json raw = provider_lifecycle_result_to_json(
        result(ProviderLifecycleOperation::install));
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

std::string reason_id(const ProviderLifecycleResult& value) {
    REQUIRE(value.reason.has_value());
    return value.reason->id;
}
} // namespace

TEST_CASE("ProviderLifecycle invokes typed adapter once with an absolute deadline") {
    FakeLifecycle lifecycle;
    const auto before = std::chrono::steady_clock::now();
    const auto actual = lifecycle.invoke(request());
    CHECK(actual.disposition == ProviderLifecycleDisposition::ready);
    CHECK(lifecycle.calls == 1);
    CHECK(lifecycle.deadline > before);
}

TEST_CASE("ProviderLifecycle rejects invalid requests before adapter invocation") {
    FakeLifecycle lifecycle;
    auto candidate = request();
    SECTION("identifier") { candidate.component_id = "bad id"; }
    SECTION("version") { candidate.component_version = ""; }
    SECTION("executable") { candidate.executable.clear(); }
    SECTION("working directory") { candidate.working_directory.clear(); }
    SECTION("arguments") { candidate.arguments.assign(33, "x"); }
    SECTION("argument bytes") { candidate.arguments = {std::string(1025, 'x')}; }
    SECTION("timeout low") { candidate.timeout = std::chrono::milliseconds{0}; }
    SECTION("timeout high") { candidate.timeout = std::chrono::seconds{121}; }
    SECTION("typed legacy envelope") { candidate.policy.legacy_envelope = ProviderLegacyResultEnvelope::startup; }
    const auto actual = lifecycle.invoke(candidate);
    CHECK(actual.disposition == ProviderLifecycleDisposition::failed);
    CHECK(reason_id(actual) == "invalid_request");
    CHECK(lifecycle.calls == 0);
}

TEST_CASE("ProviderLifecycle converts adapter exceptions and propagates allocation") {
    FakeLifecycle lifecycle;
    lifecycle.throw_runtime = true;
    CHECK(reason_id(lifecycle.invoke(request())) == "adapter_exception");
    lifecycle.throw_runtime = false;
    lifecycle.throw_allocation = true;
    CHECK_THROWS_AS(lifecycle.invoke(request()), std::bad_alloc);
}

TEST_CASE("ProviderLifecycle accepts exact ready observations for every operation") {
    for (const auto operation : {ProviderLifecycleOperation::install,
                                 ProviderLifecycleOperation::uninstall,
                                 ProviderLifecycleOperation::pre_replace,
                                 ProviderLifecycleOperation::post_replace}) {
        FakeLifecycle lifecycle;
        lifecycle.raw = provider_lifecycle_result_to_json(result(operation));
        const auto actual = lifecycle.invoke(request(operation));
        CHECK(actual.disposition == ProviderLifecycleDisposition::ready);
        CHECK(actual.operation == operation);
    }
}

TEST_CASE("ProviderLifecycle gates safe inactive by operation and policy") {
    for (const auto operation : {ProviderLifecycleOperation::install,
                                 ProviderLifecycleOperation::post_replace}) {
        FakeLifecycle lifecycle;
        lifecycle.raw = provider_lifecycle_result_to_json(
            result(operation, ProviderLifecycleDisposition::safe_inactive));
        CHECK(lifecycle.invoke(request(operation)).disposition ==
              ProviderLifecycleDisposition::safe_inactive);
    }
    FakeLifecycle lifecycle;
    auto raw = provider_lifecycle_result_to_json(
        result(ProviderLifecycleOperation::install,
               ProviderLifecycleDisposition::safe_inactive));
    lifecycle.raw = raw;
    auto denied = request();
    denied.policy.allow_safe_inactive = false;
    CHECK(reason_id(lifecycle.invoke(denied)) == "disposition_not_allowed");
}

TEST_CASE("ProviderLifecycle fails closed on identity operation and observations") {
    FakeLifecycle lifecycle;
    SECTION("identity") {
        lifecycle.raw["component_id"] = "other";
        CHECK(reason_id(lifecycle.invoke(request())) == "identity_mismatch");
    }
    SECTION("operation") {
        lifecycle.raw = provider_lifecycle_result_to_json(
            result(ProviderLifecycleOperation::uninstall));
        CHECK(reason_id(lifecycle.invoke(request())) == "operation_mismatch");
    }
    SECTION("observations") {
        lifecycle.raw["observed"]["activity"] = "inactive";
        CHECK(reason_id(lifecycle.invoke(request())) == "observation_mismatch");
    }
}

TEST_CASE("ProviderLifecycle rejects malformed typed results") {
    FakeLifecycle lifecycle;
    SECTION("extra field") { lifecycle.raw["provider"] = "forbidden"; }
    SECTION("missing field") { lifecycle.raw.erase("observed"); }
    SECTION("wrong schema") { lifecycle.raw["schema"] = "wrong"; }
    SECTION("reason required") {
        lifecycle.raw["disposition"] = "failed";
        lifecycle.raw.erase("reason");
    }
    CHECK(reason_id(lifecycle.invoke(request())) == "malformed_result");
}

TEST_CASE("ProviderLifecycle preserves opaque provider reasons") {
    FakeLifecycle lifecycle;
    lifecycle.raw = provider_lifecycle_result_to_json(
        result(ProviderLifecycleOperation::install,
               ProviderLifecycleDisposition::safe_inactive));
    const auto actual = lifecycle.invoke(request());
    REQUIRE(actual.reason);
    CHECK(actual.reason->id == "provider.wait");
    CHECK(actual.reason->message == "bounded");
}

TEST_CASE("ProviderLifecycle legacy protocols return fixed unsupported failure") {
    FakeLifecycle lifecycle;
    auto legacy = request();
    legacy.policy.wire_protocol = ProviderLifecycleWireProtocol::legacy_v1;
    const auto actual = lifecycle.invoke(legacy);
    CHECK(reason_id(actual) == "legacy_contract_not_satisfied");
    CHECK(lifecycle.calls == 0);
}

TEST_CASE("provider lifecycle serializer emits exact deterministic neutral JSON") {
    const auto value = result(ProviderLifecycleOperation::post_replace);
    const auto first = provider_lifecycle_result_to_json(value);
    const auto second = provider_lifecycle_result_to_json(value);
    CHECK(first == second);
    CHECK(first.size() == 7);
    CHECK(first.at("schema") == "arena_provider_lifecycle_result_v1");
    CHECK(first.at("operation") == "post_replace");
    CHECK_FALSE(first.contains("reason"));
    CHECK_FALSE(first.contains("provider"));
}

TEST_CASE("provider lifecycle serializer rejects invalid constructed results") {
    auto invalid = result(ProviderLifecycleOperation::install);
    SECTION("observation") { invalid.observed.activity = ProviderActivityObservation::inactive; }
    SECTION("reason") {
        invalid.disposition = ProviderLifecycleDisposition::failed;
        invalid.reason.reset();
    }
    CHECK_THROWS_AS(provider_lifecycle_result_to_json(invalid), std::invalid_argument);
}
