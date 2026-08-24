#include "arena/ports/durable_state_publisher.hpp"

#ifdef _WIN32
#include "arena/platform/durable_state_publisher.hpp"
#include "arena/platform/filesystem_identity.hpp"
#include "arena/platform/filesystem_path.hpp"
#include "../../src/platform/windows/durable_state_publisher_private.hpp"

#define NOMINMAX
#include <windows.h>
#include <aclapi.h>

#include <sodium.h>
#endif

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <limits>
#include <new>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
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

#ifdef _WIN32

class WindowsPublicationTempRoot final {
  public:
    WindowsPublicationTempRoot() {
        static std::atomic_uint64_t sequence{};
        const auto parent = std::filesystem::temp_directory_path();
        for (int attempt = 0; attempt < 100; ++attempt) {
            const auto candidate =
                parent /
                ("arena-dp-02-" + std::to_string(GetCurrentProcessId()) + "-" +
                 std::to_string(sequence.fetch_add(
                     1, std::memory_order_relaxed)));
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error)) {
                path_ = candidate;
                return;
            }
            if (error) throw std::runtime_error("temporary root creation failed");
        }
        throw std::runtime_error("temporary root allocation failed");
    }

    ~WindowsPublicationTempRoot() {
        std::error_code ignored;
        std::filesystem::remove_all(
            arena::platform::filesystem_api_path(path_), ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

[[nodiscard]] std::vector<std::byte> publication_bytes(
    const std::string_view text) {
    std::vector<std::byte> bytes;
    bytes.reserve(text.size());
    for (const unsigned char value : text) {
        bytes.push_back(static_cast<std::byte>(value));
    }
    return bytes;
}

void write_publication_file(const std::filesystem::path& path,
                            const std::string_view text) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream ||
        !stream.write(text.data(), static_cast<std::streamsize>(text.size()))) {
        throw std::runtime_error("test publication write failed");
    }
}

[[nodiscard]] std::string read_publication_file(
    const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>{stream},
            std::istreambuf_iterator<char>{}};
}

[[nodiscard]] ports::PublicationRequest windows_request(
    const std::filesystem::path& path,
    const std::span<const std::byte> bytes,
    const ports::PublicationPrecondition precondition =
        ports::PublicationPrecondition::absent,
    std::optional<std::string> expected = std::nullopt,
    const std::chrono::milliseconds timeout = 2s) {
    return {path, bytes, 1024, precondition, std::move(expected), timeout};
}

class WindowsTestHandle final {
  public:
    WindowsTestHandle() noexcept = default;
    explicit WindowsTestHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~WindowsTestHandle() {
        if (valid()) CloseHandle(handle_);
    }

    WindowsTestHandle(const WindowsTestHandle&) = delete;
    WindowsTestHandle& operator=(const WindowsTestHandle&) = delete;

    [[nodiscard]] bool valid() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }
    [[nodiscard]] HANDLE get() const noexcept { return handle_; }

  private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
};

class WindowsLocalAllocation final {
  public:
    explicit WindowsLocalAllocation(void* value = nullptr) noexcept
        : value_(value) {}
    ~WindowsLocalAllocation() {
        if (value_ != nullptr) LocalFree(value_);
    }

    WindowsLocalAllocation(const WindowsLocalAllocation&) = delete;
    WindowsLocalAllocation& operator=(const WindowsLocalAllocation&) = delete;

  private:
    void* value_{};
};

[[nodiscard]] std::string publication_sha256(
    const std::span<const std::byte> bytes) {
    std::array<unsigned char, crypto_hash_sha256_BYTES> digest_bytes{};
    crypto_hash_sha256(
        digest_bytes.data(),
        reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
    std::array<char, crypto_hash_sha256_BYTES * 2 + 1> encoded{};
    sodium_bin2hex(encoded.data(), encoded.size(), digest_bytes.data(),
                   digest_bytes.size());
    return {encoded.data(), encoded.size() - 1};
}

[[nodiscard]] std::filesystem::path find_lock_file(
    const std::filesystem::path& directory) {
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        const auto leaf = entry.path().filename().string();
        if (leaf.starts_with(".arena-state-lock-")) return entry.path();
    }
    throw std::runtime_error("publication lock fixture was not found");
}

[[nodiscard]] std::size_t stage_file_count(
    const std::filesystem::path& directory) {
    std::size_t count{};
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().filename().string().starts_with(
                ".arena-state-stage-")) {
            ++count;
        }
    }
    return count;
}

