#include "arena/application/installer/exact_document.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace installer = arena::application::installer;
namespace ports = arena::ports;

[[nodiscard]] ports::FilesystemIdentityToken token(const std::byte seed) {
    ports::FilesystemIdentityToken result{};
    result.fill(seed);
    return result;
}

[[nodiscard]] ports::FilesystemObservation file_observation(
    const std::uint64_t size,
    const std::byte object_seed = std::byte{1},
    const std::byte metadata_seed = std::byte{2}) {
    return {
        ports::FilesystemObjectKind::regular_file,
        size,
        token(object_seed),
        token(metadata_seed),
    };
}

[[nodiscard]] ports::FilesystemObservation directory_observation() {
    return {
        ports::FilesystemObjectKind::directory,
        0,
        token(std::byte{7}),
        token(std::byte{8}),
    };
}

[[nodiscard]] fs::path fake_root() {
    return (fs::absolute(fs::temp_directory_path()) / "arena-ed-01")
        .lexically_normal();
}

struct FileTrace {
    std::vector<std::uint64_t> offsets;
    std::vector<std::size_t> request_sizes;
    std::size_t observe_calls{};
    std::size_t binding_calls{};
};

enum class ReadFailure {
    none,
    allocation,
    filesystem,
    unexpected,
};

struct FileScript {
    std::vector<std::byte> bytes;
    ports::FilesystemObservation initial;
    ports::FilesystemObservation final;
    ports::PathBindingState binding{ports::PathBindingState::same_object};
    std::vector<std::size_t> short_reads;
    ReadFailure read_failure{ReadFailure::none};
    bool return_invalid_count{};
    std::shared_ptr<FileTrace> trace{std::make_shared<FileTrace>()};
};

[[nodiscard]] std::vector<std::byte> byte_vector(
    const std::string_view text) {
    std::vector<std::byte> bytes;
    bytes.reserve(text.size());
    for (const unsigned char value : text) {
        bytes.push_back(static_cast<std::byte>(value));
    }
    return bytes;
}

[[nodiscard]] FileScript stable_script(const std::string_view text) {
    FileScript script;
    script.bytes = byte_vector(text);
    script.initial = file_observation(script.bytes.size());
    script.final = script.initial;
    return script;
}

class ScriptedFile final : public ports::AdmittedRegularFile {
  public:
    ScriptedFile(fs::path path, FileScript script)
        : path_(std::move(path)), script_(std::move(script)) {}

    [[nodiscard]] const fs::path& absolute_path() const noexcept override {
        return path_;
    }

    [[nodiscard]] const ports::FilesystemObservation&
    initial_observation() const noexcept override {
        return script_.initial;
    }

    [[nodiscard]] std::size_t read_at(
        const std::uint64_t offset,
        const std::span<std::byte> destination) override {
        script_.trace->offsets.push_back(offset);
        script_.trace->request_sizes.push_back(destination.size());
        switch (script_.read_failure) {
        case ReadFailure::allocation:
            throw std::bad_alloc{};
        case ReadFailure::filesystem:
            throw ports::FilesystemIdentityException(
                ports::FilesystemIdentityErrorCode::access_denied,
                "fixture filesystem failure");
        case ReadFailure::unexpected:
            throw std::runtime_error("fixture-secret-unexpected-read");
        case ReadFailure::none:
            break;
        }
        if (script_.return_invalid_count) return destination.size() + 1;
        if (offset >= script_.bytes.size()) return 0;

        auto count = (std::min)(
            destination.size(),
            script_.bytes.size() - static_cast<std::size_t>(offset));
        if (short_read_index_ < script_.short_reads.size()) {
            count = (std::min)(count,
                               script_.short_reads[short_read_index_++]);
        }
        std::copy_n(script_.bytes.begin() + static_cast<std::size_t>(offset),
                    count, destination.begin());
        return count;
    }

