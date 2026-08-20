#include "arena/application/installer/artifact_acquirer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace arena::application::installer::artifact_acquirer_test_seam {
std::string_view https_protocols() noexcept;
long https_follow_location() noexcept;
long https_fail_on_error() noexcept;
long https_verify_peer() noexcept;
long https_verify_host() noexcept;
long https_redirect_limit() noexcept;
long https_low_speed_limit() noexcept;
long https_low_speed_window() noexcept;
std::chrono::milliseconds https_connect_timeout(
    std::chrono::milliseconds requested,
    std::chrono::milliseconds remaining) noexcept;
bool hard_byte_cap_allows(
    std::uint64_t total,
    std::size_t incoming,
    std::uint64_t expected) noexcept;
} // namespace arena::application::installer::artifact_acquirer_test_seam

namespace {

namespace app = arena::application::installer;
namespace ports = arena::ports;
namespace fs = std::filesystem;
namespace seam =
    arena::application::installer::artifact_acquirer_test_seam;

constexpr std::string_view kAbcSha256 =
    "ba7816bf8f01cfea414140de5dae2223"
    "b00361a396177a9cb410ff61f20015ad";

[[nodiscard]] std::vector<std::byte> bytes_of(
    const std::string_view text) {
    std::vector<std::byte> result;
    result.reserve(text.size());
    for (const unsigned char value : text) {
        result.push_back(static_cast<std::byte>(value));
    }
    return result;
}

[[nodiscard]] ports::FilesystemIdentityToken token(const unsigned int seed) {
    ports::FilesystemIdentityToken result{};
    result.fill(static_cast<std::byte>(seed & 0xffU));
    return result;
}

[[nodiscard]] ports::FilesystemObservation observation(
    const std::uint64_t size,
    const unsigned int seed) {
    return ports::FilesystemObservation{
        ports::FilesystemObjectKind::regular_file,
        size,
        token(seed),
        token(seed + 1U),
    };
}

[[nodiscard]] ports::FilesystemObservation directory_observation(
    const unsigned int seed) {
    return ports::FilesystemObservation{
        ports::FilesystemObjectKind::directory,
        0,
        token(seed),
        token(seed + 1U),
    };
}

struct FakeFileData final {
    std::vector<std::byte> bytes;
    ports::FilesystemObservation initial;
    ports::FilesystemObservation current;
    std::optional<ports::PathBindingState> forced_binding;
    std::chrono::milliseconds read_delay{};
};

class FakeFilesystemIdentity;

class FakeRegularFile final : public ports::AdmittedRegularFile {
  public:
    FakeRegularFile(
        FakeFilesystemIdentity& filesystem,
        fs::path path,
        std::shared_ptr<FakeFileData> data);

    [[nodiscard]] const fs::path& absolute_path() const noexcept override {
        return path_;
    }

    [[nodiscard]] const ports::FilesystemObservation&
    initial_observation() const noexcept override {
        return initial_;
    }

    [[nodiscard]] std::size_t read_at(
        std::uint64_t offset,
        std::span<std::byte> destination) override;

    [[nodiscard]] ports::FilesystemObservation
    observe_open_object() const override {
        return data_->current;
    }

    [[nodiscard]] ports::PathBindingState
    observe_path_binding() const override;

  private:
    FakeFilesystemIdentity& filesystem_;
    fs::path path_;
    std::shared_ptr<FakeFileData> data_;
    ports::FilesystemObservation initial_;
};

class FakeDirectory final : public ports::AdmittedDirectory {
  public:
    FakeDirectory(
        FakeFilesystemIdentity& filesystem,
        fs::path path,
        ports::FilesystemObservation initial);

    [[nodiscard]] ports::FilesystemObservation
    observe_open_object() const override {
        return initial_observation();
    }

    [[nodiscard]] ports::PathBindingState
    observe_path_binding() const override;

  protected:
    [[nodiscard]] std::unique_ptr<ports::AdmittedRegularFile>
    open_regular_file_validated(std::string_view leaf) const override;

    [[nodiscard]] std::unique_ptr<ports::AdmittedDirectory>
    open_directory_validated(std::string_view leaf) const override;

  private:
    FakeFilesystemIdentity& filesystem_;
};

class FakeFilesystemIdentity final : public ports::FilesystemIdentity {
  public:
    void add_directory(const fs::path& path) {
        directories_[path] = directory_observation(next_seed_++);
    }

    std::shared_ptr<FakeFileData> add_file(
        const fs::path& path,
        const std::string_view bytes) {
        auto data = std::make_shared<FakeFileData>();
        data->bytes = bytes_of(bytes);
        data->initial = observation(data->bytes.size(), next_seed_++);
        data->current = data->initial;
        files_[path] = data;
        return data;
    }

    [[nodiscard]] bool has_directory(const fs::path& path) const {
        return directories_.contains(path);
    }

