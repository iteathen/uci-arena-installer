#include "arena/application/installer/archive_expander.hpp"

#include <archive.h>
#include <archive_entry.h>
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
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace arena::application::installer::archive_expander_test_seam {
bool portable_path_is_valid(std::string_view value) noexcept;
bool expansion_ratio_allows(
    std::uint64_t archive_size,
    std::uint64_t expanded_size) noexcept;
std::size_t maximum_path_bytes() noexcept;
std::size_t maximum_leaf_bytes() noexcept;
std::size_t maximum_depth() noexcept;
std::uint64_t maximum_headers() noexcept;
std::uint64_t maximum_path_metadata_bytes() noexcept;
bool entry_metadata_is_allowed(struct archive_entry* entry) noexcept;
bool entry_type_is_allowed(int filetype) noexcept;
bool header_status_continues(int status) noexcept;
bool header_status_is_success_terminal(int status) noexcept;
} // namespace arena::application::installer::archive_expander_test_seam

namespace {

namespace app = arena::application::installer;
namespace ports = arena::ports;
namespace fs = std::filesystem;
namespace seam = arena::application::installer::archive_expander_test_seam;

[[nodiscard]] ports::FilesystemIdentityToken token(const unsigned int seed) {
    ports::FilesystemIdentityToken result{};
    result.fill(static_cast<std::byte>(seed & 0xffU));
    return result;
}

[[nodiscard]] ports::FilesystemObservation file_observation(
    const std::uint64_t size,
    const unsigned int seed = 1U) {
    return ports::FilesystemObservation{
        ports::FilesystemObjectKind::regular_file,
        size,
        token(seed),
        token(seed + 1U),
    };
}

struct ArchiveFileState final {
    std::size_t read_calls{};
    std::vector<std::uint64_t> offsets;
    std::size_t maximum_read{std::numeric_limits<std::size_t>::max()};
    std::chrono::milliseconds read_delay{};
};

class FakeArchiveFile final : public ports::AdmittedRegularFile {
  public:
    explicit FakeArchiveFile(
        std::vector<std::byte> bytes,
        fs::path source_path = fs::path{"Z:/does-not-exist/source.tar.gz"})
        : bytes_(std::move(bytes)),
          path_(std::move(source_path)),
          initial_(file_observation(bytes_.size())),
          final_(initial_),
          state_(std::make_shared<ArchiveFileState>()) {}

    [[nodiscard]] const fs::path& absolute_path() const noexcept override {
        return path_;
    }

    [[nodiscard]] const ports::FilesystemObservation&
    initial_observation() const noexcept override {
        return initial_;
    }

    [[nodiscard]] std::size_t read_at(
        const std::uint64_t offset,
        const std::span<std::byte> destination) override {
        ++state_->read_calls;
        state_->offsets.push_back(offset);
        if (state_->read_delay > std::chrono::milliseconds::zero()) {
            std::this_thread::sleep_for(state_->read_delay);
        }
        if (offset >= bytes_.size() || destination.empty()) {
            return 0;
        }
        const auto available =
            bytes_.size() - static_cast<std::size_t>(offset);
        const auto count = (std::min)(
            {available, destination.size(), state_->maximum_read});
        std::copy_n(
            bytes_.begin() + static_cast<std::ptrdiff_t>(offset),
            count,
            destination.begin());
        return count;
    }

    [[nodiscard]] ports::FilesystemObservation
    observe_open_object() const override {
        return final_;
    }

    [[nodiscard]] ports::PathBindingState
    observe_path_binding() const override {
        return binding_;
    }

    void mutate_metadata() {
        final_.metadata_identity[0] = std::byte{99};
    }

    void set_binding(const ports::PathBindingState binding) {
        binding_ = binding;
    }

    [[nodiscard]] std::shared_ptr<ArchiveFileState> state() const {
        return state_;
    }

  private:
    std::vector<std::byte> bytes_;
    fs::path path_;
    ports::FilesystemObservation initial_;
    ports::FilesystemObservation final_;
    ports::PathBindingState binding_{ports::PathBindingState::same_object};
    std::shared_ptr<ArchiveFileState> state_;
};

enum class FixtureFormat {
    tar,
    zip,
};

enum class FixtureFilter {
    gzip,
    none,
};

struct FixtureEntry final {
    std::string path;
    int filetype{AE_IFREG};
    std::string data;
    int permissions{0644};
    std::optional<std::string> hardlink;
    std::optional<std::string> symlink;
};

class ArchiveWriteHandle final {
  public:
    ArchiveWriteHandle() : value_(archive_write_new()) {
        REQUIRE(value_ != nullptr);
    }