void grant_everyone_full_control(const std::filesystem::path& directory) {
    std::array<std::byte, SECURITY_MAX_SID_SIZE> sid_storage{};
    DWORD sid_size = static_cast<DWORD>(sid_storage.size());
    auto* everyone = reinterpret_cast<PSID>(sid_storage.data());
    if (!CreateWellKnownSid(WinWorldSid, nullptr, everyone, &sid_size)) {
        throw std::runtime_error("Everyone SID creation failed");
    }
    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = GENERIC_ALL;
    access.grfAccessMode = SET_ACCESS;
    access.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    BuildTrusteeWithSidW(&access.Trustee, everyone);
    PACL raw_acl{};
    if (SetEntriesInAclW(1, &access, nullptr, &raw_acl) != ERROR_SUCCESS) {
        throw std::runtime_error("broad ACL construction failed");
    }
    WindowsLocalAllocation acl{raw_acl};
    const auto api_path = arena::platform::filesystem_api_path(directory);
    if (SetNamedSecurityInfoW(
            const_cast<LPWSTR>(api_path.c_str()), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            nullptr, nullptr, raw_acl, nullptr) != ERROR_SUCCESS) {
        throw std::runtime_error("broad ACL application failed");
    }
}

enum class StageHookAction {
    collide_regular,
    collide_directory,
    collide_reparse,
    delete_after_close,
    corrupt_after_close,
    replace_after_close,
    hold_after_close,
    rename_lease_after_close,
    rename_parent_after_close,
};

struct StageHookContext {
    StageHookAction action{};
    std::filesystem::path reparse_target;
    std::filesystem::path displaced_stage;
    HANDLE held_handle{INVALID_HANDLE_VALUE};
    DWORD error{ERROR_SUCCESS};
    bool succeeded{};

    ~StageHookContext() {
        if (held_handle != nullptr && held_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(held_handle);
        }
    }

    void release_handle() noexcept {
        if (held_handle != nullptr && held_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(held_handle);
            held_handle = INVALID_HANDLE_VALUE;
        }
    }
};