    [[nodiscard]] ports::PathBindingState binding_for(
        const fs::path& path,
        const std::shared_ptr<FakeFileData>& expected) const {
        if (expected->forced_binding.has_value()) {
            return *expected->forced_binding;
        }
        const auto found = files_.find(path);
        if (found == files_.end()) {
            return ports::PathBindingState::missing;
        }
        return found->second == expected
                   ? ports::PathBindingState::same_object
                   : ports::PathBindingState::different_object;
    }

    void remove_file(const fs::path& path) {
        files_.erase(path);
    }

    void move_file(const fs::path& from, const fs::path& to) {
        const auto found = files_.find(from);
        REQUIRE(found != files_.end());
        auto data = found->second;
        files_.erase(found);
        files_[to] = std::move(data);
    }

    [[nodiscard]] std::shared_ptr<FakeFileData>
    file_data(const fs::path& path) const {
        const auto found = files_.find(path);
        return found == files_.end() ? nullptr : found->second;
    }

    [[nodiscard]] std::unique_ptr<ports::AdmittedRegularFile>
    open_file(const fs::path& path) {
        const auto found = files_.find(path);
        if (found == files_.end()) {
            throw ports::FilesystemIdentityException(
                ports::FilesystemIdentityErrorCode::missing,
                "fake file is missing");
        }
        return std::make_unique<FakeRegularFile>(*this, path, found->second);
    }

    [[nodiscard]] std::unique_ptr<ports::AdmittedDirectory>
    open_directory(const fs::path& path) {
        const auto found = directories_.find(path);
        if (found == directories_.end()) {
            throw ports::FilesystemIdentityException(
                ports::FilesystemIdentityErrorCode::missing,
                "fake directory is missing");
        }
        return std::make_unique<FakeDirectory>(*this, path, found->second);
    }

  protected:
    [[nodiscard]] std::unique_ptr<ports::AdmittedRegularFile>
    admit_regular_file_validated(const fs::path& path) override {
        return open_file(path);
    }

    [[nodiscard]] std::unique_ptr<ports::AdmittedDirectory>
    admit_directory_validated(const fs::path& path) override {
        return open_directory(path);
    }

  private:
    std::map<fs::path, std::shared_ptr<FakeFileData>> files_;
    std::map<fs::path, ports::FilesystemObservation> directories_;
    unsigned int next_seed_{20U};
};

FakeRegularFile::FakeRegularFile(
    FakeFilesystemIdentity& filesystem,
    fs::path path,
    std::shared_ptr<FakeFileData> data)
    : filesystem_(filesystem),
      path_(std::move(path)),
      data_(std::move(data)),
      initial_(data_->initial) {}

std::size_t FakeRegularFile::read_at(
    const std::uint64_t offset,
    const std::span<std::byte> destination) {
    if (data_->read_delay > std::chrono::milliseconds::zero()) {
        std::this_thread::sleep_for(data_->read_delay);
    }
    if (offset >= data_->bytes.size() || destination.empty()) {
        return 0;
    }
    const auto count = (std::min)(
        destination.size(),
        data_->bytes.size() - static_cast<std::size_t>(offset));
    std::copy_n(
        data_->bytes.begin() + static_cast<std::ptrdiff_t>(offset),
        count,
        destination.begin());
    return count;
}

ports::PathBindingState FakeRegularFile::observe_path_binding() const {
    return filesystem_.binding_for(path_, data_);
}

FakeDirectory::FakeDirectory(
    FakeFilesystemIdentity& filesystem,
    fs::path path,
    ports::FilesystemObservation initial)
    : ports::AdmittedDirectory(std::move(path), std::move(initial)),
      filesystem_(filesystem) {}

ports::PathBindingState FakeDirectory::observe_path_binding() const {
    return filesystem_.has_directory(absolute_path())
               ? ports::PathBindingState::same_object
               : ports::PathBindingState::missing;
}

std::unique_ptr<ports::AdmittedRegularFile>
FakeDirectory::open_regular_file_validated(const std::string_view leaf) const {
    return filesystem_.open_file(absolute_path() / std::string{leaf});
}

std::unique_ptr<ports::AdmittedDirectory>
FakeDirectory::open_directory_validated(const std::string_view leaf) const {
    return filesystem_.open_directory(absolute_path() / std::string{leaf});
}

struct CacheState final {
    fs::path target_path;
    ports::ArtifactCacheActivationOutcome activation_outcome{
        ports::ArtifactCacheActivationOutcome::activated};
    std::optional<std::string> occupied_winner_bytes;
    bool corrupt_finished_candidate{false};
    bool cleanup_failure{false};
    bool stop_on_candidate_write{false};
    std::stop_source* stop_source{};
    std::size_t lease_calls{};
    std::size_t candidate_creates{};
    std::size_t candidate_cleanups{};
    std::size_t discard_calls{};
    std::size_t activate_calls{};
    std::vector<std::string> candidate_ids;
    std::vector<std::string> quarantine_ids;
    std::chrono::steady_clock::time_point captured_lease_deadline{};
};

class FakeCandidate final : public ports::ArtifactCacheCandidate {
  public:
    FakeCandidate(
        FakeFilesystemIdentity& filesystem,
        std::shared_ptr<CacheState> state,
        fs::path path,
        const std::uint64_t expected_size)
        : filesystem_(filesystem),
          state_(std::move(state)),
          path_(std::move(path)),
          expected_size_(expected_size) {}

