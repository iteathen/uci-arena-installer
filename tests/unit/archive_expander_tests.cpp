#include "arena/application/installer/archive_expander.hpp"

#include <archive.h>
#include <archive_entry.h>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace arena;

ports::FilesystemIdentityToken token(const unsigned char value) {
    ports::FilesystemIdentityToken result{};
    result[0] = static_cast<std::byte>(value);
    return result;
}

class FakeFile final : public ports::AdmittedRegularFile {
  public:
    explicit FakeFile(std::vector<std::byte> bytes) : bytes_(std::move(bytes)) {
        observation_ = {ports::FilesystemObjectKind::regular_file, bytes_.size(),
                        token(1), token(2)};
    }
    const std::filesystem::path& absolute_path() const noexcept override { return path_; }
    const ports::FilesystemObservation& initial_observation() const noexcept override {
        return observation_;
    }
    std::size_t read_at(std::uint64_t offset, std::span<std::byte> output) override {
        ++reads;
        if (offset >= bytes_.size()) return 0;
        const auto count = std::min<std::size_t>(output.size(), bytes_.size() - offset);
        std::memcpy(output.data(), bytes_.data() + offset, count);
        return count;
    }
    ports::FilesystemObservation observe_open_object() const override {
        auto result = observation_;
        if (mutate) result.metadata_identity = token(3);
        return result;
    }
    ports::PathBindingState observe_path_binding() const override {
        return rebound ? ports::PathBindingState::different_object
                       : ports::PathBindingState::same_object;
    }
    bool mutate{};
    bool rebound{};
    std::size_t reads{};
  private:
    std::filesystem::path path_{"C:/must-not-be-opened/archive.tar.gz"};
    std::vector<std::byte> bytes_;
    ports::FilesystemObservation observation_{};
};

struct SessionState {
    std::vector<std::string> directories;
    std::map<std::string, std::vector<std::byte>> files;
    bool finished{};
    bool cleaned{};
};

class FakeOutput final : public ports::ArchiveOutputFile {
  public:
    FakeOutput(std::vector<std::byte>& bytes, std::uint64_t expected)
        : bytes_(bytes), expected_(expected) {}
    void write(std::span<const std::byte> bytes) override {
        if (bytes_.size() + bytes.size() > expected_) throw std::runtime_error("overflow");
        bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
    }
    void finish() override {
        if (bytes_.size() != expected_) throw std::runtime_error("short output");
        finished_ = true;
    }
    ~FakeOutput() override = default;
  private:
    std::vector<std::byte>& bytes_;
    std::uint64_t expected_;
    bool finished_{};
};

class FakeSession final : public ports::ArchiveExtractionSession {
  public:
    FakeSession(std::shared_ptr<SessionState> state, bool fold_case)
        : state_(std::move(state)), fold_case_(fold_case) {}
    const std::filesystem::path& root() const noexcept override { return root_; }
    ports::ArchiveTargetPath validate_path(std::string_view path) const override {
        std::string key_text{path};
        if (fold_case_) std::ranges::transform(key_text, key_text.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        ports::ArchivePathCollisionKey key{};
        for (std::size_t i = 0; i < key_text.size(); ++i)
            key[i % key.size()] ^= static_cast<std::byte>(key_text[i]);
        return {std::string{path}, key,
                static_cast<std::size_t>(std::count(path.begin(), path.end(), '/') + 1)};
    }
    void ensure_directory(const ports::ArchiveTargetPath& path) override {
        state_->directories.push_back(path.portable_relative_path);
    }
    std::unique_ptr<ports::ArchiveOutputFile> create_file(
        const ports::ArchiveTargetPath& path, std::uint64_t expected, bool) override {
        return std::make_unique<FakeOutput>(state_->files[path.portable_relative_path], expected);
    }
    void finish() override { state_->finished = true; }
    std::optional<ports::ArchiveExtractionDiagnostic> cleanup() noexcept override {
        state_->cleaned = true;
        return std::nullopt;
    }
  private:
    std::filesystem::path root_{"C:/extract"};
    std::shared_ptr<SessionState> state_;
    bool fold_case_;
};

class FakePlatform final : public ports::ArchiveExtractionPlatform {
  public:
    std::shared_ptr<SessionState> state = std::make_shared<SessionState>();
    bool fold_case{};
  private:
    std::unique_ptr<ports::ArchiveExtractionSession> begin_validated(
        const std::filesystem::path&, ports::ArchiveTargetPlatform,
        std::chrono::steady_clock::time_point) override {
        return std::make_unique<FakeSession>(state, fold_case);
    }
};

struct Entry { std::string path; std::string data; int type{AE_IFREG}; int mode{0644}; };

std::vector<std::byte> make_archive(const std::vector<Entry>& entries,
                                    bool gzip = true) {
    std::vector<char> storage(1024 * 1024);
    std::size_t used = 0;
    archive* writer = archive_write_new();
    REQUIRE(writer != nullptr);
    if (gzip) REQUIRE(archive_write_add_filter_gzip(writer) == ARCHIVE_OK);
    REQUIRE(archive_write_set_format_pax_restricted(writer) == ARCHIVE_OK);
    REQUIRE(archive_write_open_memory(writer, storage.data(), storage.size(), &used) == ARCHIVE_OK);
    for (const auto& item : entries) {
        archive_entry* entry = archive_entry_new();
        archive_entry_set_pathname(entry, item.path.c_str());
        archive_entry_set_filetype(entry, item.type);
        archive_entry_set_perm(entry, item.mode);
        archive_entry_set_size(entry, item.type == AE_IFREG ? item.data.size() : 0);
        REQUIRE(archive_write_header(writer, entry) == ARCHIVE_OK);
        if (!item.data.empty())
            REQUIRE(archive_write_data(writer, item.data.data(), item.data.size()) ==
                    static_cast<la_ssize_t>(item.data.size()));
        archive_entry_free(entry);
    }
    REQUIRE(archive_write_close(writer) == ARCHIVE_OK);
    REQUIRE(archive_write_free(writer) == ARCHIVE_OK);
    std::vector<std::byte> result(used);
    std::memcpy(result.data(), storage.data(), used);
    return result;
}

application::installer::ArchiveExpansionRequest request(std::uint64_t bytes) {
    return {"C:/extract", bytes, ports::ArchiveTargetPlatform::windows_x86_64,
            std::chrono::seconds{30}, {}};
}
} // namespace