    [[nodiscard]] ports::FilesystemObservation
    observe_open_object() const override {
        ++script_.trace->observe_calls;
        return script_.final;
    }

    [[nodiscard]] ports::PathBindingState
    observe_path_binding() const override {
        ++script_.trace->binding_calls;
        return script_.binding;
    }

  private:
    fs::path path_;
    mutable FileScript script_;
    std::size_t short_read_index_{};
};

class ScriptedDirectory final : public ports::AdmittedDirectory {
  public:
    explicit ScriptedDirectory(FileScript script)
        : ports::AdmittedDirectory(fake_root(), directory_observation()),
          script_(std::move(script)) {}

    std::vector<ports::PathBindingState> bindings{
        ports::PathBindingState::same_object,
        ports::PathBindingState::same_object,
    };
    mutable std::size_t open_calls{};
    mutable std::size_t binding_calls{};

    [[nodiscard]] ports::FilesystemObservation
    observe_open_object() const override {
        return initial_observation();
    }

    [[nodiscard]] ports::PathBindingState
    observe_path_binding() const override {
        const auto index = (std::min)(binding_calls, bindings.size() - 1);
        ++binding_calls;
        return bindings[index];
    }

  protected:
    [[nodiscard]] std::unique_ptr<ports::AdmittedRegularFile>
    open_regular_file_validated(const std::string_view leaf) const override {
        ++open_calls;
        if (!script_.has_value()) {
            throw std::runtime_error("fixture opened more than once");
        }
        auto result = std::make_unique<ScriptedFile>(
            absolute_path() / std::string{leaf}, std::move(*script_));
        script_.reset();
        return result;
    }

    [[nodiscard]] std::unique_ptr<ports::AdmittedDirectory>
    open_directory_validated(std::string_view) const override {
        return nullptr;
    }

  private:
    mutable std::optional<FileScript> script_;
};

template <typename Action>
[[nodiscard]] std::string runtime_failure(Action&& action) {
    try {
        std::forward<Action>(action)();
    } catch (const std::runtime_error& error) {
        return error.what();
    }
    throw std::logic_error("expected runtime failure did not occur");
}

[[nodiscard]] installer::ExactDocument json_document(
    const std::string_view text) {
    installer::ExactDocument document;
    document.bytes = byte_vector(text);
    return document;
}

template <typename T>
concept HasPathReopen = requires(T& value) {
    value.reopen_path();
};

static_assert(!HasPathReopen<ScriptedFile>);
static_assert(std::is_same_v<decltype(FileTrace::offsets)::value_type,
                             std::uint64_t>);

} // namespace