    [[nodiscard]] const fs::path& path() const noexcept override {
        return path_;
    }

    void write(const std::span<const std::byte> bytes) override {
        REQUIRE_FALSE(finished_);
        REQUIRE(bytes_.size() + bytes.size() <= expected_size_);
        bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
        if (state_->stop_on_candidate_write && state_->stop_source != nullptr) {
            state_->stop_source->request_stop();
        }
    }

    void finish() override {
        REQUIRE_FALSE(finished_);
        finished_ = true;
        std::string contents{
            reinterpret_cast<const char*>(bytes_.data()), bytes_.size()};
        if (state_->corrupt_finished_candidate && !contents.empty()) {
            contents.back() = contents.back() == 'x' ? 'y' : 'x';
        }
        filesystem_.add_file(path_, contents);
    }

    [[nodiscard]] std::optional<ports::ArtifactCacheDiagnostic>
    cleanup() noexcept override {
        ++state_->candidate_cleanups;
        filesystem_.remove_file(path_);
        if (state_->cleanup_failure) {
            return ports::ArtifactCacheDiagnostic{
                "cleanup_failed", "fake cleanup failure"};
        }
        return std::nullopt;
    }

    [[nodiscard]] bool finished() const noexcept {
        return finished_;
    }

  private:
    FakeFilesystemIdentity& filesystem_;
    std::shared_ptr<CacheState> state_;
    fs::path path_;
    std::uint64_t expected_size_{};
    std::vector<std::byte> bytes_;
    bool finished_{};
};

class FakeLease final : public ports::ArtifactCacheLease {
  public:
    FakeLease(
        FakeFilesystemIdentity& filesystem,
        std::shared_ptr<CacheState> state)
        : filesystem_(filesystem), state_(std::move(state)) {}

    [[nodiscard]] const fs::path& target_path() const noexcept override {
        return state_->target_path;
    }

    [[nodiscard]] std::unique_ptr<ports::ArtifactCacheCandidate>
    create_candidate(
        const std::string_view random_id,
        const std::uint64_t expected_size_bytes) override {
        ++state_->candidate_creates;
        state_->candidate_ids.emplace_back(random_id);
        const auto candidate_path =
            state_->target_path.parent_path() /
            (".candidate-" + std::string{random_id});
        return std::make_unique<FakeCandidate>(
            filesystem_, state_, candidate_path, expected_size_bytes);
    }

    void discard_exact_target(
        const ports::FilesystemIdentityToken& expected_object_identity,
        const std::string_view random_quarantine_id) override {
        ++state_->discard_calls;
        state_->quarantine_ids.emplace_back(random_quarantine_id);
        const auto data = filesystem_.file_data(state_->target_path);
        REQUIRE(data != nullptr);
        REQUIRE(data->initial.object_identity == expected_object_identity);
        filesystem_.remove_file(state_->target_path);
    }

    [[nodiscard]] ports::ArtifactCacheActivationResult activate_candidate(
        ports::ArtifactCacheCandidate& candidate,
        const ports::FilesystemIdentityToken& expected_candidate_identity) override {
        ++state_->activate_calls;
        auto& fake = dynamic_cast<FakeCandidate&>(candidate);
        REQUIRE(fake.finished());
        const auto data = filesystem_.file_data(fake.path());
        REQUIRE(data != nullptr);
        REQUIRE(data->initial.object_identity == expected_candidate_identity);

        switch (state_->activation_outcome) {
        case ports::ArtifactCacheActivationOutcome::activated:
            filesystem_.move_file(fake.path(), state_->target_path);
            break;
        case ports::ArtifactCacheActivationOutcome::target_occupied:
            if (state_->occupied_winner_bytes.has_value()) {
                filesystem_.add_file(
                    state_->target_path, *state_->occupied_winner_bytes);
            }
            break;
        case ports::ArtifactCacheActivationOutcome::activated_durability_unproven:
            filesystem_.move_file(fake.path(), state_->target_path);
            break;
        case ports::ArtifactCacheActivationOutcome::indeterminate:
            break;
        }
        return ports::ArtifactCacheActivationResult{
            state_->activation_outcome,
            std::nullopt,
        };
    }

  private:
    FakeFilesystemIdentity& filesystem_;
    std::shared_ptr<CacheState> state_;
};

class FakeCachePlatform final : public ports::ArtifactCachePlatform {
  public:
    FakeCachePlatform(
        FakeFilesystemIdentity& filesystem,
        std::shared_ptr<CacheState> state)
        : filesystem_(filesystem), state_(std::move(state)) {}