TEST_CASE("ArchiveExpander streams gzip tar files and implicit directories") {
    FakeFile file{make_archive({{"a/b.txt", "hello"}})};
    FakePlatform platform;
    application::installer::ArchiveExpander expander{platform};
    const auto result = expander.expand(file, request(5));
    CHECK(result.file_count == 1);
    CHECK(result.directory_count == 2);
    CHECK(result.total_file_bytes == 5);
    CHECK(platform.state->finished);
    CHECK(platform.state->files.at("a/b.txt").size() == 5);
    CHECK(file.reads > 0);
}

TEST_CASE("ArchiveExpander rejects non-gzip tar and cleans the session") {
    FakeFile file{make_archive({{"file", "x"}}, false)};
    FakePlatform platform;
    application::installer::ArchiveExpander expander{platform};
    CHECK_THROWS(expander.expand(file, request(1)));
    CHECK(platform.state->cleaned);
}

TEST_CASE("ArchiveExpander rejects traversal, duplicates, type conflicts, and aliases") {
    for (const auto& entries : std::vector<std::vector<Entry>>{
             {{"../bad", "x"}}, {{"same", "x"}, {"same", "y"}},
             {{"node", "", AE_IFDIR}, {"node", "x"}}}) {
        FakeFile file{make_archive(entries)};
        FakePlatform platform;
        application::installer::ArchiveExpander expander{platform};
        CHECK_THROWS(expander.expand(file, request(entries.size() == 1 ? 1 : 2)));
        CHECK(platform.state->cleaned);
    }
    FakeFile aliases{make_archive({{"A", "x"}, {"a", "y"}})};
    FakePlatform platform;
    platform.fold_case = true;
    application::installer::ArchiveExpander expander{platform};
    CHECK_THROWS(expander.expand(aliases, request(2)));
}

TEST_CASE("ArchiveExpander preserves Linux-like case-distinct paths") {
    FakeFile file{make_archive({{"A", "x"}, {"a", "y"}})};
    FakePlatform platform;
    application::installer::ArchiveExpander expander{platform};
    const auto result = expander.expand(file, request(2));
    CHECK(result.file_count == 2);
}

TEST_CASE("ArchiveExpander rejects forbidden types and permission metadata") {
    for (const auto& entry : std::vector<Entry>{{"link", "", AE_IFLNK},
                                                 {"fifo", "", AE_IFIFO},
                                                 {"special", "x", AE_IFREG, 04755}}) {
        FakeFile file{make_archive({entry})};
        FakePlatform platform;
        application::installer::ArchiveExpander expander{platform};
        CHECK_THROWS(expander.expand(file, request(1)));
    }
}

TEST_CASE("ArchiveExpander enforces exact total and stable admitted input") {
    SECTION("total") {
        FakeFile file{make_archive({{"file", "x"}})};
        FakePlatform platform;
        application::installer::ArchiveExpander expander{platform};
        CHECK_THROWS(expander.expand(file, request(2)));
    }
    SECTION("object mutation") {
        FakeFile file{make_archive({{"file", "x"}})};
        file.mutate = true;
        FakePlatform platform;
        application::installer::ArchiveExpander expander{platform};
        CHECK_THROWS(expander.expand(file, request(1)));
    }
    SECTION("binding mutation") {
        FakeFile file{make_archive({{"file", "x"}})};
        file.rebound = true;
        FakePlatform platform;
        application::installer::ArchiveExpander expander{platform};
        CHECK_THROWS(expander.expand(file, request(1)));
    }
}

TEST_CASE("ArchiveExpander honors cancellation before output") {
    FakeFile file{make_archive({{"file", "x"}})};
    FakePlatform platform;
    application::installer::ArchiveExpander expander{platform};
    std::stop_source stop;
    stop.request_stop();
    auto cancelled = request(1);
    cancelled.stop_token = stop.get_token();
    CHECK_THROWS(expander.expand(file, cancelled));
    CHECK_FALSE(platform.state->finished);
}
