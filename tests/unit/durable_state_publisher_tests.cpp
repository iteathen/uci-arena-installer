#include "arena/ports/durable_state_publisher.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <new>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

namespace ports = arena::ports;
using namespace std::chrono_literals;

constexpr std::string_view empty_sha =
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
constexpr std::string_view abc_sha =
    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
constexpr std::string_view prior_sha =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

enum class Behavior { return_result, throw_runtime, throw_unknown, throw_allocation };

class FakePublisher final : public ports::DurableStatePublisher {
  public:
    ports::PublicationResult result{};
    Behavior behavior{Behavior::return_result};
    int calls{};
    std::string digest;
    std::string stage_id;
    std::chrono::steady_clock::time_point deadline{};

  protected:
    ports::PublicationResult publish_validated(
        const ports::PublicationRequest&,
        const std::string_view candidate_sha256,
        const std::string_view random_stage_id,
        const std::chrono::steady_clock::time_point supplied_deadline) override {
        ++calls;
        digest = candidate_sha256;
        stage_id = random_stage_id;
        deadline = supplied_deadline;
        if (behavior == Behavior::throw_runtime) throw std::runtime_error("private");
        if (behavior == Behavior::throw_unknown) throw 7;
        if (behavior == Behavior::throw_allocation) throw std::bad_alloc{};
        return result;
    }
};

[[nodiscard]] std::filesystem::path target() {
    return (std::filesystem::temp_directory_path() / "arena-dp-01" / "state.json")
        .lexically_normal();
}

[[nodiscard]] ports::PublicationRequest request(
    const std::span<const std::byte> bytes,
    const ports::PublicationPrecondition precondition =
        ports::PublicationPrecondition::absent) {
    return {target(), bytes, 1024, precondition,
            precondition == ports::PublicationPrecondition::exact_sha256
                ? std::optional<std::string>{prior_sha}
                : std::nullopt,
            50ms};
}

[[nodiscard]] ports::PublicationDiagnostic diagnostic(
    std::string code = "platform_error", std::string message = "bounded") {
    return {std::move(code), std::move(message)};
}

[[nodiscard]] ports::PublicationResult result(
    const ports::PublicationOutcome outcome, const std::string_view candidate,
    const std::uint64_t size = 3) {
    ports::PublicationResult value{outcome, size, std::string{candidate}, std::nullopt,
                                   std::nullopt, std::nullopt, {}};
    if (outcome == ports::PublicationOutcome::activated ||
        outcome == ports::PublicationOutcome::activated_durability_unproven) {
        value.observed_target_sha256 = candidate;
    }
    if (outcome != ports::PublicationOutcome::activated) {
        value.primary_diagnostic = diagnostic();
    }
    return value;
}

void require_common_failure(const ports::PublicationResult& value,
                            const ports::PublicationOutcome outcome,
                            const std::string& code) {
    REQUIRE(value.outcome == outcome);
    REQUIRE(value.primary_diagnostic.has_value());
    REQUIRE(value.primary_diagnostic->code == code);
    REQUIRE(value.primary_diagnostic->message.empty());
}

} // namespace

TEST_CASE("DurableStatePublisher exposes fixed contract bounds") {
    STATIC_REQUIRE(ports::publication_path_max_utf8_bytes == 131072);
    STATIC_REQUIRE(ports::publication_diagnostic_code_max_bytes == 64);
    STATIC_REQUIRE(ports::publication_diagnostic_message_max_utf8_bytes == 512);
    STATIC_REQUIRE(ports::publication_sha256_hex_bytes == 64);
    STATIC_REQUIRE(ports::publication_global_max_bytes == 67108864);
    STATIC_REQUIRE(ports::publication_cleanup_diagnostics_max_count == 16);
    STATIC_REQUIRE(ports::publication_timeout_minimum == 1ms);
    STATIC_REQUIRE(ports::publication_timeout_maximum == 120s);
}