  protected:
    [[nodiscard]] std::unique_ptr<ports::ArtifactCacheLease>
    acquire_digest_lease_validated(
        const fs::path&,
        std::string_view,
        const std::chrono::steady_clock::time_point deadline) override {
        ++state_->lease_calls;
        state_->captured_lease_deadline = deadline;
        return std::make_unique<FakeLease>(filesystem_, state_);
    }

  private:
    FakeFilesystemIdentity& filesystem_;
    std::shared_ptr<CacheState> state_;
};

[[nodiscard]] fs::path fake_root(const std::string_view name) {
    return (fs::absolute(fs::temp_directory_path()) /
            "arena-acq-01" / std::string{name})
        .lexically_normal();
}

[[nodiscard]] std::shared_ptr<CacheState> cache_state() {
    auto state = std::make_shared<CacheState>();
    const auto cache = fake_root("cache");
    state->target_path = cache / "artifacts" / "objects" / "sha256" /
                         "ba" / std::string{kAbcSha256};
    return state;
}

[[nodiscard]] app::ArtifactAcquisitionRequest request_for(
    std::vector<std::string> sources,
    const bool offline = true) {
    return app::ArtifactAcquisitionRequest{
        std::move(sources),
        std::string{kAbcSha256},
        3,
        fake_root("catalog"),
        fake_root("cache"),
        offline,
        std::stop_token{},
        {},
    };
}

void add_local_file(
    FakeFilesystemIdentity& filesystem,
    const app::ArtifactAcquisitionRequest& request,
    const std::string_view leaf,
    const std::string_view bytes) {
    filesystem.add_directory(request.catalog_directory);
    filesystem.add_file(
        request.catalog_directory / std::string{leaf}, bytes);
}

[[nodiscard]] bool lower_hex_32(const std::string_view value) {
    return value.size() == 32U &&
           std::ranges::all_of(value, [](const char c) {
               return (c >= '0' && c <= '9') ||
                      (c >= 'a' && c <= 'f');
           });
}

static_assert(!std::is_copy_constructible_v<app::AcquiredArtifact>);
static_assert(!std::is_copy_assignable_v<app::AcquiredArtifact>);
static_assert(std::is_nothrow_move_constructible_v<app::AcquiredArtifact>);
static_assert(std::is_nothrow_move_assignable_v<app::AcquiredArtifact>);

} // namespace

TEST_CASE("ArtifactCachePlatform validates cache root digest and timeout") {
    FakeFilesystemIdentity filesystem;
    const auto state = cache_state();
    FakeCachePlatform platform(filesystem, state);

    REQUIRE_THROWS_AS(
        platform.acquire_digest_lease(
            fs::path{"relative"}, kAbcSha256, std::chrono::seconds{1}),
        std::runtime_error);
    REQUIRE_THROWS_AS(
        platform.acquire_digest_lease(
            fake_root("cache"), std::string(64, 'A'),
            std::chrono::seconds{1}),
        std::runtime_error);
    REQUIRE_THROWS_AS(
        platform.acquire_digest_lease(
            fake_root("cache"), kAbcSha256,
            std::chrono::milliseconds{0}),
        std::runtime_error);
    REQUIRE_THROWS_AS(
        platform.acquire_digest_lease(
            fake_root("cache"), kAbcSha256,
            std::chrono::seconds{121}),
        std::runtime_error);
    REQUIRE(state->lease_calls == 0);

    const auto before = std::chrono::steady_clock::now();
    auto lease = platform.acquire_digest_lease(
        fake_root("cache"), kAbcSha256, std::chrono::milliseconds{250});
    REQUIRE(lease != nullptr);
    REQUIRE(state->lease_calls == 1);
    REQUIRE(state->captured_lease_deadline > before);
    REQUIRE(state->captured_lease_deadline <=
            before + std::chrono::milliseconds{300});
}