    ~ArchiveWriteHandle() {
        if (value_ != nullptr) {
            (void)archive_write_close(value_);
            (void)archive_write_free(value_);
        }
    }

    [[nodiscard]] struct archive* get() const noexcept {
        return value_;
    }

    void release_after_close() {
        REQUIRE(archive_write_close(value_) == ARCHIVE_OK);
        REQUIRE(archive_write_free(value_) == ARCHIVE_OK);
        value_ = nullptr;
    }

  private:
    struct archive* value_{};
};

[[nodiscard]] std::vector<std::byte> make_archive(
    const std::vector<FixtureEntry>& entries,
    const FixtureFormat format = FixtureFormat::tar,
    const FixtureFilter filter = FixtureFilter::gzip) {
    std::size_t raw_bytes = 0;
    for (const auto& entry : entries) {
        raw_bytes += entry.data.size();
    }
    std::vector<std::byte> storage(
        (std::max)(std::size_t{2U * 1024U * 1024U}, raw_bytes + 512U * 1024U));
    std::size_t used = 0;

    ArchiveWriteHandle writer;
    if (filter == FixtureFilter::gzip) {
        REQUIRE(archive_write_add_filter_gzip(writer.get()) == ARCHIVE_OK);
    }
    if (format == FixtureFormat::tar) {
        REQUIRE(archive_write_set_format_pax_restricted(writer.get()) == ARCHIVE_OK);
    } else {
        REQUIRE(archive_write_set_format_zip(writer.get()) == ARCHIVE_OK);
    }
    REQUIRE(archive_write_open_memory(
                writer.get(), storage.data(), storage.size(), &used) == ARCHIVE_OK);

    for (const auto& spec : entries) {
        auto* entry = archive_entry_new();
        REQUIRE(entry != nullptr);
        archive_entry_set_pathname_utf8(entry, spec.path.c_str());
        archive_entry_set_filetype(entry, spec.filetype);
        archive_entry_set_perm(entry, spec.permissions);
        if (spec.hardlink.has_value()) {
            archive_entry_set_hardlink(entry, spec.hardlink->c_str());
        }
        if (spec.symlink.has_value()) {
            archive_entry_set_symlink(entry, spec.symlink->c_str());
        }
        const bool ordinary_file =
            spec.filetype == AE_IFREG && !spec.hardlink.has_value();
        archive_entry_set_size(
            entry,
            ordinary_file ? static_cast<la_int64_t>(spec.data.size()) : 0);
        REQUIRE(archive_write_header(writer.get(), entry) == ARCHIVE_OK);
        if (ordinary_file && !spec.data.empty()) {
            REQUIRE(archive_write_data(
                        writer.get(), spec.data.data(), spec.data.size()) ==
                    static_cast<la_ssize_t>(spec.data.size()));
        }
        archive_entry_free(entry);
    }

    writer.release_after_close();
    storage.resize(used);
    return storage;
}

[[nodiscard]] std::string lower_ascii(std::string value) {
    for (auto& character : value) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character + ('a' - 'A'));
        }
    }
    return value;
}

struct ExtractionState final {
    fs::path root;
    ports::ArchiveTargetPlatform target{ports::ArchiveTargetPlatform::linux_x86_64};
    std::size_t validate_calls{};
    std::size_t ensure_calls{};
    std::size_t create_calls{};
    std::size_t cleanup_calls{};
    std::size_t session_finish_calls{};
    std::size_t file_finish_calls{};
    std::size_t file_write_calls{};
    std::size_t maximum_write{};
    bool cleanup_failure{};
    bool fail_write{};
    bool fail_session_finish{};
    bool collide_every_path{};
    std::chrono::milliseconds write_delay{};
    std::stop_source* stop_on_write{};
    std::vector<std::string> ensured_directories;
    std::map<std::string, std::vector<std::byte>> files;
    std::map<std::string, bool> executable;
    mutable std::map<std::string, ports::ArchivePathCollisionKey> keys;
    mutable unsigned int next_key{1U};
};

[[nodiscard]] ports::ArchivePathCollisionKey collision_key(
    ExtractionState& state,
    const std::string& portable_path) {
    const auto normalized = state.collide_every_path
                                ? std::string{"forced-collision"}
                                : state.target == ports::ArchiveTargetPlatform::windows_x86_64
                                      ? lower_ascii(portable_path)
                                      : portable_path;
    if (const auto found = state.keys.find(normalized); found != state.keys.end()) {
        return found->second;
    }
    ports::ArchivePathCollisionKey key{};
    key.fill(static_cast<std::byte>(state.next_key++ & 0xffU));
    state.keys.emplace(normalized, key);
    return key;
}