void publication_stage_hook(
    const arena::platform::windows_detail::PublicationStageTestMoment moment,
    const std::filesystem::path& stage_path,
    void* opaque) noexcept {
    auto& context = *static_cast<StageHookContext*>(opaque);
    try {
        const bool before = moment == arena::platform::windows_detail::
                                          PublicationStageTestMoment::before_create;
        const bool before_action =
            context.action == StageHookAction::collide_regular ||
            context.action == StageHookAction::collide_directory ||
            context.action == StageHookAction::collide_reparse;
        if (before != before_action) return;
        context.succeeded = false;
        context.error = ERROR_SUCCESS;
        if (before && context.action == StageHookAction::collide_regular) {
            const WindowsTestHandle file{CreateFileW(
                stage_path.c_str(), GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
            context.succeeded = file.valid();
        } else if (before &&
                   context.action == StageHookAction::collide_directory) {
            context.succeeded = CreateDirectoryW(stage_path.c_str(), nullptr) != FALSE;
        } else if (before &&
                   context.action == StageHookAction::collide_reparse) {
            context.succeeded =
                CreateSymbolicLinkW(
                    stage_path.c_str(), context.reparse_target.c_str(),
                    SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != FALSE;
        } else if (!before &&
                   context.action == StageHookAction::delete_after_close) {
            context.succeeded = DeleteFileW(stage_path.c_str()) != FALSE;
        } else if (!before &&
                   context.action == StageHookAction::corrupt_after_close) {
            const WindowsTestHandle file{CreateFileW(
                stage_path.c_str(), GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
            constexpr std::string_view corruption{"corrupt"};
            DWORD written{};
            context.succeeded =
                file.valid() &&
                WriteFile(file.get(), corruption.data(),
                          static_cast<DWORD>(corruption.size()), &written,
                          nullptr) != FALSE &&
                written == corruption.size();
        } else if (!before &&
                   context.action == StageHookAction::replace_after_close) {
            context.displaced_stage = stage_path;
            context.displaced_stage += L".original";
            if (MoveFileW(stage_path.c_str(),
                          context.displaced_stage.c_str()) != FALSE) {
                const WindowsTestHandle replacement{CreateFileW(
                    stage_path.c_str(), GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
                constexpr std::string_view replacement_bytes{"candidate"};
                DWORD written{};
                context.succeeded =
                    replacement.valid() &&
                    WriteFile(replacement.get(), replacement_bytes.data(),
                              static_cast<DWORD>(replacement_bytes.size()),
                              &written, nullptr) != FALSE &&
                    written == replacement_bytes.size();
            }
        } else if (!before &&
                   context.action == StageHookAction::hold_after_close) {
            context.held_handle = CreateFileW(
                stage_path.c_str(), GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL, nullptr);
            context.succeeded = context.held_handle != nullptr &&
                                context.held_handle != INVALID_HANDLE_VALUE;
        } else if (!before &&
                   context.action == StageHookAction::rename_lease_after_close) {
            const auto lock_path = find_lock_file(stage_path.parent_path());
            context.displaced_stage = lock_path;
            context.displaced_stage += L".moved";
            context.succeeded =
                MoveFileW(lock_path.c_str(),
                          context.displaced_stage.c_str()) != FALSE;
        } else if (!before &&
                   context.action == StageHookAction::rename_parent_after_close) {
            const auto parent = stage_path.parent_path();
            context.displaced_stage = parent;
            context.displaced_stage += L".moved";
            context.succeeded =
                MoveFileW(parent.c_str(),
                          context.displaced_stage.c_str()) != FALSE;
        }
        if (!context.succeeded) context.error = GetLastError();
    } catch (...) {
        context.error = ERROR_NOT_ENOUGH_MEMORY;
        context.succeeded = false;
    }
}

class CountingFilesystemIdentity final : public ports::FilesystemIdentity {
  public:
    explicit CountingFilesystemIdentity(ports::FilesystemIdentity& delegate)
        : delegate_(delegate) {}

    std::size_t regular_admissions{};
    std::size_t directory_admissions{};

  protected:
    [[nodiscard]] std::unique_ptr<ports::AdmittedRegularFile>
    admit_regular_file_validated(
        const std::filesystem::path& path) override {
        ++regular_admissions;
        return delegate_.admit_regular_file(path);
    }

    [[nodiscard]] std::unique_ptr<ports::AdmittedDirectory>
    admit_directory_validated(
        const std::filesystem::path& path) override {
        ++directory_admissions;
        return delegate_.admit_directory(path);
    }

  private:
    ports::FilesystemIdentity& delegate_;
};

#endif

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

#ifdef _WIN32

TEST_CASE("Windows DurableStatePublisher activates an absent target") {
    WindowsPublicationTempRoot temporary;
    const auto target_path = temporary.path() / "state.json";
    const auto bytes = publication_bytes("abc");
    auto identity = arena::platform::make_filesystem_identity();
    auto publisher = arena::platform::make_durable_state_publisher(*identity);

    const auto actual = publisher->publish(windows_request(target_path, bytes));

    REQUIRE(actual.outcome == ports::PublicationOutcome::activated);
    REQUIRE(actual.candidate_sha256 == abc_sha);
    REQUIRE(actual.observed_target_sha256 == abc_sha);
    REQUIRE_FALSE(actual.primary_diagnostic.has_value());
    REQUIRE(read_publication_file(target_path) == "abc");
    REQUIRE(actual.cleanup_diagnostics.empty());
}

TEST_CASE("Windows DurableStatePublisher enforces absent and exact preconditions") {
    const auto candidate = publication_bytes("candidate");
    const auto prior = publication_bytes("prior");
    const auto prior_digest = publication_sha256(prior);

    SECTION("occupied absent target") {
        WindowsPublicationTempRoot temporary;
        const auto path = temporary.path() / "state.json";
        write_publication_file(path, "prior");
        auto identity = arena::platform::make_filesystem_identity();
        auto publisher = arena::platform::make_durable_state_publisher(*identity);
        const auto actual = publisher->publish(windows_request(path, candidate));
        REQUIRE(actual.outcome == ports::PublicationOutcome::not_activated);
        REQUIRE(actual.primary_diagnostic->code == "precondition_occupied");
        REQUIRE(read_publication_file(path) == "prior");
    }

    SECTION("exact prior success") {
        WindowsPublicationTempRoot temporary;
        const auto path = temporary.path() / "state.json";
        write_publication_file(path, "prior");
        auto identity = arena::platform::make_filesystem_identity();
        auto publisher = arena::platform::make_durable_state_publisher(*identity);
        const auto actual = publisher->publish(windows_request(
            path, candidate, ports::PublicationPrecondition::exact_sha256,
            prior_digest));
        REQUIRE(actual.outcome == ports::PublicationOutcome::activated);
        REQUIRE(actual.prior_sha256 == prior_digest);
        REQUIRE(actual.observed_target_sha256 ==
                publication_sha256(candidate));
        REQUIRE(read_publication_file(path) == "candidate");
    }

    SECTION("exact prior mismatch") {
        WindowsPublicationTempRoot temporary;
        const auto path = temporary.path() / "state.json";
        write_publication_file(path, "prior");
        auto identity = arena::platform::make_filesystem_identity();
        auto publisher = arena::platform::make_durable_state_publisher(*identity);
        const auto actual = publisher->publish(windows_request(
            path, candidate, ports::PublicationPrecondition::exact_sha256,
            std::string(64, '0')));
        REQUIRE(actual.outcome == ports::PublicationOutcome::not_activated);
        REQUIRE(actual.prior_sha256 == prior_digest);
        REQUIRE(actual.observed_target_sha256 == prior_digest);
        REQUIRE(read_publication_file(path) == "prior");
    }

    SECTION("missing and wrong-kind exact prior") {
        for (const bool wrong_kind : {false, true}) {
            WindowsPublicationTempRoot temporary;
            const auto path = temporary.path() / "state.json";
            if (wrong_kind) REQUIRE(std::filesystem::create_directory(path));
            auto identity = arena::platform::make_filesystem_identity();
            auto publisher =
                arena::platform::make_durable_state_publisher(*identity);
            const auto actual = publisher->publish(windows_request(
                path, candidate, ports::PublicationPrecondition::exact_sha256,
                prior_digest));
            REQUIRE(actual.outcome == ports::PublicationOutcome::not_activated);
            REQUIRE(actual.primary_diagnostic->code ==
                    "precondition_proof_failed");
        }
    }
}

TEST_CASE("Windows DurableStatePublisher rejects broad parent authority") {
    WindowsPublicationTempRoot temporary;
    grant_everyone_full_control(temporary.path());
    const auto bytes = publication_bytes("candidate");
    auto identity = arena::platform::make_filesystem_identity();
    auto publisher = arena::platform::make_durable_state_publisher(*identity);

    const auto actual = publisher->publish(
        windows_request(temporary.path() / "state.json", bytes));

    REQUIRE(actual.outcome == ports::PublicationOutcome::not_activated);
    REQUIRE(actual.primary_diagnostic->code == "parent_authority_rejected");
    REQUIRE_FALSE(std::filesystem::exists(temporary.path() / "state.json"));
}

TEST_CASE("Windows DurableStatePublisher rejects reparse exact prior when available") {
    WindowsPublicationTempRoot temporary;
    const auto source = temporary.path() / "source.json";
    const auto target_path = temporary.path() / "state.json";
    write_publication_file(source, "prior");
    if (CreateSymbolicLinkW(
            target_path.c_str(), source.c_str(),
            SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) == FALSE) {
        SKIP("symbolic-link creation is unavailable (win32=" << GetLastError()
                                                              << ")");
    }
    const auto candidate = publication_bytes("candidate");
    const auto prior = publication_bytes("prior");
    auto identity = arena::platform::make_filesystem_identity();
    auto publisher = arena::platform::make_durable_state_publisher(*identity);

    const auto actual = publisher->publish(windows_request(
        target_path, candidate, ports::PublicationPrecondition::exact_sha256,
        publication_sha256(prior)));

    REQUIRE(actual.outcome == ports::PublicationOutcome::not_activated);
    REQUIRE(actual.primary_diagnostic->code == "precondition_proof_failed");
    REQUIRE(read_publication_file(source) == "prior");
}

TEST_CASE("Windows DurableStatePublisher uses bounded writes and exact proofs") {
    WindowsPublicationTempRoot temporary;
    const auto target_path = temporary.path() / "state.json";
    const auto bytes = publication_bytes("candidate");
    std::size_t write_calls{};
    std::size_t proof_calls{};
    arena::platform::windows_detail::DurableStatePublisherTestControl control;
    control.maximum_write_chunk = 1;
    control.write_calls = &write_calls;
    control.exact_document_proofs = &proof_calls;
    auto native_identity = arena::platform::make_filesystem_identity();
    CountingFilesystemIdentity identity(*native_identity);
    auto publisher =
        arena::platform::windows_detail::make_durable_state_publisher_for_testing(
            identity, control);

    const auto actual = publisher->publish(windows_request(target_path, bytes));

    REQUIRE(actual.outcome == ports::PublicationOutcome::activated);
    REQUIRE(write_calls == bytes.size());
    REQUIRE(proof_calls == 3);
    REQUIRE(identity.directory_admissions == 1);
    REQUIRE(stage_file_count(temporary.path()) == 0);
}

TEST_CASE("Windows DurableStatePublisher contains a pre-activation stage flush failure") {
    const auto bytes = publication_bytes("candidate");
    WindowsPublicationTempRoot temporary;
    arena::platform::windows_detail::DurableStatePublisherTestControl control;
    control.fail_stage_flush = true;
    auto identity = arena::platform::make_filesystem_identity();
    auto publisher = arena::platform::windows_detail::
        make_durable_state_publisher_for_testing(*identity, control);

    const auto target_path = temporary.path() / "state.json";
    const auto actual = publisher->publish(windows_request(target_path, bytes));

    REQUIRE(actual.outcome == ports::PublicationOutcome::not_activated);
    REQUIRE(actual.primary_diagnostic->code == "stage_flush_failed");
    REQUIRE_FALSE(std::filesystem::exists(target_path));
    REQUIRE(stage_file_count(temporary.path()) == 0);
}

TEST_CASE("Windows DurableStatePublisher rejects hostile stage objects") {
    const auto bytes = publication_bytes("candidate");
    for (const auto action : {StageHookAction::collide_regular,
                              StageHookAction::collide_directory}) {
        WindowsPublicationTempRoot temporary;
        StageHookContext context;
        context.action = action;
        arena::platform::windows_detail::DurableStatePublisherTestControl control;
        control.stage_hook = publication_stage_hook;
        control.stage_hook_context = &context;
        auto identity = arena::platform::make_filesystem_identity();
        auto publisher = arena::platform::windows_detail::
            make_durable_state_publisher_for_testing(*identity, control);

        const auto actual = publisher->publish(windows_request(
            temporary.path() / "state.json", bytes));

        REQUIRE(context.succeeded);
        REQUIRE(actual.outcome == ports::PublicationOutcome::not_activated);
        REQUIRE((actual.primary_diagnostic->code == "stage_collision" ||
                 actual.primary_diagnostic->code == "stage_create_failed"));
        REQUIRE_FALSE(std::filesystem::exists(temporary.path() / "state.json"));
        REQUIRE(stage_file_count(temporary.path()) == 1);
    }
}

TEST_CASE("Windows DurableStatePublisher rejects a reparse stage collision when available") {
    WindowsPublicationTempRoot temporary;
    const auto source = temporary.path() / "source.json";
    write_publication_file(source, "foreign");
    StageHookContext context;
    context.action = StageHookAction::collide_reparse;
    context.reparse_target = source;
    arena::platform::windows_detail::DurableStatePublisherTestControl control;
    control.stage_hook = publication_stage_hook;
    control.stage_hook_context = &context;
    auto identity = arena::platform::make_filesystem_identity();
    auto publisher = arena::platform::windows_detail::
        make_durable_state_publisher_for_testing(*identity, control);

    const auto actual = publisher->publish(windows_request(
        temporary.path() / "state.json", publication_bytes("candidate")));

    if (!context.succeeded && context.error == ERROR_PRIVILEGE_NOT_HELD) {
        SKIP("symbolic-link creation is unavailable (win32=" << context.error
                                                              << ")");
    }
    INFO("stage reparse creation win32=" << context.error
                                         << " outcome="
                                         << static_cast<int>(actual.outcome)
                                         << " code="
                                         << (actual.primary_diagnostic.has_value()
                                                 ? actual.primary_diagnostic->code
                                                 : "none"));
    REQUIRE(context.succeeded);
    REQUIRE(actual.outcome == ports::PublicationOutcome::not_activated);
    REQUIRE(actual.primary_diagnostic->code == "stage_collision");
    REQUIRE(read_publication_file(source) == "foreign");
}

TEST_CASE("Windows DurableStatePublisher contains partial write and stage proof failures") {
    const auto bytes = publication_bytes("candidate");

    SECTION("partial write") {
        WindowsPublicationTempRoot temporary;
        std::size_t write_calls{};
        arena::platform::windows_detail::DurableStatePublisherTestControl control;
        control.simulated_write_failure_after_bytes = 2;
        control.write_calls = &write_calls;
        auto identity = arena::platform::make_filesystem_identity();
        auto publisher = arena::platform::windows_detail::
            make_durable_state_publisher_for_testing(*identity, control);
        const auto actual = publisher->publish(windows_request(
            temporary.path() / "state.json", bytes));
        REQUIRE(actual.outcome == ports::PublicationOutcome::not_activated);
        REQUIRE(actual.primary_diagnostic->code == "stage_write_failed");
        REQUIRE(write_calls == 1);
        REQUIRE(stage_file_count(temporary.path()) == 0);
    }

    for (const auto action : {StageHookAction::delete_after_close,
                              StageHookAction::corrupt_after_close,
                              StageHookAction::replace_after_close}) {
        WindowsPublicationTempRoot temporary;
        StageHookContext context;
        context.action = action;
        arena::platform::windows_detail::DurableStatePublisherTestControl control;
        control.stage_hook = publication_stage_hook;
        control.stage_hook_context = &context;
        auto identity = arena::platform::make_filesystem_identity();
        auto publisher = arena::platform::windows_detail::
            make_durable_state_publisher_for_testing(*identity, control);
        const auto actual = publisher->publish(windows_request(
            temporary.path() / "state.json", bytes));
        REQUIRE(context.succeeded);
        REQUIRE(actual.outcome == ports::PublicationOutcome::not_activated);
        REQUIRE(actual.primary_diagnostic->code == "stage_verification_failed");
        REQUIRE_FALSE(std::filesystem::exists(temporary.path() / "state.json"));
        if (action == StageHookAction::replace_after_close) {
            REQUIRE(actual.cleanup_diagnostics.size() == 1);
            REQUIRE(actual.cleanup_diagnostics.front().code ==
                    "stage_cleanup_identity_changed");
        } else {
            REQUIRE(actual.cleanup_diagnostics.empty());
            REQUIRE(stage_file_count(temporary.path()) == 0);
        }
    }
}

TEST_CASE("Windows DurableStatePublisher proves native move non-activation") {
    WindowsPublicationTempRoot temporary;
    StageHookContext context;
    context.action = StageHookAction::hold_after_close;
    arena::platform::windows_detail::DurableStatePublisherTestControl control;
    control.stage_hook = publication_stage_hook;
    control.stage_hook_context = &context;
    auto identity = arena::platform::make_filesystem_identity();
    auto publisher = arena::platform::windows_detail::
        make_durable_state_publisher_for_testing(*identity, control);
    const auto target_path = temporary.path() / "state.json";

    const auto actual = publisher->publish(
        windows_request(target_path, publication_bytes("candidate")));

    REQUIRE(context.succeeded);
    REQUIRE(actual.outcome == ports::PublicationOutcome::not_activated);
    REQUIRE(actual.primary_diagnostic->code == "move_not_activated");
    REQUIRE_FALSE(std::filesystem::exists(target_path));
    REQUIRE(actual.cleanup_diagnostics.size() == 1);
    context.release_handle();
}

TEST_CASE("Windows DurableStatePublisher classifies every move observation") {
    const auto bytes = publication_bytes("candidate");

    SECTION("reported failure and absence visible") {
        WindowsPublicationTempRoot temporary;
        arena::platform::windows_detail::DurableStatePublisherTestControl control;
        control.move_behavior = arena::platform::windows_detail::
            PublicationMoveTestBehavior::report_failure_without_move;
        auto identity = arena::platform::make_filesystem_identity();
        auto publisher = arena::platform::windows_detail::
            make_durable_state_publisher_for_testing(*identity, control);
        const auto path = temporary.path() / "state.json";
        const auto actual = publisher->publish(windows_request(path, bytes));
        REQUIRE(actual.outcome == ports::PublicationOutcome::not_activated);
        REQUIRE_FALSE(std::filesystem::exists(path));
    }

    SECTION("reported failure and candidate visible") {
        WindowsPublicationTempRoot temporary;
        arena::platform::windows_detail::DurableStatePublisherTestControl control;
        control.move_behavior = arena::platform::windows_detail::
            PublicationMoveTestBehavior::move_then_report_failure;
        auto identity = arena::platform::make_filesystem_identity();
        auto publisher = arena::platform::windows_detail::
            make_durable_state_publisher_for_testing(*identity, control);
        const auto path = temporary.path() / "state.json";
        const auto actual = publisher->publish(windows_request(path, bytes));
        REQUIRE(actual.outcome ==
                ports::PublicationOutcome::activated_durability_unproven);
        REQUIRE(actual.observed_target_sha256 == publication_sha256(bytes));
        REQUIRE(read_publication_file(path) == "candidate");
    }

    SECTION("candidate visible but target flush fails") {
        WindowsPublicationTempRoot temporary;
        arena::platform::windows_detail::DurableStatePublisherTestControl control;
        control.fail_target_flush = true;
        auto identity = arena::platform::make_filesystem_identity();
        auto publisher = arena::platform::windows_detail::
            make_durable_state_publisher_for_testing(*identity, control);
        const auto path = temporary.path() / "state.json";
        const auto actual = publisher->publish(windows_request(path, bytes));
        REQUIRE(actual.outcome ==
                ports::PublicationOutcome::activated_durability_unproven);
        REQUIRE(actual.observed_target_sha256 == publication_sha256(bytes));
    }

    SECTION("post-move target is uninspectable") {
        WindowsPublicationTempRoot temporary;
        arena::platform::windows_detail::DurableStatePublisherTestControl control;
        control.fail_post_move_inspection = true;
        auto identity = arena::platform::make_filesystem_identity();
        auto publisher = arena::platform::windows_detail::
            make_durable_state_publisher_for_testing(*identity, control);
        const auto path = temporary.path() / "state.json";
        const auto actual = publisher->publish(windows_request(path, bytes));
        REQUIRE(actual.outcome == ports::PublicationOutcome::indeterminate);
        REQUIRE(read_publication_file(path) == "candidate");
    }

    SECTION("prior remains visible after reported move failure") {
        WindowsPublicationTempRoot temporary;
        const auto path = temporary.path() / "state.json";
        write_publication_file(path, "prior");
        const auto prior = publication_bytes("prior");
        const auto prior_digest = publication_sha256(prior);
        arena::platform::windows_detail::DurableStatePublisherTestControl control;
        control.move_behavior = arena::platform::windows_detail::
            PublicationMoveTestBehavior::report_failure_without_move;
        auto identity = arena::platform::make_filesystem_identity();
        auto publisher = arena::platform::windows_detail::
            make_durable_state_publisher_for_testing(*identity, control);
        const auto actual = publisher->publish(windows_request(
            path, bytes, ports::PublicationPrecondition::exact_sha256,
            prior_digest));
        REQUIRE(actual.outcome == ports::PublicationOutcome::not_activated);
        REQUIRE(actual.observed_target_sha256 == prior_digest);
        REQUIRE(read_publication_file(path) == "prior");
    }
}

TEST_CASE("Windows DurableStatePublisher preserves outcome across cleanup failure") {
    const auto bytes = publication_bytes("candidate");
    for (const bool activate : {false, true}) {
        WindowsPublicationTempRoot temporary;
        arena::platform::windows_detail::DurableStatePublisherTestControl control;
        if (!activate) {
            control.move_behavior = arena::platform::windows_detail::
                PublicationMoveTestBehavior::report_failure_without_move;
        }
        control.fail_stage_cleanup = true;
        auto identity = arena::platform::make_filesystem_identity();
        auto publisher = arena::platform::windows_detail::
            make_durable_state_publisher_for_testing(*identity, control);

        const auto actual = publisher->publish(
            windows_request(temporary.path() / "state.json", bytes));

        REQUIRE(actual.outcome ==
                (activate ? ports::PublicationOutcome::activated
                          : ports::PublicationOutcome::not_activated));
        REQUIRE(actual.cleanup_diagnostics.size() == 1);
        REQUIRE(actual.cleanup_diagnostics.front().code ==
                "stage_cleanup_failed");
        REQUIRE(stage_file_count(temporary.path()) == (activate ? 0 : 1));
    }
}

TEST_CASE("Windows DurableStatePublisher obeys a contended lease deadline") {
    WindowsPublicationTempRoot temporary;
    const auto bytes = publication_bytes("candidate");
    const auto path = temporary.path() / "state.json";
    arena::platform::windows_detail::DurableStatePublisherTestControl control;
    control.move_behavior = arena::platform::windows_detail::
        PublicationMoveTestBehavior::report_failure_without_move;
    auto identity = arena::platform::make_filesystem_identity();
    auto priming_publisher = arena::platform::windows_detail::
        make_durable_state_publisher_for_testing(*identity, control);
    REQUIRE(priming_publisher->publish(windows_request(path, bytes)).outcome ==
            ports::PublicationOutcome::not_activated);
    const auto lock_path = find_lock_file(temporary.path());
    WindowsTestHandle handle{CreateFileW(
        lock_path.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    REQUIRE(handle.valid());
    OVERLAPPED lock_state{};
    REQUIRE(LockFileEx(handle.get(), LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD,
                       MAXDWORD, &lock_state) != FALSE);

    auto blocked_publisher =
        arena::platform::make_durable_state_publisher(*identity);
    const auto started = std::chrono::steady_clock::now();
    const auto actual = blocked_publisher->publish(
        windows_request(path, bytes, ports::PublicationPrecondition::absent,
                        std::nullopt, 20ms));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    REQUIRE(actual.outcome == ports::PublicationOutcome::not_activated);
    REQUIRE(actual.primary_diagnostic->code == "lease_deadline");
    REQUIRE(elapsed < 500ms);
    REQUIRE(UnlockFileEx(handle.get(), 0, MAXDWORD, MAXDWORD, &lock_state) !=
            FALSE);
}

TEST_CASE("Windows DurableStatePublisher rejects a hostile persistent lock") {
    WindowsPublicationTempRoot temporary;
    const auto bytes = publication_bytes("candidate");
    const auto path = temporary.path() / "state.json";
    arena::platform::windows_detail::DurableStatePublisherTestControl control;
    control.move_behavior = arena::platform::windows_detail::
        PublicationMoveTestBehavior::report_failure_without_move;
    auto identity = arena::platform::make_filesystem_identity();
    auto priming_publisher = arena::platform::windows_detail::
        make_durable_state_publisher_for_testing(*identity, control);
    REQUIRE(priming_publisher->publish(windows_request(path, bytes)).outcome ==
            ports::PublicationOutcome::not_activated);
    const auto lock_path = find_lock_file(temporary.path());
    REQUIRE(std::filesystem::remove(lock_path));
    REQUIRE(std::filesystem::create_directory(lock_path));

    auto publisher = arena::platform::make_durable_state_publisher(*identity);
    const auto actual = publisher->publish(windows_request(path, bytes));

    REQUIRE(actual.outcome == ports::PublicationOutcome::not_activated);
    REQUIRE(actual.primary_diagnostic->code == "lease_object_rejected");
}

TEST_CASE("Windows DurableStatePublisher rejects a broadly writable lock") {
    WindowsPublicationTempRoot temporary;
    const auto bytes = publication_bytes("candidate");
    const auto path = temporary.path() / "state.json";
    arena::platform::windows_detail::DurableStatePublisherTestControl control;
    control.move_behavior = arena::platform::windows_detail::
        PublicationMoveTestBehavior::report_failure_without_move;
    auto identity = arena::platform::make_filesystem_identity();
    auto priming_publisher = arena::platform::windows_detail::
        make_durable_state_publisher_for_testing(*identity, control);
    REQUIRE(priming_publisher->publish(windows_request(path, bytes)).outcome ==
            ports::PublicationOutcome::not_activated);
    const auto lock_path = find_lock_file(temporary.path());
    grant_everyone_full_control(lock_path);

    auto publisher = arena::platform::make_durable_state_publisher(*identity);
    const auto actual = publisher->publish(windows_request(path, bytes));

    REQUIRE(actual.outcome == ports::PublicationOutcome::not_activated);
    REQUIRE(actual.primary_diagnostic->code == "lease_object_rejected");
}

TEST_CASE("Windows DurableStatePublisher rejects a reparse lock when available") {
    WindowsPublicationTempRoot temporary;
    const auto bytes = publication_bytes("candidate");
    const auto path = temporary.path() / "state.json";
    arena::platform::windows_detail::DurableStatePublisherTestControl control;
    control.move_behavior = arena::platform::windows_detail::
        PublicationMoveTestBehavior::report_failure_without_move;
    auto identity = arena::platform::make_filesystem_identity();
    auto priming_publisher = arena::platform::windows_detail::
        make_durable_state_publisher_for_testing(*identity, control);
    REQUIRE(priming_publisher->publish(windows_request(path, bytes)).outcome ==
            ports::PublicationOutcome::not_activated);
    const auto lock_path = find_lock_file(temporary.path());
    REQUIRE(std::filesystem::remove(lock_path));
    const auto source = temporary.path() / "foreign.lock";
    write_publication_file(source, "foreign");
    if (CreateSymbolicLinkW(
            lock_path.c_str(), source.c_str(),
            SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) == FALSE) {
        SKIP("symbolic-link creation is unavailable (win32=" << GetLastError()
                                                              << ")");
    }

    auto publisher = arena::platform::make_durable_state_publisher(*identity);
    const auto actual = publisher->publish(windows_request(path, bytes));

    REQUIRE(actual.outcome == ports::PublicationOutcome::not_activated);
    REQUIRE(actual.primary_diagnostic->code == "lease_object_rejected");
    REQUIRE(read_publication_file(source) == "foreign");
}

TEST_CASE("Windows DurableStatePublisher releases native handles and lease") {
    WindowsPublicationTempRoot temporary;
    const auto path = temporary.path() / "state.json";
    auto identity = arena::platform::make_filesystem_identity();
    auto publisher = arena::platform::make_durable_state_publisher(*identity);
    REQUIRE(publisher->publish(
                windows_request(path, publication_bytes("candidate")))
                .outcome == ports::PublicationOutcome::activated);

    const auto lock_path = find_lock_file(temporary.path());
    WindowsTestHandle lock_handle{CreateFileW(
        lock_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    REQUIRE(lock_handle.valid());
    OVERLAPPED lock_state{};
    REQUIRE(LockFileEx(lock_handle.get(),
                       LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                       0, MAXDWORD, MAXDWORD, &lock_state) != FALSE);
    REQUIRE(UnlockFileEx(lock_handle.get(), 0, MAXDWORD, MAXDWORD,
                         &lock_state) != FALSE);

    WindowsTestHandle target_handle{CreateFileW(
        path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr)};
    REQUIRE(target_handle.valid());
    REQUIRE(stage_file_count(temporary.path()) == 0);
}

TEST_CASE("Windows DurableStatePublisher pins parent and lease during publication") {
    for (const auto action : {StageHookAction::rename_lease_after_close,
                              StageHookAction::rename_parent_after_close}) {
        WindowsPublicationTempRoot temporary;
        StageHookContext context;
        context.action = action;
        arena::platform::windows_detail::DurableStatePublisherTestControl control;
        control.stage_hook = publication_stage_hook;
        control.stage_hook_context = &context;
        auto identity = arena::platform::make_filesystem_identity();
        auto publisher = arena::platform::windows_detail::
            make_durable_state_publisher_for_testing(*identity, control);

        const auto actual = publisher->publish(windows_request(
            temporary.path() / "state.json", publication_bytes("candidate")));

        REQUIRE_FALSE(context.succeeded);
        REQUIRE((context.error == ERROR_SHARING_VIOLATION ||
                 context.error == ERROR_ACCESS_DENIED));
        REQUIRE(actual.outcome == ports::PublicationOutcome::activated);
        REQUIRE(read_publication_file(temporary.path() / "state.json") ==
                "candidate");
    }
}

TEST_CASE("Windows DurableStatePublisher serializes concurrent stale writers") {
    WindowsPublicationTempRoot temporary;
    const auto path = temporary.path() / "state.json";
    write_publication_file(path, "prior");
    const auto prior = publication_bytes("prior");
    const auto prior_digest = publication_sha256(prior);
    const std::array candidates{
        publication_bytes("candidate-one"),
        publication_bytes("candidate-two"),
    };
    std::array<ports::PublicationResult, 2> results;
    std::barrier start{3};
    std::array<std::thread, 2> writers;
    for (std::size_t index = 0; index < writers.size(); ++index) {
        writers[index] = std::thread([&, index] {
            auto identity = arena::platform::make_filesystem_identity();
            auto publisher =
                arena::platform::make_durable_state_publisher(*identity);
            start.arrive_and_wait();
            results[index] = publisher->publish(windows_request(
                path, candidates[index],
                ports::PublicationPrecondition::exact_sha256, prior_digest,
                2s));
        });
    }
    start.arrive_and_wait();
    for (auto& writer : writers) writer.join();

    const auto activated = std::count_if(
        results.begin(), results.end(), [](const auto& value) {
            return value.outcome == ports::PublicationOutcome::activated;
        });
    const auto not_activated = std::count_if(
        results.begin(), results.end(), [](const auto& value) {
            return value.outcome == ports::PublicationOutcome::not_activated;
        });
    REQUIRE(activated == 1);
    REQUIRE(not_activated == 1);
    REQUIRE((read_publication_file(path) == "candidate-one" ||
             read_publication_file(path) == "candidate-two"));
}

#endif