TEST_CASE("ArtifactAcquirer validates request and source grammar before lease") {
    const std::vector<std::string> invalid_sources{
        "",
        "/absolute.bin",
        "C:/drive.bin",
        "file:artifact.bin",
        "ftp:artifact.bin",
        "http://example.com/a.bin",
        "dir//artifact.bin",
        "dir/./artifact.bin",
        "dir/../artifact.bin",
        "dir\\artifact.bin",
        std::string{"bad\nname.bin"},
        "https://user@example.com/a.bin",
        "https://example.com/a.bin#fragment",
        "https:///missing-host",
        "https://example.com:abc/a.bin",
        "https://example.com:65536/a.bin",
    };

    for (const auto& source : invalid_sources) {
        FakeFilesystemIdentity filesystem;
        const auto state = cache_state();
        FakeCachePlatform platform(filesystem, state);
        app::ArtifactAcquirer acquirer(filesystem, platform);
        auto request = request_for({source});
        REQUIRE_THROWS_AS(acquirer.acquire(request), std::runtime_error);
        REQUIRE(state->lease_calls == 0);
    }

    {
        FakeFilesystemIdentity filesystem;
        const auto state = cache_state();
        FakeCachePlatform platform(filesystem, state);
        app::ArtifactAcquirer acquirer(filesystem, platform);
        auto request = request_for({});
        REQUIRE_THROWS_AS(acquirer.acquire(request), std::runtime_error);
        REQUIRE(state->lease_calls == 0);
    }
    {
        FakeFilesystemIdentity filesystem;
        const auto state = cache_state();
        FakeCachePlatform platform(filesystem, state);
        app::ArtifactAcquirer acquirer(filesystem, platform);
        std::vector<std::string> sources(17, "artifact.bin");
        auto request = request_for(std::move(sources));
        REQUIRE_THROWS_AS(acquirer.acquire(request), std::runtime_error);
        REQUIRE(state->lease_calls == 0);
    }
    {
        FakeFilesystemIdentity filesystem;
        const auto state = cache_state();
        FakeCachePlatform platform(filesystem, state);
        app::ArtifactAcquirer acquirer(filesystem, platform);
        auto request = request_for({std::string(4097, 'x')});
        REQUIRE_THROWS_AS(acquirer.acquire(request), std::runtime_error);
        REQUIRE(state->lease_calls == 0);
    }
    {
        FakeFilesystemIdentity filesystem;
        const auto state = cache_state();
        FakeCachePlatform platform(filesystem, state);
        app::ArtifactAcquirer acquirer(filesystem, platform);
        std::string invalid_utf8{"artifact-"};
        invalid_utf8.push_back(static_cast<char>(0xc3));
        invalid_utf8.push_back(static_cast<char>(0x28));
        auto request = request_for({invalid_utf8});
        REQUIRE_THROWS_AS(acquirer.acquire(request), std::runtime_error);
        REQUIRE(state->lease_calls == 0);
    }
    {
        FakeFilesystemIdentity filesystem;
        const auto state = cache_state();
        FakeCachePlatform platform(filesystem, state);
        app::ArtifactAcquirer acquirer(filesystem, platform);
        auto request = request_for({"artifact.bin"});
        request.expected_sha256 = std::string(64, 'A');
        REQUIRE_THROWS_AS(acquirer.acquire(request), std::runtime_error);
        request.expected_sha256 = std::string{kAbcSha256};
        request.expected_size_bytes = 0;
        REQUIRE_THROWS_AS(acquirer.acquire(request), std::runtime_error);
        REQUIRE(state->lease_calls == 0);
    }
    {
        FakeFilesystemIdentity filesystem;
        const auto state = cache_state();
        FakeCachePlatform platform(filesystem, state);
        app::ArtifactAcquirer acquirer(filesystem, platform);
        auto request = request_for({"artifact.bin"});
        request.catalog_directory = fs::path{"relative"};
        REQUIRE_THROWS_AS(acquirer.acquire(request), std::runtime_error);
        request.catalog_directory = fake_root("catalog");
        request.cache_root = request.cache_root.root_path();
        REQUIRE_THROWS_AS(acquirer.acquire(request), std::runtime_error);
        REQUIRE(state->lease_calls == 0);
    }
    {
        FakeFilesystemIdentity filesystem;
        const auto state = cache_state();
        FakeCachePlatform platform(filesystem, state);
        app::ArtifactAcquirer acquirer(filesystem, platform);
        auto request = request_for({"artifact.bin"});
        request.policy.cache_lock_timeout = std::chrono::milliseconds{0};
        REQUIRE_THROWS_AS(acquirer.acquire(request), std::runtime_error);
        request.policy.cache_lock_timeout = std::chrono::seconds{121};
        REQUIRE_THROWS_AS(acquirer.acquire(request), std::runtime_error);
        request.policy.cache_lock_timeout = std::chrono::seconds{30};
        request.policy.connect_timeout = std::chrono::milliseconds{0};
        REQUIRE_THROWS_AS(acquirer.acquire(request), std::runtime_error);
        request.policy.connect_timeout = std::chrono::seconds{121};
        REQUIRE_THROWS_AS(acquirer.acquire(request), std::runtime_error);
        request.policy.connect_timeout = std::chrono::seconds{15};
        request.policy.overall_timeout = std::chrono::milliseconds{999};
        REQUIRE_THROWS_AS(acquirer.acquire(request), std::runtime_error);
        request.policy.overall_timeout = std::chrono::hours{168} +
                                         std::chrono::milliseconds{1};
        REQUIRE_THROWS_AS(acquirer.acquire(request), std::runtime_error);
        REQUIRE(state->lease_calls == 0);
    }
}

TEST_CASE("ArtifactAcquirer accepts normalized HTTPS grammar without networking when offline") {
    FakeFilesystemIdentity filesystem;
    const auto state = cache_state();
    FakeCachePlatform platform(filesystem, state);
    app::ArtifactAcquirer acquirer(filesystem, platform);
    auto request = request_for(
        {"HTTPS://example.com:443/artifact.bin?token=opaque", "good.bin"});
    add_local_file(filesystem, request, "good.bin", "abc");
    REQUIRE_NOTHROW(acquirer.acquire(request));
    REQUIRE(state->candidate_creates == 1);
}