class FakeOutputFile final : public ports::ArchiveOutputFile {
  public:
    FakeOutputFile(
        std::shared_ptr<ExtractionState> state,
        std::string path,
        const std::uint64_t expected_size)
        : state_(std::move(state)),
          path_(std::move(path)),
          expected_size_(expected_size) {}

    void write(const std::span<const std::byte> bytes) override {
        ++state_->file_write_calls;
        state_->maximum_write = (std::max)(state_->maximum_write, bytes.size());
        if (state_->write_delay > std::chrono::milliseconds::zero()) {
            std::this_thread::sleep_for(state_->write_delay);
        }
        if (state_->stop_on_write != nullptr) {
            state_->stop_on_write->request_stop();
        }
        if (state_->fail_write) {
            throw std::runtime_error("fixture-output-secret");
        }
        auto& output = state_->files[path_];
        REQUIRE(output.size() + bytes.size() <= expected_size_);
        output.insert(output.end(), bytes.begin(), bytes.end());
    }

    void finish() override {
        ++state_->file_finish_calls;
        auto& output = state_->files[path_];
        REQUIRE(output.size() == expected_size_);
        REQUIRE_FALSE(finished_);
        finished_ = true;
    }

  private:
    std::shared_ptr<ExtractionState> state_;
    std::string path_;
    std::uint64_t expected_size_{};
    bool finished_{};
};

class FakeSession final : public ports::ArchiveExtractionSession {
  public:
    explicit FakeSession(std::shared_ptr<ExtractionState> state)
        : state_(std::move(state)) {}

    [[nodiscard]] const fs::path& root() const noexcept override {
        return state_->root;
    }

    [[nodiscard]] ports::ArchiveTargetPath validate_path(
        const std::string_view portable_relative_path) const override {
        ++state_->validate_calls;
        std::size_t depth = 1;
        for (const char value : portable_relative_path) {
            if (value == '/') {
                ++depth;
            }
        }
        return ports::ArchiveTargetPath{
            std::string{portable_relative_path},
            collision_key(*state_, std::string{portable_relative_path}),
            depth,
        };
    }

    void ensure_directory(const ports::ArchiveTargetPath& path) override {
        ++state_->ensure_calls;
        state_->ensured_directories.push_back(path.portable_relative_path);
    }

    [[nodiscard]] std::unique_ptr<ports::ArchiveOutputFile> create_file(
        const ports::ArchiveTargetPath& path,
        const std::uint64_t expected_size_bytes,
        const bool executable) override {
        ++state_->create_calls;
        state_->files[path.portable_relative_path] = {};
        state_->executable[path.portable_relative_path] = executable;
        return std::make_unique<FakeOutputFile>(
            state_, path.portable_relative_path, expected_size_bytes);
    }

    void finish() override {
        ++state_->session_finish_calls;
        if (state_->fail_session_finish) {
            throw std::runtime_error("fixture-session-secret");
        }
    }

    [[nodiscard]] std::optional<ports::ArchiveExtractionDiagnostic>
    cleanup() noexcept override {
        ++state_->cleanup_calls;
        if (state_->cleanup_failure) {
            return ports::ArchiveExtractionDiagnostic{
                "cleanup_failed", "fixture-cleanup-secret"};
        }
        return std::nullopt;
    }

  private:
    std::shared_ptr<ExtractionState> state_;
};

class FakePlatform final : public ports::ArchiveExtractionPlatform {
  public:
    explicit FakePlatform(std::shared_ptr<ExtractionState> state)
        : state_(std::move(state)) {}

    [[nodiscard]] std::size_t begin_calls() const noexcept {
        return begin_calls_;
    }

    [[nodiscard]] std::chrono::steady_clock::time_point deadline() const noexcept {
        return deadline_;
    }

  protected:
    [[nodiscard]] std::unique_ptr<ports::ArchiveExtractionSession>
    begin_validated(
        const fs::path& destination_root,
        const ports::ArchiveTargetPlatform target_platform,
        const std::chrono::steady_clock::time_point deadline) override {
        ++begin_calls_;
        deadline_ = deadline;
        state_->root = destination_root;
        state_->target = target_platform;
        return std::make_unique<FakeSession>(state_);
    }

  private:
    std::shared_ptr<ExtractionState> state_;
    std::size_t begin_calls_{};
    std::chrono::steady_clock::time_point deadline_{};
};

[[nodiscard]] fs::path destination() {
    return (fs::absolute(fs::temp_directory_path()) /
            "arena-arc-01" / "expanded")
        .lexically_normal();
}

[[nodiscard]] app::ArchiveExpansionRequest request(
    const std::uint64_t expected,
    const ports::ArchiveTargetPlatform target =
        ports::ArchiveTargetPlatform::linux_x86_64) {
    return app::ArchiveExpansionRequest{
        destination(),
        expected,
        target,
        std::chrono::hours{24},
        {},
    };
}