TEST_CASE("DurableStatePublisher passes exact digest random id and one deadline") {
    const std::array bytes{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
    std::set<std::string> stage_ids;
    for (int index = 0; index < 32; ++index) {
        FakePublisher publisher;
        publisher.result = result(ports::PublicationOutcome::activated, abc_sha);
        const auto before = std::chrono::steady_clock::now();
        const auto actual = publisher.publish(request(bytes));
        const auto after = std::chrono::steady_clock::now();
        REQUIRE(actual.outcome == ports::PublicationOutcome::activated);
        REQUIRE(publisher.calls == 1);
        REQUIRE(publisher.digest == abc_sha);
        REQUIRE(publisher.stage_id.size() == 32);
        REQUIRE(publisher.stage_id.find_first_not_of("0123456789abcdef") == std::string::npos);
        REQUIRE(publisher.deadline >= before + 50ms);
        REQUIRE(publisher.deadline <= after + 50ms);
        stage_ids.insert(publisher.stage_id);
    }
    REQUIRE(stage_ids.size() == 32);
}

TEST_CASE("DurableStatePublisher accepts absent and exact digest requests") {
    const std::array bytes{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
    for (const auto precondition : {ports::PublicationPrecondition::absent,
                                    ports::PublicationPrecondition::exact_sha256}) {
        FakePublisher publisher;
        publisher.result = result(ports::PublicationOutcome::activated, abc_sha);
        REQUIRE(publisher.publish(request(bytes, precondition)).outcome ==
                ports::PublicationOutcome::activated);
        REQUIRE(publisher.calls == 1);
    }
}

TEST_CASE("DurableStatePublisher rejects invalid requests before adapter invocation") {
    const std::array bytes{std::byte{'a'}};
    FakePublisher publisher;
    auto candidate = request(bytes);
    const auto relative = std::filesystem::path{"state.json"};
    const auto root = target().root_path();
    for (const auto& path : {std::filesystem::path{}, relative, root,
                             target().parent_path() / "x" / ".." / "state.json",
                             target().parent_path() / std::string{"bad\nname"}}) {
        candidate.target = path;
        require_common_failure(publisher.publish(candidate),
                               ports::PublicationOutcome::not_activated,
                               "invalid_request");
    }
    candidate = request({});
    require_common_failure(publisher.publish(candidate),
                           ports::PublicationOutcome::not_activated, "invalid_request");
    candidate = request(bytes);
    candidate.maximum_bytes = 0;
    require_common_failure(publisher.publish(candidate),
                           ports::PublicationOutcome::not_activated, "invalid_request");
    candidate.maximum_bytes = ports::publication_global_max_bytes + 1;
    require_common_failure(publisher.publish(candidate),
                           ports::PublicationOutcome::not_activated, "invalid_request");
    candidate = request(bytes);
    candidate.maximum_bytes = bytes.size() - 1;
    require_common_failure(publisher.publish(candidate),
                           ports::PublicationOutcome::not_activated, "invalid_request");
    candidate = request(bytes);
    candidate.timeout = 0ms;
    require_common_failure(publisher.publish(candidate),
                           ports::PublicationOutcome::not_activated, "invalid_request");
    candidate = request(bytes);
    candidate.timeout = 120s + 1ms;
    require_common_failure(publisher.publish(candidate),
                           ports::PublicationOutcome::not_activated, "invalid_request");
    candidate = request(bytes);
    candidate.expected_prior_sha256 = std::string{prior_sha};
    require_common_failure(publisher.publish(candidate),
                           ports::PublicationOutcome::not_activated, "invalid_request");
    candidate = request(bytes, ports::PublicationPrecondition::exact_sha256);
    candidate.expected_prior_sha256 = "ABC";
    require_common_failure(publisher.publish(candidate),
                           ports::PublicationOutcome::not_activated, "invalid_request");
    candidate.precondition = static_cast<ports::PublicationPrecondition>(99);
    require_common_failure(publisher.publish(candidate),
                           ports::PublicationOutcome::not_activated, "invalid_request");
    REQUIRE(publisher.calls == 0);
}

TEST_CASE("DurableStatePublisher accepts truthful envelopes for every outcome") {
    const std::array bytes{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
    for (const auto outcome : {ports::PublicationOutcome::activated,
                               ports::PublicationOutcome::not_activated,
                               ports::PublicationOutcome::activated_durability_unproven,
                               ports::PublicationOutcome::indeterminate}) {
        FakePublisher publisher;
        publisher.result = result(outcome, abc_sha);
        publisher.result.cleanup_diagnostics.push_back(diagnostic("cleanup_failed"));
        const auto actual = publisher.publish(request(bytes));
        REQUIRE(actual.outcome == outcome);
        REQUIRE(actual.cleanup_diagnostics.size() == 1);
    }
}

TEST_CASE("DurableStatePublisher rejects untruthful outcome evidence") {
    const std::array bytes{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
    const auto verify = [&](ports::PublicationResult envelope) {
        FakePublisher publisher;
        publisher.result = std::move(envelope);
        require_common_failure(publisher.publish(request(bytes)),
                               ports::PublicationOutcome::indeterminate,
                               "adapter_contract_violation");
    };
    auto envelope = result(ports::PublicationOutcome::activated, abc_sha);
    envelope.observed_target_sha256.reset();
    verify(envelope);
    envelope = result(ports::PublicationOutcome::not_activated, abc_sha);
    envelope.observed_target_sha256 = abc_sha;
    verify(envelope);
    envelope = result(ports::PublicationOutcome::activated_durability_unproven, abc_sha);
    envelope.observed_target_sha256.reset();
    verify(envelope);
    for (const auto outcome : {ports::PublicationOutcome::not_activated,
                               ports::PublicationOutcome::activated_durability_unproven,
                               ports::PublicationOutcome::indeterminate}) {
        envelope = result(outcome, abc_sha);
        envelope.primary_diagnostic.reset();
        verify(envelope);
    }
    envelope = result(ports::PublicationOutcome::activated, abc_sha);
    envelope.primary_diagnostic = diagnostic();
    verify(envelope);
}

TEST_CASE("DurableStatePublisher rejects invalid adapter identity and diagnostics") {
    const std::array bytes{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
    const auto verify = [&](ports::PublicationResult envelope) {
        FakePublisher publisher;
        publisher.result = std::move(envelope);
        require_common_failure(publisher.publish(request(bytes)),
                               ports::PublicationOutcome::indeterminate,
                               "adapter_contract_violation");
    };
    auto envelope = result(ports::PublicationOutcome::activated, abc_sha, 4);
    verify(envelope);
    envelope = result(ports::PublicationOutcome::activated, prior_sha);
    verify(envelope);
    envelope = result(ports::PublicationOutcome::not_activated, abc_sha);
    envelope.prior_sha256 = "BAD";
    verify(envelope);
    for (auto invalid : {diagnostic("bad code"), diagnostic(std::string(65, 'x')),
                         diagnostic("code", std::string(513, 'x')),
                         diagnostic("code", "abc"), diagnostic("code", "line\nbreak")}) {
        envelope = result(ports::PublicationOutcome::not_activated, abc_sha);
        envelope.primary_diagnostic = std::move(invalid);
        verify(envelope);
    }
    envelope = result(ports::PublicationOutcome::activated, abc_sha);
    envelope.cleanup_diagnostics.assign(17, diagnostic("cleanup"));
    verify(envelope);
}

TEST_CASE("DurableStatePublisher maps adapter exception to indeterminate") {
    const std::array bytes{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
    FakePublisher publisher;
    publisher.behavior = Behavior::throw_runtime;
    const auto actual = publisher.publish(request(bytes));
    require_common_failure(actual, ports::PublicationOutcome::indeterminate,
                           "adapter_exception");
    REQUIRE(actual.candidate_sha256 == abc_sha);
    publisher.behavior = Behavior::throw_unknown;
    require_common_failure(publisher.publish(request(bytes)),
                           ports::PublicationOutcome::indeterminate,
                           "adapter_exception");
    publisher.behavior = Behavior::throw_allocation;
    REQUIRE_THROWS_AS(publisher.publish(request(bytes)), std::bad_alloc);
}

TEST_CASE("DurableStatePublisher hashes the exact candidate bytes") {
    const std::array bytes{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
    FakePublisher publisher;
    publisher.result = result(ports::PublicationOutcome::activated, abc_sha);
    REQUIRE(publisher.publish(request(bytes)).candidate_sha256 == abc_sha);
    REQUIRE(empty_sha != abc_sha);
}