TEST_CASE("ArtifactAcquirer clips lease wait to one overall deadline") {
    FakeFilesystemIdentity filesystem;
    const auto state = cache_state();
    FakeCachePlatform platform(filesystem, state);
    app::ArtifactAcquirer acquirer(filesystem, platform);
    auto request = request_for({"missing.bin"});
    request.policy.cache_lock_timeout = std::chrono::seconds{120};
    request.policy.overall_timeout = std::chrono::seconds{1};

    const auto before = std::chrono::steady_clock::now();
    REQUIRE_THROWS_AS(acquirer.acquire(request), std::runtime_error);
    REQUIRE(state->lease_calls == 1);
    REQUIRE(state->captured_lease_deadline > before);
    REQUIRE(state->captured_lease_deadline <=
            before + std::chrono::milliseconds{1100});
}

TEST_CASE("ArtifactAcquirer cancellation is terminal before and during source") {
    SECTION("before lease") {
        FakeFilesystemIdentity filesystem;
        const auto state = cache_state();
        FakeCachePlatform platform(filesystem, state);
        app::ArtifactAcquirer acquirer(filesystem, platform);
        std::stop_source stop;
        stop.request_stop();
        auto request = request_for({"good.bin"});
        request.stop_token = stop.get_token();
        REQUIRE_THROWS_AS(acquirer.acquire(request), std::runtime_error);
        REQUIRE(state->lease_calls == 0);
    }

    SECTION("during source") {
        FakeFilesystemIdentity filesystem;
        const auto state = cache_state();
        FakeCachePlatform platform(filesystem, state);
        app::ArtifactAcquirer acquirer(filesystem, platform);
        std::stop_source stop;
        state->stop_on_candidate_write = true;
        state->stop_source = &stop;
        auto request = request_for({"good.bin"});
        request.stop_token = stop.get_token();
        add_local_file(filesystem, request, "good.bin", "abc");
        REQUIRE_THROWS_AS(acquirer.acquire(request), std::runtime_error);
        REQUIRE(state->candidate_creates == 1);
        REQUIRE(state->candidate_cleanups == 1);
        REQUIRE(state->activate_calls == 0);
    }
}

TEST_CASE("ArtifactAcquirer valid cache hit bypasses all sources") {
    FakeFilesystemIdentity filesystem;
    const auto state = cache_state();
    filesystem.add_file(state->target_path, "abc");
    FakeCachePlatform platform(filesystem, state);
    app::ArtifactAcquirer acquirer(filesystem, platform);

    const auto artifact = acquirer.acquire(request_for({"missing.bin"}));
    REQUIRE(artifact.cache_path == state->target_path);
    REQUIRE(artifact.size_bytes == 3);
    REQUIRE(artifact.sha256 == kAbcSha256);
    REQUIRE(artifact.file != nullptr);
    REQUIRE(state->candidate_creates == 0);
    REQUIRE(state->discard_calls == 0);
}

TEST_CASE("ArtifactAcquirer discards only exact corrupt target") {
    FakeFilesystemIdentity filesystem;
    const auto state = cache_state();
    filesystem.add_file(state->target_path, "abd");
    FakeCachePlatform platform(filesystem, state);
    app::ArtifactAcquirer acquirer(filesystem, platform);
    auto request = request_for({"good.bin"});
    add_local_file(filesystem, request, "good.bin", "abc");

    const auto artifact = acquirer.acquire(request);
    REQUIRE(artifact.file != nullptr);
    REQUIRE(state->discard_calls == 1);
    REQUIRE(state->quarantine_ids.size() == 1);
    REQUIRE(lower_hex_32(state->quarantine_ids.front()));
}

TEST_CASE("ArtifactAcquirer preserves source ordering and cleans failed candidates") {
    FakeFilesystemIdentity filesystem;
    const auto state = cache_state();
    FakeCachePlatform platform(filesystem, state);
    app::ArtifactAcquirer acquirer(filesystem, platform);
    auto request = request_for({"bad.bin", "good.bin"});
    filesystem.add_directory(request.catalog_directory);
    filesystem.add_file(request.catalog_directory / "bad.bin", "abd");
    filesystem.add_file(request.catalog_directory / "good.bin", "abc");

    const auto artifact = acquirer.acquire(request);
    REQUIRE(artifact.file != nullptr);
    REQUIRE(state->candidate_creates == 2);
    REQUIRE(state->candidate_cleanups == 1);
    REQUIRE(state->candidate_ids.size() == 2);
    REQUIRE(lower_hex_32(state->candidate_ids[0]));
    REQUIRE(lower_hex_32(state->candidate_ids[1]));
    REQUIRE(state->candidate_ids[0] != state->candidate_ids[1]);
    REQUIRE(state->target_path.string().find(".partial") == std::string::npos);
}