[[nodiscard]] std::string bytes_as_string(
    const std::vector<std::byte>& bytes) {
    return std::string{
        reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

[[nodiscard]] std::string runtime_message(const auto& callable) {
    try {
        callable();
    } catch (const std::runtime_error& error) {
        return error.what();
    }
    FAIL("expected runtime_error");
    return {};
}

class EntryHandle final {
  public:
    EntryHandle() : entry_(archive_entry_new()) {
        REQUIRE(entry_ != nullptr);
        archive_entry_set_pathname(entry_, "entry");
        archive_entry_set_filetype(entry_, AE_IFREG);
        archive_entry_set_perm(entry_, 0644);
        archive_entry_set_size(entry_, 1);
    }

    ~EntryHandle() {
        archive_entry_free(entry_);
    }

    [[nodiscard]] struct archive_entry* get() const noexcept {
        return entry_;
    }

  private:
    struct archive_entry* entry_{};
};

} // namespace

TEST_CASE("ArchiveExtractionPlatform validates destination target and timeout") {
    auto state = std::make_shared<ExtractionState>();
    FakePlatform platform(state);

    REQUIRE_THROWS_AS(
        platform.begin(
            fs::path{"relative"}, ports::ArchiveTargetPlatform::linux_x86_64,
            std::chrono::seconds{1}),
        std::runtime_error);
    REQUIRE_THROWS_AS(
        platform.begin(
            destination().root_path(), ports::ArchiveTargetPlatform::linux_x86_64,
            std::chrono::seconds{1}),
        std::runtime_error);
    REQUIRE_THROWS_AS(
        platform.begin(
            destination(), ports::ArchiveTargetPlatform::linux_x86_64,
            std::chrono::milliseconds{999}),
        std::runtime_error);
    REQUIRE_THROWS_AS(
        platform.begin(
            destination(), ports::ArchiveTargetPlatform::linux_x86_64,
            std::chrono::hours{168} + std::chrono::milliseconds{1}),
        std::runtime_error);
    REQUIRE_THROWS_AS(
        platform.begin(
            destination(), static_cast<ports::ArchiveTargetPlatform>(99),
            std::chrono::seconds{1}),
        std::runtime_error);
    REQUIRE(platform.begin_calls() == 0);

    const auto before = std::chrono::steady_clock::now();
    auto session = platform.begin(
        destination(), ports::ArchiveTargetPlatform::windows_x86_64,
        std::chrono::milliseconds{1500});
    REQUIRE(session != nullptr);
    REQUIRE(platform.begin_calls() == 1);
    REQUIRE(platform.deadline() > before);
    REQUIRE(platform.deadline() <= before + std::chrono::milliseconds{1600});
}

TEST_CASE("ArchiveExpander accepts tar gzip files directories root and implicit parents") {
    const auto archive = make_archive({
        FixtureEntry{".", AE_IFDIR, {}, 0755},
        FixtureEntry{"explicit", AE_IFDIR, {}, 0755},
        FixtureEntry{"implicit/nested.txt", AE_IFREG, "abc", 0755},
    });
    FakeArchiveFile source(archive);
    source.state()->maximum_read = 7;
    auto state = std::make_shared<ExtractionState>();
    FakePlatform platform(state);
    app::ArchiveExpander expander(platform);

    const auto result = expander.expand(source, request(3));
    REQUIRE(result.root == destination());
    REQUIRE(result.file_count == 1);
    REQUIRE(result.directory_count == 3);
    REQUIRE(result.total_file_bytes == 3);
    REQUIRE(bytes_as_string(state->files["implicit/nested.txt"]) == "abc");
    REQUIRE(state->executable["implicit/nested.txt"]);
    REQUIRE(state->session_finish_calls == 1);
    REQUIRE(state->cleanup_calls == 0);
    REQUIRE(source.absolute_path() == fs::path{"Z:/does-not-exist/source.tar.gz"});
    REQUIRE(source.state()->read_calls > 1);
    REQUIRE_FALSE(source.state()->offsets.empty());
    REQUIRE(source.state()->offsets.front() == 0);
}

TEST_CASE("ArchiveExpander rejects plain tar zip and truncated input") {
    const std::vector<FixtureEntry> entries{
        FixtureEntry{"file.txt", AE_IFREG, "abc", 0644},
    };

    for (auto archive : {
             make_archive(entries, FixtureFormat::tar, FixtureFilter::none),
             make_archive(entries, FixtureFormat::zip, FixtureFilter::none)}) {
        FakeArchiveFile source(std::move(archive));
        auto state = std::make_shared<ExtractionState>();
        FakePlatform platform(state);
        app::ArchiveExpander expander(platform);
        REQUIRE_THROWS_AS(expander.expand(source, request(3)), std::runtime_error);
        REQUIRE(state->cleanup_calls == 1);
        REQUIRE(state->session_finish_calls == 0);
    }

    auto truncated = make_archive(entries);
    REQUIRE(truncated.size() > 16);
    truncated.resize(truncated.size() - 16U);
    FakeArchiveFile source(std::move(truncated));
    auto state = std::make_shared<ExtractionState>();
    FakePlatform platform(state);
    app::ArchiveExpander expander(platform);
    REQUIRE_THROWS_AS(expander.expand(source, request(3)), std::runtime_error);
    REQUIRE(state->cleanup_calls == 1);
}

TEST_CASE("ArchiveExpander header status contract is fail closed") {
    REQUIRE(seam::header_status_continues(ARCHIVE_OK));
    REQUIRE_FALSE(seam::header_status_continues(ARCHIVE_WARN));
    REQUIRE_FALSE(seam::header_status_continues(ARCHIVE_RETRY));
    REQUIRE_FALSE(seam::header_status_continues(ARCHIVE_FAILED));
    REQUIRE_FALSE(seam::header_status_continues(ARCHIVE_FATAL));
    REQUIRE_FALSE(seam::header_status_continues(ARCHIVE_EOF));

    REQUIRE(seam::header_status_is_success_terminal(ARCHIVE_EOF));
    REQUIRE_FALSE(seam::header_status_is_success_terminal(ARCHIVE_OK));
    REQUIRE_FALSE(seam::header_status_is_success_terminal(ARCHIVE_WARN));
    REQUIRE_FALSE(seam::header_status_is_success_terminal(ARCHIVE_FAILED));
    REQUIRE_FALSE(seam::header_status_is_success_terminal(ARCHIVE_FATAL));
}

TEST_CASE("ArchiveExpander validates portable path exact bounds") {
    REQUIRE(seam::maximum_path_bytes() == 131072);
    REQUIRE(seam::maximum_leaf_bytes() == 4096);
    REQUIRE(seam::maximum_depth() == 1024);
    REQUIRE(seam::maximum_headers() == UINT64_C(1048576));
    REQUIRE(seam::maximum_path_metadata_bytes() == UINT64_C(67108864));

    for (const std::string value : {
             "",
             "/rooted",
             "trailing/",
             "a//b",
             "a/./b",
             "a/../b",
             "a\\b",
             "C:/drive",
             std::string{"bad\nname"}}) {
        REQUIRE_FALSE(seam::portable_path_is_valid(value));
    }

    std::string invalid_utf8{"bad"};
    invalid_utf8.push_back(static_cast<char>(0xc3));
    invalid_utf8.push_back(static_cast<char>(0x28));
    REQUIRE_FALSE(seam::portable_path_is_valid(invalid_utf8));

    REQUIRE(seam::portable_path_is_valid("a:b"));
    REQUIRE(seam::portable_path_is_valid(std::string(4096, 'x')));
    REQUIRE_FALSE(seam::portable_path_is_valid(std::string(4097, 'x')));

    std::string depth_exact;
    for (std::size_t index = 0; index < 1024; ++index) {
        if (!depth_exact.empty()) {
            depth_exact.push_back('/');
        }
        depth_exact.push_back('a');
    }
    REQUIRE(seam::portable_path_is_valid(depth_exact));
    REQUIRE_FALSE(seam::portable_path_is_valid(depth_exact + "/a"));

    std::string whole_exact;
    for (std::size_t index = 0; index < 32; ++index) {
        if (!whole_exact.empty()) {
            whole_exact.push_back('/');
        }
        whole_exact.append(index == 0 ? 4096 : 4095, 'x');
    }
    REQUIRE(whole_exact.size() == 131072);
    REQUIRE(seam::portable_path_is_valid(whole_exact));
    whole_exact.push_back('x');
    REQUIRE_FALSE(seam::portable_path_is_valid(whole_exact));
}

TEST_CASE("ArchiveExpander rejects every forbidden entry type and metadata") {
    for (const int filetype : {
             AE_IFLNK, AE_IFCHR, AE_IFBLK, AE_IFIFO, AE_IFSOCK, 0}) {
        REQUIRE_FALSE(seam::entry_type_is_allowed(filetype));
    }
    REQUIRE(seam::entry_type_is_allowed(AE_IFREG));
    REQUIRE(seam::entry_type_is_allowed(AE_IFDIR));

    SECTION("hard link") {
        EntryHandle entry;
        archive_entry_set_hardlink(entry.get(), "target");
        REQUIRE_FALSE(seam::entry_metadata_is_allowed(entry.get()));
    }
    SECTION("symbolic link") {
        EntryHandle entry;
        archive_entry_set_symlink(entry.get(), "target");
        REQUIRE_FALSE(seam::entry_metadata_is_allowed(entry.get()));
    }
    SECTION("sparse") {
        EntryHandle entry;
        archive_entry_sparse_add_entry(entry.get(), 0, 1);
        REQUIRE_FALSE(seam::entry_metadata_is_allowed(entry.get()));
    }
    SECTION("encrypted data") {
        EntryHandle entry;
        archive_entry_set_is_data_encrypted(entry.get(), 1);
        REQUIRE_FALSE(seam::entry_metadata_is_allowed(entry.get()));
    }
    SECTION("encrypted metadata") {
        EntryHandle entry;
        archive_entry_set_is_metadata_encrypted(entry.get(), 1);
        REQUIRE_FALSE(seam::entry_metadata_is_allowed(entry.get()));
    }
    SECTION("xattr") {
        EntryHandle entry;
        const char value = 'x';
        archive_entry_xattr_add_entry(
            entry.get(), "user.test", &value, sizeof(value));
        REQUIRE_FALSE(seam::entry_metadata_is_allowed(entry.get()));
    }
    SECTION("ACL") {
        EntryHandle entry;
        REQUIRE(archive_entry_acl_add_entry(
                    entry.get(),
                    ARCHIVE_ENTRY_ACL_TYPE_ACCESS,
                    ARCHIVE_ENTRY_ACL_READ,
                    ARCHIVE_ENTRY_ACL_USER,
                    1000,
                    "fixture") == ARCHIVE_OK);
        REQUIRE_FALSE(seam::entry_metadata_is_allowed(entry.get()));
    }
    for (const int mode : {04755, 02755, 01755}) {
        EntryHandle entry;
        archive_entry_set_perm(entry.get(), mode);
        REQUIRE_FALSE(seam::entry_metadata_is_allowed(entry.get()));
    }

    EntryHandle ordinary;
    REQUIRE(seam::entry_metadata_is_allowed(ordinary.get()));
}

TEST_CASE("ArchiveExpander rejects real link entries before output creation") {
    for (const auto& entry : {
             FixtureEntry{"link", AE_IFLNK, {}, 0777, std::nullopt, "target"},
             FixtureEntry{"hard", AE_IFREG, {}, 0644, "target", std::nullopt}}) {
        auto archive = make_archive({entry});
        FakeArchiveFile source(std::move(archive));
        auto state = std::make_shared<ExtractionState>();
        FakePlatform platform(state);
        app::ArchiveExpander expander(platform);
        REQUIRE_THROWS_AS(expander.expand(source, request(1)), std::runtime_error);
        REQUIRE(state->create_calls == 0);
        REQUIRE(state->cleanup_calls == 1);
    }
}

TEST_CASE("ArchiveExpander enforces expansion ratio before destination creation") {
    REQUIRE(seam::expansion_ratio_allows(1, UINT64_C(16384)));
    REQUIRE_FALSE(seam::expansion_ratio_allows(1, UINT64_C(16385)));
    REQUIRE(seam::expansion_ratio_allows(
        std::numeric_limits<std::uint64_t>::max(),
        std::numeric_limits<std::uint64_t>::max()));

    FakeArchiveFile source(std::vector<std::byte>{std::byte{'x'}});
    auto state = std::make_shared<ExtractionState>();
    FakePlatform platform(state);
    app::ArchiveExpander expander(platform);
    REQUIRE_THROWS_AS(
        expander.expand(source, request(UINT64_C(16385))),
        std::runtime_error);
    REQUIRE(platform.begin_calls() == 0);
}

TEST_CASE("ArchiveExpander rejects invalid request before output") {
    auto archive = make_archive({FixtureEntry{"a", AE_IFREG, "a", 0644}});

    SECTION("zero expected") {
        FakeArchiveFile source(archive);
        auto state = std::make_shared<ExtractionState>();
        FakePlatform platform(state);
        app::ArchiveExpander expander(platform);
        REQUIRE_THROWS_AS(expander.expand(source, request(0)), std::runtime_error);
        REQUIRE(platform.begin_calls() == 0);
    }
    SECTION("timeout") {
        FakeArchiveFile source(archive);
        auto state = std::make_shared<ExtractionState>();
        FakePlatform platform(state);
        app::ArchiveExpander expander(platform);
        auto value = request(1);
        value.timeout = std::chrono::milliseconds{999};
        REQUIRE_THROWS_AS(expander.expand(source, value), std::runtime_error);
        REQUIRE(platform.begin_calls() == 0);
    }
    SECTION("empty archive capability") {
        FakeArchiveFile source({});
        auto state = std::make_shared<ExtractionState>();
        FakePlatform platform(state);
        app::ArchiveExpander expander(platform);
        REQUIRE_THROWS_AS(expander.expand(source, request(1)), std::runtime_error);
        REQUIRE(platform.begin_calls() == 0);
    }
}

TEST_CASE("ArchiveExpander accepts implicit parent followed by one exact explicit directory") {
    auto archive = make_archive({
        FixtureEntry{"parent/file.txt", AE_IFREG, "x", 0644},
        FixtureEntry{"parent", AE_IFDIR, {}, 0755},
    });
    FakeArchiveFile source(std::move(archive));
    auto state = std::make_shared<ExtractionState>();
    FakePlatform platform(state);
    app::ArchiveExpander expander(platform);

    const auto result = expander.expand(source, request(1));
    REQUIRE(result.file_count == 1);
    REQUIRE(result.directory_count == 2);
    REQUIRE(state->ensure_calls == 1);
}

TEST_CASE("ArchiveExpander rejects duplicate explicit and file directory collisions") {
    const std::vector<std::vector<FixtureEntry>> archives{
        {
            FixtureEntry{"dir", AE_IFDIR, {}, 0755},
            FixtureEntry{"dir", AE_IFDIR, {}, 0755},
        },
        {
            FixtureEntry{"same", AE_IFREG, "x", 0644},
            FixtureEntry{"same", AE_IFDIR, {}, 0755},
        },
        {
            FixtureEntry{"same", AE_IFREG, "x", 0644},
            FixtureEntry{"same", AE_IFREG, "y", 0644},
        },
    };

    for (const auto& entries : archives) {
        FakeArchiveFile source(make_archive(entries));
        auto state = std::make_shared<ExtractionState>();
        FakePlatform platform(state);
        app::ArchiveExpander expander(platform);
        REQUIRE_THROWS_AS(expander.expand(source, request(1)), std::runtime_error);
        REQUIRE(state->cleanup_calls == 1);
    }
}

TEST_CASE("ArchiveExpander applies target collision keys to aliases") {
    const std::vector<FixtureEntry> entries{
        FixtureEntry{"Foo/file.txt", AE_IFREG, "x", 0644},
        FixtureEntry{"foo", AE_IFDIR, {}, 0755},
    };

    SECTION("Windows-like case alias collides") {
        FakeArchiveFile source(make_archive(entries));
        auto state = std::make_shared<ExtractionState>();
        FakePlatform platform(state);
        app::ArchiveExpander expander(platform);
        REQUIRE_THROWS_AS(
            expander.expand(
                source,
                request(1, ports::ArchiveTargetPlatform::windows_x86_64)),
            std::runtime_error);
        REQUIRE(state->cleanup_calls == 1);
    }

    SECTION("Linux-like case distinction remains distinct") {
        FakeArchiveFile source(make_archive(entries));
        auto state = std::make_shared<ExtractionState>();
        FakePlatform platform(state);
        app::ArchiveExpander expander(platform);
        const auto result = expander.expand(
            source,
            request(1, ports::ArchiveTargetPlatform::linux_x86_64));
        REQUIRE(result.directory_count == 3);
    }

    SECTION("opaque key conflict is authoritative") {
        FakeArchiveFile source(make_archive({
            FixtureEntry{"a", AE_IFDIR, {}, 0755},
            FixtureEntry{"b", AE_IFDIR, {}, 0755},
        }));
        auto state = std::make_shared<ExtractionState>();
        state->collide_every_path = true;
        FakePlatform platform(state);
        app::ArchiveExpander expander(platform);
        REQUIRE_THROWS_AS(expander.expand(source, request(1)), std::runtime_error);
    }
}

TEST_CASE("ArchiveExpander requires exact expanded byte total") {
    auto archive = make_archive({FixtureEntry{"file", AE_IFREG, "abc", 0644}});

    for (const std::uint64_t expected : {UINT64_C(2), UINT64_C(4)}) {
        FakeArchiveFile source(archive);
        auto state = std::make_shared<ExtractionState>();
        FakePlatform platform(state);
        app::ArchiveExpander expander(platform);
        REQUIRE_THROWS_AS(expander.expand(source, request(expected)), std::runtime_error);
        REQUIRE(state->cleanup_calls == 1);
        REQUIRE(state->session_finish_calls == 0);
    }
}

TEST_CASE("ArchiveExpander source mutation and rebinding fail after parse") {
    auto archive = make_archive({FixtureEntry{"file", AE_IFREG, "abc", 0644}});

    SECTION("metadata mutation") {
        FakeArchiveFile source(archive);
        source.mutate_metadata();
        auto state = std::make_shared<ExtractionState>();
        FakePlatform platform(state);
        app::ArchiveExpander expander(platform);
        REQUIRE_THROWS_AS(expander.expand(source, request(3)), std::runtime_error);
        REQUIRE(state->cleanup_calls == 1);
        REQUIRE(state->session_finish_calls == 0);
    }

    SECTION("path rebinding") {
        FakeArchiveFile source(archive);
        source.set_binding(ports::PathBindingState::different_object);
        auto state = std::make_shared<ExtractionState>();
        FakePlatform platform(state);
        app::ArchiveExpander expander(platform);
        REQUIRE_THROWS_AS(expander.expand(source, request(3)), std::runtime_error);
        REQUIRE(state->cleanup_calls == 1);
        REQUIRE(state->session_finish_calls == 0);
    }
}

TEST_CASE("ArchiveExpander streams large file through bounded writes") {
    std::string data(300000, 'x');
    auto archive = make_archive({FixtureEntry{"large.bin", AE_IFREG, data, 0644}});
    FakeArchiveFile source(std::move(archive));
    source.state()->maximum_read = 113;
    auto state = std::make_shared<ExtractionState>();
    FakePlatform platform(state);
    app::ArchiveExpander expander(platform);

    const auto result = expander.expand(source, request(data.size()));
    REQUIRE(result.total_file_bytes == data.size());
    REQUIRE(state->file_write_calls > 1);
    REQUIRE(state->maximum_write <= 65536);
    REQUIRE(state->files["large.bin"].size() == data.size());
}

TEST_CASE("ArchiveExpander cancellation and deadline clean the private session") {
    auto archive = make_archive({FixtureEntry{"file", AE_IFREG, "abc", 0644}});

    SECTION("before begin") {
        FakeArchiveFile source(archive);
        auto state = std::make_shared<ExtractionState>();
        FakePlatform platform(state);
        app::ArchiveExpander expander(platform);
        std::stop_source stop;
        stop.request_stop();
        auto value = request(3);
        value.stop_token = stop.get_token();
        REQUIRE_THROWS_AS(expander.expand(source, value), std::runtime_error);
        REQUIRE(platform.begin_calls() == 0);
    }

    SECTION("during output") {
        FakeArchiveFile source(archive);
        auto state = std::make_shared<ExtractionState>();
        FakePlatform platform(state);
        app::ArchiveExpander expander(platform);
        std::stop_source stop;
        state->stop_on_write = &stop;
        auto value = request(3);
        value.stop_token = stop.get_token();
        REQUIRE_THROWS_AS(expander.expand(source, value), std::runtime_error);
        REQUIRE(state->cleanup_calls == 1);
        REQUIRE(state->session_finish_calls == 0);
    }

    SECTION("deadline during output") {
        FakeArchiveFile source(archive);
        auto state = std::make_shared<ExtractionState>();
        state->write_delay = std::chrono::milliseconds{1100};
        FakePlatform platform(state);
        app::ArchiveExpander expander(platform);
        auto value = request(3);
        value.timeout = std::chrono::seconds{1};
        REQUIRE_THROWS_AS(expander.expand(source, value), std::runtime_error);
        REQUIRE(state->cleanup_calls == 1);
    }
}

TEST_CASE("ArchiveExpander preserves primary failure when cleanup also fails") {
    auto archive = make_archive({FixtureEntry{"file", AE_IFREG, "abc", 0644}});
    FakeArchiveFile source(std::move(archive));
    auto state = std::make_shared<ExtractionState>();
    state->fail_write = true;
    state->cleanup_failure = true;
    FakePlatform platform(state);
    app::ArchiveExpander expander(platform);

    const auto message = runtime_message([&] {
        (void)expander.expand(source, request(3));
    });
    REQUIRE(message.find("output write") != std::string::npos);
    REQUIRE(message.find("fixture-output-secret") == std::string::npos);
    REQUIRE(message.find("fixture-cleanup-secret") == std::string::npos);
    REQUIRE(state->cleanup_calls == 1);
}

TEST_CASE("ArchiveExpander target platform is passed through to fake session") {
    auto archive = make_archive({FixtureEntry{"file", AE_IFREG, "x", 0644}});
    FakeArchiveFile source(std::move(archive));
    auto state = std::make_shared<ExtractionState>();
    FakePlatform platform(state);
    app::ArchiveExpander expander(platform);

    REQUIRE_NOTHROW(expander.expand(
        source,
        request(1, ports::ArchiveTargetPlatform::windows_x86_64)));
    REQUIRE(state->target == ports::ArchiveTargetPlatform::windows_x86_64);
}