TEST_CASE("ExactDocumentReader accepts stable empty and nonempty documents") {
    installer::ExactDocumentReader reader;

    auto empty_script = stable_script("");
    const auto empty_trace = empty_script.trace;
    ScriptedDirectory empty_directory(std::move(empty_script));
    const auto empty = reader.read_child(
        empty_directory, "empty.json", {8, false});
    REQUIRE(empty.bytes.empty());
    REQUIRE(empty.sha256 ==
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    REQUIRE(empty.initial_observation == empty.final_observation);
    REQUIRE(empty_trace->offsets == std::vector<std::uint64_t>{0});

    auto content_script = stable_script("abc");
    const auto content_trace = content_script.trace;
    ScriptedDirectory content_directory(std::move(content_script));
    const auto content = reader.read_child(
        content_directory, "content.json", {8, true});
    REQUIRE(content.bytes == byte_vector("abc"));
    REQUIRE(content.sha256 ==
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    REQUIRE(content_trace->observe_calls == 1);
    REQUIRE(content_trace->binding_calls == 1);
    REQUIRE(content_directory.binding_calls == 2);
    REQUIRE(content_directory.open_calls == 1);
}

TEST_CASE("ExactDocumentReader enforces byte limits and probes exact maximum") {
    installer::ExactDocumentReader reader;

    auto exact_script = stable_script("12345678");
    const auto exact_trace = exact_script.trace;
    ScriptedDirectory exact_directory(std::move(exact_script));
    const auto exact = reader.read_child(
        exact_directory, "exact.bin", {8, true});
    REQUIRE(exact.bytes == byte_vector("12345678"));
    REQUIRE(exact_trace->offsets ==
            std::vector<std::uint64_t>{0, 8});
    REQUIRE(exact_trace->request_sizes ==
            std::vector<std::size_t>{8, 1});

    auto overflow_script = stable_script("12345");
    overflow_script.initial = file_observation(4);
    const auto overflow_trace = overflow_script.trace;
    ScriptedDirectory overflow_directory(std::move(overflow_script));
    REQUIRE_NOTHROW(runtime_failure([&] {
        (void)reader.read_child(overflow_directory, "overflow.bin", {4, true});
    }));
    REQUIRE(overflow_trace->offsets ==
            std::vector<std::uint64_t>{0, 4});
    REQUIRE(overflow_trace->request_sizes ==
            std::vector<std::size_t>{4, 1});

    auto initial_script = stable_script("12345");
    const auto initial_trace = initial_script.trace;
    ScriptedDirectory initial_directory(std::move(initial_script));
    REQUIRE_NOTHROW(runtime_failure([&] {
        (void)reader.read_child(initial_directory, "initial.bin", {4, true});
    }));
    REQUIRE(initial_trace->offsets.empty());
}

TEST_CASE("ExactDocumentReader accumulates short reads at exact offsets") {
    auto script = stable_script("abcdef");
    script.short_reads = {1, 2, 1, 2};
    const auto trace = script.trace;
    ScriptedDirectory directory(std::move(script));
    installer::ExactDocumentReader reader;

    const auto document = reader.read_child(
        directory, "short.bin", {10, true});

    REQUIRE(document.bytes == byte_vector("abcdef"));
    REQUIRE(trace->offsets ==
            std::vector<std::uint64_t>{0, 1, 3, 4, 6});
    REQUIRE(trace->request_sizes ==
            std::vector<std::size_t>{10, 9, 7, 6, 4});
}

TEST_CASE("ExactDocumentReader rejects unstable observations and bindings") {
    installer::ExactDocumentReader reader;

    auto truncation = stable_script("1234");
    truncation.initial = file_observation(5);
    truncation.final = truncation.initial;
    ScriptedDirectory truncated_directory(std::move(truncation));
    REQUIRE_NOTHROW(runtime_failure([&] {
        (void)reader.read_child(truncated_directory, "truncated", {8, true});
    }));

    auto growth = stable_script("12345");
    growth.initial = file_observation(4);
    growth.final = file_observation(5, std::byte{1}, std::byte{3});
    ScriptedDirectory grown_directory(std::move(growth));
    REQUIRE_NOTHROW(runtime_failure([&] {
        (void)reader.read_child(grown_directory, "grown", {8, true});
    }));

    auto metadata = stable_script("same");
    metadata.final.metadata_identity = token(std::byte{9});
    ScriptedDirectory metadata_directory(std::move(metadata));
    REQUIRE_NOTHROW(runtime_failure([&] {
        (void)reader.read_child(metadata_directory, "metadata", {8, true});
    }));

    auto replacement = stable_script("same");
    replacement.final.object_identity = token(std::byte{9});
    ScriptedDirectory replacement_directory(std::move(replacement));
    REQUIRE_NOTHROW(runtime_failure([&] {
        (void)reader.read_child(replacement_directory, "replacement", {8, true});
    }));

    auto child_binding = stable_script("same");
    child_binding.binding = ports::PathBindingState::different_object;
    ScriptedDirectory child_directory(std::move(child_binding));
    REQUIRE_NOTHROW(runtime_failure([&] {
        (void)reader.read_child(child_directory, "child-binding", {8, true});
    }));

    auto parent_binding = stable_script("same");
    ScriptedDirectory parent_directory(std::move(parent_binding));
    parent_directory.bindings.back() =
        ports::PathBindingState::different_object;
    REQUIRE_NOTHROW(runtime_failure([&] {
        (void)reader.read_child(parent_directory, "parent-binding", {8, true});
    }));
}

TEST_CASE("ExactDocumentReader rejects stale parent and invalid limits before open") {
    installer::ExactDocumentReader reader;

    auto stale_script = stable_script("data");
    ScriptedDirectory stale_directory(std::move(stale_script));
    stale_directory.bindings.front() = ports::PathBindingState::missing;
    REQUIRE_NOTHROW(runtime_failure([&] {
        (void)reader.read_child(stale_directory, "child", {8, true});
    }));
    REQUIRE(stale_directory.open_calls == 0);

    for (const std::uint64_t maximum :
         {UINT64_C(0), UINT64_C(64) * 1024 * 1024 + 1}) {
        auto script = stable_script("data");
        ScriptedDirectory directory(std::move(script));
        REQUIRE_NOTHROW(runtime_failure([&] {
            (void)reader.read_child(directory, "child", {maximum, true});
        }));
        REQUIRE(directory.binding_calls == 0);
        REQUIRE(directory.open_calls == 0);
    }
}

TEST_CASE("ExactDocumentReader rejects empty policy and invalid adapter counts") {
    installer::ExactDocumentReader reader;

    ScriptedDirectory empty_directory(stable_script(""));
    REQUIRE_NOTHROW(runtime_failure([&] {
        (void)reader.read_child(empty_directory, "empty", {8, true});
    }));

    auto invalid_count = stable_script("x");
    invalid_count.return_invalid_count = true;
    ScriptedDirectory invalid_directory(std::move(invalid_count));
    REQUIRE_NOTHROW(runtime_failure([&] {
        (void)reader.read_child(invalid_directory, "invalid", {8, true});
    }));
}

TEST_CASE("ExactDocumentReader preserves owned and allocation failures") {
    installer::ExactDocumentReader reader;

    auto filesystem = stable_script("x");
    filesystem.read_failure = ReadFailure::filesystem;
    ScriptedDirectory filesystem_directory(std::move(filesystem));
    try {
        (void)reader.read_child(filesystem_directory, "owned", {8, true});
        FAIL("owned filesystem failure unexpectedly succeeded");
    } catch (const ports::FilesystemIdentityException& error) {
        REQUIRE(error.code() ==
                ports::FilesystemIdentityErrorCode::access_denied);
    }

    auto allocation = stable_script("x");
    allocation.read_failure = ReadFailure::allocation;
    ScriptedDirectory allocation_directory(std::move(allocation));
    REQUIRE_THROWS_AS(
        reader.read_child(allocation_directory, "allocation", {8, true}),
        std::bad_alloc);

    auto unexpected = stable_script("x");
    unexpected.read_failure = ReadFailure::unexpected;
    ScriptedDirectory unexpected_directory(std::move(unexpected));
    const auto message = runtime_failure([&] {
        (void)reader.read_child(unexpected_directory, "unexpected", {8, true});
    });
    REQUIRE(message.size() <= 512);
    REQUIRE(message.find("fixture-secret") == std::string::npos);
}

TEST_CASE("parse_bounded_json accepts objects arrays and scalar values") {
    const auto value = installer::parse_bounded_json(json_document(
        R"({"array":[null,true,-2,3,"text"],"object":{"value":false}})"),
        {});
    REQUIRE(value.is_object());
    REQUIRE(value.at("array").size() == 5);
    REQUIRE(value.at("array").at(2) == -2);
    REQUIRE(value.at("object").at("value") == false);

    REQUIRE(installer::parse_bounded_json(json_document("null"), {}).is_null());
    REQUIRE(installer::parse_bounded_json(json_document("42"), {}) == 42);
    REQUIRE(installer::parse_bounded_json(json_document(R"("text")"), {}) ==
            "text");
}

TEST_CASE("parse_bounded_json rejects invalid UTF-8 and duplicate keys") {
    auto invalid_utf8 = json_document("");
    invalid_utf8.bytes = {
        std::byte{'"'}, std::byte{0xc0}, std::byte{0x80}, std::byte{'"'}};
    REQUIRE_NOTHROW(runtime_failure([&] {
        (void)installer::parse_bounded_json(invalid_utf8, {});
    }));

    for (const std::string_view text : {
         R"({"key":1,"key":2})",
             R"({"key":{},"key":2})",
             R"({"outer":{"key":1,"key":2}})",
         }) {
        const auto message = runtime_failure([&] {
            (void)installer::parse_bounded_json(json_document(text), {});
        });
        REQUIRE(message.find("duplicate") != std::string::npos);
    }
}

TEST_CASE("parse_bounded_json enforces every structural and text limit") {
    auto limits = installer::BoundedJsonLimits{};
    limits.maximum_depth = 1;
    REQUIRE(installer::parse_bounded_json(json_document("[0]"), limits) ==
            nlohmann::json::array({0}));
    REQUIRE_NOTHROW(runtime_failure([&] {
        (void)installer::parse_bounded_json(json_document("[[0]]"), limits);
    }));

    limits = {};
    limits.maximum_values = 2;
    REQUIRE(installer::parse_bounded_json(json_document("[1]"), limits) ==
            nlohmann::json::array({1}));
    REQUIRE_NOTHROW(runtime_failure([&] {
        (void)installer::parse_bounded_json(json_document("[1,2]"), limits);
    }));

    limits = {};
    limits.maximum_object_members = 1;
    REQUIRE_NOTHROW(runtime_failure([&] {
        (void)installer::parse_bounded_json(
            json_document(R"({"a":1,"b":2})"), limits);
    }));

    limits = {};
    limits.maximum_array_items = 1;
    REQUIRE_NOTHROW(runtime_failure([&] {
        (void)installer::parse_bounded_json(json_document("[1,2]"), limits);
    }));

    limits = {};
    limits.maximum_key_utf8_bytes = 1;
    REQUIRE_NOTHROW(runtime_failure([&] {
        (void)installer::parse_bounded_json(json_document(R"({"ab":1})"),
                                            limits);
    }));

    limits = {};
    limits.maximum_string_utf8_bytes = 1;
    REQUIRE_NOTHROW(runtime_failure([&] {
        (void)installer::parse_bounded_json(json_document(R"("ab")"), limits);
    }));
}

TEST_CASE("parse_bounded_json applies floating-point policy") {
    auto limits = installer::BoundedJsonLimits{};
    REQUIRE_NOTHROW(runtime_failure([&] {
        (void)installer::parse_bounded_json(json_document("1.25"), limits);
    }));

    limits.allow_floating_point = true;
    REQUIRE(installer::parse_bounded_json(json_document("1.25"), limits) ==
            1.25);
    REQUIRE_NOTHROW(runtime_failure([&] {
        (void)installer::parse_bounded_json(json_document("1e10000"), limits);
    }));
}

TEST_CASE("parse_bounded_json rejects malformed trailing and invalid limits") {
    for (const std::string_view text : {
             "",
             "{",
             "[1,]",
             "true false",
             R"({"fixture-secret":)",
         }) {
        const auto message = runtime_failure([&] {
            (void)installer::parse_bounded_json(json_document(text), {});
        });
        REQUIRE(message.size() <= 512);
        REQUIRE(message.find("fixture-secret") == std::string::npos);
    }

    auto limits = installer::BoundedJsonLimits{};
    limits.maximum_values = 0;
    const auto message = runtime_failure([&] {
        (void)installer::parse_bounded_json(json_document("null"), limits);
    });
    REQUIRE(message.find("limits") != std::string::npos);
}