TEST_CASE("ArtifactAcquirer offline mode skips HTTPS without network") {
    FakeFilesystemIdentity filesystem;
    const auto state = cache_state();
    FakeCachePlatform platform(filesystem, state);
    app::ArtifactAcquirer acquirer(filesystem, platform);
    auto request = request_for(
        {"https://example.com/artifact.bin?token=fixture-secret", "good.bin"});
    add_local_file(filesystem, request, "good.bin", "abc");

    const auto artifact = acquirer.acquire(request);
    REQUIRE(artifact.file != nullptr);
    REQUIRE(state->candidate_creates == 1);

    FakeFilesystemIdentity offline_filesystem;
    const auto offline_state = cache_state();
    FakeCachePlatform offline_platform(offline_filesystem, offline_state);
    app::ArtifactAcquirer offline_acquirer(
        offline_filesystem, offline_platform);
    const auto message = [&] {
        try {
            (void)offline_acquirer.acquire(request_for(
                {"https://example.com/artifact.bin?token=fixture-secret"}));
        } catch (const std::runtime_error& error) {
            return std::string{error.what()};
        }
        FAIL("offline HTTPS-only request unexpectedly succeeded");
        return std::string{};
    }();
    REQUIRE(message.find("offline_unavailable") != std::string::npos);
    REQUIRE(message.find("fixture-secret") == std::string::npos);
    REQUIRE(offline_state->candidate_creates == 0);
}

TEST_CASE("ArtifactAcquirer local source enforces size digest mutation and rebinding") {
    SECTION("size") {
        FakeFilesystemIdentity filesystem;
        const auto state = cache_state();
        FakeCachePlatform platform(filesystem, state);
        app::ArtifactAcquirer acquirer(filesystem, platform);
        auto request = request_for({"source.bin"});
        add_local_file(filesystem, request, "source.bin", "abcd");
        REQUIRE_THROWS_AS(acquirer.acquire(request), std::runtime_error);
        REQUIRE(state->candidate_cleanups == 1);
        REQUIRE(state->activate_calls == 0);
    }

    SECTION("digest") {
        FakeFilesystemIdentity filesystem;
        const auto state = cache_state();
        FakeCachePlatform platform(filesystem, state);
        app::ArtifactAcquirer acquirer(filesystem, platform);
        auto request = request_for({"source.bin"});
        add_local_file(filesystem, request, "source.bin", "abd");
        REQUIRE_THROWS_AS(acquirer.acquire(request), std::runtime_error);
        REQUIRE(state->candidate_cleanups == 1);
        REQUIRE(state->activate_calls == 0);
    }

    SECTION("metadata mutation") {
        FakeFilesystemIdentity filesystem;
        const auto state = cache_state();
        FakeCachePlatform platform(filesystem, state);
        app::ArtifactAcquirer acquirer(filesystem, platform);
        auto request = request_for({"source.bin"});
        filesystem.add_directory(request.catalog_directory);
        const auto data = filesystem.add_file(
            request.catalog_directory / "source.bin", "abc");
        data->current.metadata_identity[0] = std::byte{99};
        REQUIRE_THROWS_AS(acquirer.acquire(request), std::runtime_error);
        REQUIRE(state->candidate_cleanups == 1);
        REQUIRE(state->activate_calls == 0);
    }

    SECTION("rebinding") {
        FakeFilesystemIdentity filesystem;
        const auto state = cache_state();
        FakeCachePlatform platform(filesystem, state);
        app::ArtifactAcquirer acquirer(filesystem, platform);
        auto request = request_for({"source.bin"});
        filesystem.add_directory(request.catalog_directory);
        const auto data = filesystem.add_file(
            request.catalog_directory / "source.bin", "abc");
        data->forced_binding = ports::PathBindingState::different_object;
        REQUIRE_THROWS_AS(acquirer.acquire(request), std::runtime_error);
        REQUIRE(state->candidate_cleanups == 1);
        REQUIRE(state->activate_calls == 0);
    }
}

TEST_CASE("ArtifactAcquirer independently verifies finished candidate") {
    FakeFilesystemIdentity filesystem;
    const auto state = cache_state();
    state->corrupt_finished_candidate = true;
    FakeCachePlatform platform(filesystem, state);
    app::ArtifactAcquirer acquirer(filesystem, platform);
    auto request = request_for({"good.bin"});
    add_local_file(filesystem, request, "good.bin", "abc");

    REQUIRE_THROWS_AS(acquirer.acquire(request), std::runtime_error);
    REQUIRE(state->activate_calls == 0);
}

TEST_CASE("ArtifactAcquirer handles every cache activation outcome") {
    SECTION("activated") {
        FakeFilesystemIdentity filesystem;
        const auto state = cache_state();
        FakeCachePlatform platform(filesystem, state);
        app::ArtifactAcquirer acquirer(filesystem, platform);
        auto request = request_for({"good.bin"});
        add_local_file(filesystem, request, "good.bin", "abc");
        const auto artifact = acquirer.acquire(request);
        REQUIRE(artifact.file != nullptr);
        REQUIRE(state->activate_calls == 1);
    }

    SECTION("target occupied by exact winner") {
        FakeFilesystemIdentity filesystem;
        const auto state = cache_state();
        state->activation_outcome =
            ports::ArtifactCacheActivationOutcome::target_occupied;
        state->occupied_winner_bytes = "abc";
        FakeCachePlatform platform(filesystem, state);
        app::ArtifactAcquirer acquirer(filesystem, platform);
        auto request = request_for({"good.bin"});
        add_local_file(filesystem, request, "good.bin", "abc");
        const auto artifact = acquirer.acquire(request);
        REQUIRE(artifact.file != nullptr);
        REQUIRE(state->candidate_cleanups == 1);
    }

    SECTION("target occupied by invalid winner") {
        FakeFilesystemIdentity filesystem;
        const auto state = cache_state();
        state->activation_outcome =
            ports::ArtifactCacheActivationOutcome::target_occupied;
        state->occupied_winner_bytes = "abd";
        FakeCachePlatform platform(filesystem, state);
        app::ArtifactAcquirer acquirer(filesystem, platform);
        auto request = request_for({"good.bin"});
        add_local_file(filesystem, request, "good.bin", "abc");
        REQUIRE_THROWS_AS(acquirer.acquire(request), std::runtime_error);
        REQUIRE(state->candidate_cleanups == 1);
    }

    SECTION("durability unproven is terminal") {
        FakeFilesystemIdentity filesystem;
        const auto state = cache_state();
        state->activation_outcome =
            ports::ArtifactCacheActivationOutcome::activated_durability_unproven;
        FakeCachePlatform platform(filesystem, state);
        app::ArtifactAcquirer acquirer(filesystem, platform);
        auto request = request_for({"good.bin"});
        add_local_file(filesystem, request, "good.bin", "abc");
        REQUIRE_THROWS_AS(acquirer.acquire(request), std::runtime_error);
        REQUIRE(state->activate_calls == 1);
        REQUIRE(state->candidate_cleanups == 0);
    }

    SECTION("indeterminate is terminal") {
        FakeFilesystemIdentity filesystem;
        const auto state = cache_state();
        state->activation_outcome =
            ports::ArtifactCacheActivationOutcome::indeterminate;
        FakeCachePlatform platform(filesystem, state);
        app::ArtifactAcquirer acquirer(filesystem, platform);
        auto request = request_for({"good.bin"});
        add_local_file(filesystem, request, "good.bin", "abc");
        REQUIRE_THROWS_AS(acquirer.acquire(request), std::runtime_error);
        REQUIRE(state->activate_calls == 1);
    }
}

TEST_CASE("ArtifactAcquirer uses one deadline across fallback attempts") {
    FakeFilesystemIdentity filesystem;
    const auto state = cache_state();
    FakeCachePlatform platform(filesystem, state);
    app::ArtifactAcquirer acquirer(filesystem, platform);
    auto request = request_for({"bad.bin", "good.bin"});
    request.policy.overall_timeout = std::chrono::seconds{1};
    filesystem.add_directory(request.catalog_directory);
    const auto bad = filesystem.add_file(
        request.catalog_directory / "bad.bin", "abd");
    const auto good = filesystem.add_file(
        request.catalog_directory / "good.bin", "abc");
    bad->read_delay = std::chrono::milliseconds{250};
    good->read_delay = std::chrono::milliseconds{600};

    const auto started = std::chrono::steady_clock::now();
    REQUIRE_THROWS_AS(acquirer.acquire(request), std::runtime_error);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    REQUIRE(elapsed < std::chrono::seconds{2});
    REQUIRE(state->candidate_creates == 2);
    REQUIRE(state->candidate_cleanups == 2);
    REQUIRE(state->activate_calls == 0);
}

TEST_CASE("ArtifactAcquirer HTTPS policy seam exposes frozen safeguards") {
    REQUIRE(seam::https_protocols() == "https");
    REQUIRE(seam::https_follow_location() == 1);
    REQUIRE(seam::https_fail_on_error() == 1);
    REQUIRE(seam::https_verify_peer() == 1);
    REQUIRE(seam::https_verify_host() == 2);
    REQUIRE(seam::https_redirect_limit() == 5);
    REQUIRE(seam::https_low_speed_limit() == 1024);
    REQUIRE(seam::https_low_speed_window() == 120);
    REQUIRE(seam::https_connect_timeout(
                std::chrono::seconds{15}, std::chrono::seconds{2}) ==
            std::chrono::seconds{2});
    REQUIRE(seam::https_connect_timeout(
                std::chrono::seconds{1}, std::chrono::seconds{2}) ==
            std::chrono::seconds{1});
    REQUIRE(seam::hard_byte_cap_allows(0, 3, 3));
    REQUIRE(seam::hard_byte_cap_allows(2, 1, 3));
    REQUIRE_FALSE(seam::hard_byte_cap_allows(2, 2, 3));
    REQUIRE_FALSE(seam::hard_byte_cap_allows(4, 0, 3));
}
