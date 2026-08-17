#include "arena/application/installer/exact_document.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace app = arena::application::installer;
namespace ports = arena::ports;
namespace fs = std::filesystem;

[[nodiscard]] ports::FilesystemIdentityToken token(const std::byte seed) {
    ports::FilesystemIdentityToken result{};
    result.fill(seed);
    return result;
}

[[nodiscard]] ports::FilesystemObservation file_observation(
    const std::uint64_t size,
    const std::byte object_seed = std::byte{1},
    const std::byte metadata_seed = std::byte{2}) {
    return ports::FilesystemObservation{
        ports::FilesystemObjectKind::regular_file,
        size,
        token(object_seed),
        token(metadata_seed),
    };
}

[[nodiscard]] ports::FilesystemObservation directory_observation() {
    return ports::FilesystemObservation{
        ports::FilesystemObjectKind::directory,
        0,
        token(std::byte{11}),
        token(std::byte{12}),
    };
}

[[nodiscard]] std::vector<std::byte> bytes_of(
    const std::string_view text) {
    std::vector<std::byte> result;
    result.reserve(text.size());
    for (const unsigned char value : text) {
        result.push_back(static_cast<std::byte>(value));
    }
    return result;
}

struct FileState final {
    std::vector<std::uint64_t> offsets;
    std::size_t read_calls{};
};

struct FilePlan final {
    std::vector<std::byte> bytes;
    ports::FilesystemObservation initial;
    ports::FilesystemObservation final;
    ports::PathBindingState binding{ports::PathBindingState::same_object};
    std::size_t maximum_per_read{std::numeric_limits<std::size_t>::max()};
    bool throw_bad_alloc_on_read{false};
    std::shared_ptr<FileState> state{std::make_shared<FileState>()};
};

[[nodiscard]] FilePlan stable_plan(const std::string_view text) {
    FilePlan plan;
    plan.bytes = bytes_of(text);
    plan.initial = file_observation(plan.bytes.size());
    plan.final = plan.initial;
    return plan;
}

class FakeRegularFile final : public ports::AdmittedRegularFile {
  public:
    FakeRegularFile(fs::path path, FilePlan plan)
        : path_(std::move(path)), plan_(std::move(plan)) {}

    [[nodiscard]] const fs::path& absolute_path() const noexcept override {
        return path_;
    }

    [[nodiscard]] const ports::FilesystemObservation&
    initial_observation() const noexcept override {
        return plan_.initial;
    }

    [[nodiscard]] std::size_t read_at(
        const std::uint64_t offset,
        const std::span<std::byte> destination) override {
        ++plan_.state->read_calls;
        plan_.state->offsets.push_back(offset);
        if (plan_.throw_bad_alloc_on_read) {
            throw std::bad_alloc{};
        }
        if (offset >= plan_.bytes.size() || destination.empty()) {
            return 0;
        }

        const auto available =
            plan_.bytes.size() - static_cast<std::size_t>(offset);
        const auto count =
            (std::min)({available, destination.size(), plan_.maximum_per_read});
        std::copy_n(
            plan_.bytes.begin() + static_cast<std::ptrdiff_t>(offset),
            count,
            destination.begin());
        return count;
    }

    [[nodiscard]] ports::FilesystemObservation
    observe_open_object() const override {
        return plan_.final;
    }

    [[nodiscard]] ports::PathBindingState
    observe_path_binding() const override {
        return plan_.binding;
    }

  private:
    fs::path path_;
    FilePlan plan_;
};

class FakeDirectory final : public ports::AdmittedDirectory {
  public:
    explicit FakeDirectory(
        FilePlan plan,
        const ports::PathBindingState before =
            ports::PathBindingState::same_object,
        const ports::PathBindingState after =
            ports::PathBindingState::same_object)
        : ports::AdmittedDirectory(
              (fs::absolute(fs::temp_directory_path()) / "arena-ed-01")
                  .lexically_normal(),
              directory_observation()),
          plan_(std::move(plan)),
          before_(before),
          after_(after) {}

    [[nodiscard]] ports::FilesystemObservation
    observe_open_object() const override {
        return initial_observation();
    }

    [[nodiscard]] ports::PathBindingState
    observe_path_binding() const override {
        const auto result = binding_calls_ == 0 ? before_ : after_;
        ++binding_calls_;
        return result;
    }

    [[nodiscard]] std::size_t open_calls() const noexcept {
        return open_calls_;
    }

  protected:
    [[nodiscard]] std::unique_ptr<ports::AdmittedRegularFile>
    open_regular_file_validated(const std::string_view leaf) const override {
        ++open_calls_;
        return std::make_unique<FakeRegularFile>(
            absolute_path() / std::string{leaf}, plan_);
    }

    [[nodiscard]] std::unique_ptr<ports::AdmittedDirectory>
    open_directory_validated(std::string_view) const override {
        return nullptr;
    }

  private:
    FilePlan plan_;
    ports::PathBindingState before_;
    ports::PathBindingState after_;
    mutable std::size_t binding_calls_{};
    mutable std::size_t open_calls_{};
};

template <typename T>
concept HasPathReopen = requires(T& value) {
    value.reopen_path();
};

static_assert(!HasPathReopen<ports::AdmittedRegularFile>);

[[nodiscard]] app::ExactDocument document_of(
    const std::string_view text) {
    const auto observation = file_observation(text.size());
    return app::ExactDocument{
        bytes_of(text),
        {},
        observation,
        observation,
    };
}

[[nodiscard]] app::BoundedJsonLimits json_limits() {
    return app::BoundedJsonLimits{
        8,
        100,
        20,
        20,
        32,
        64,
        false,
    };
}

[[nodiscard]] std::string runtime_error_message(
    const auto& callable) {
    try {
        callable();
    } catch (const std::runtime_error& error) {
        return error.what();
    }
    FAIL("expected std::runtime_error");
    return {};
}

} // namespace

TEST_CASE("ExactDocumentReader accepts stable empty and nonempty documents") {
    app::ExactDocumentReader reader;

    {
        auto plan = stable_plan("");
        FakeDirectory directory(plan);
        const auto document = reader.read_child(
            directory, "empty.json", app::ExactDocumentLimits{8, false});
        REQUIRE(document.bytes.empty());
        REQUIRE(document.sha256 ==
                "e3b0c44298fc1c149afbf4c8996fb924"
                "27ae41e4649b934ca495991b7852b855");
    }

    {
        auto plan = stable_plan("");
        FakeDirectory directory(plan);
        REQUIRE_THROWS_AS(
            reader.read_child(
                directory, "empty.json", app::ExactDocumentLimits{8, true}),
            std::runtime_error);
    }

    {
        auto plan = stable_plan("abc");
        FakeDirectory directory(plan);
        const auto document = reader.read_child(
            directory, "document.json", app::ExactDocumentLimits{8, true});
        REQUIRE(document.bytes == bytes_of("abc"));
        REQUIRE(document.sha256 ==
                "ba7816bf8f01cfea414140de5dae2223"
                "b00361a396177a9cb410ff61f20015ad");
        REQUIRE(document.initial_observation == plan.initial);
        REQUIRE(document.final_observation == plan.final);
    }
}

TEST_CASE("ExactDocumentReader accepts exact maximum and rejects one byte overflow") {
    app::ExactDocumentReader reader;

    {
        auto plan = stable_plan("abcd");
        FakeDirectory directory(plan);
        const auto document = reader.read_child(
            directory, "document.json", app::ExactDocumentLimits{4, true});
        REQUIRE(document.bytes == bytes_of("abcd"));
    }

    {
        auto plan = stable_plan("abcde");
        plan.initial = file_observation(4);
        plan.final = file_observation(5);
        FakeDirectory directory(plan);
        REQUIRE_THROWS_AS(
            reader.read_child(
                directory, "document.json", app::ExactDocumentLimits{4, true}),
            std::runtime_error);
    }
}

TEST_CASE("ExactDocumentReader rejects initial oversize before reading") {
    app::ExactDocumentReader reader;
    auto plan = stable_plan("abcde");
    plan.initial = file_observation(5);
    plan.final = plan.initial;
    const auto state = plan.state;
    FakeDirectory directory(plan);

    REQUIRE_THROWS_AS(
        reader.read_child(
            directory, "document.json", app::ExactDocumentLimits{4, true}),
        std::runtime_error);
    REQUIRE(state->read_calls == 0);
}

TEST_CASE("ExactDocumentReader rejects truncation growth and identity mutation") {
    app::ExactDocumentReader reader;

    SECTION("truncation") {
        auto plan = stable_plan("abc");
        plan.initial = file_observation(4);
        plan.final = plan.initial;
        FakeDirectory directory(plan);
        REQUIRE_THROWS_AS(
            reader.read_child(
                directory, "document.json", app::ExactDocumentLimits{8, true}),
            std::runtime_error);
    }

    SECTION("growth") {
        auto plan = stable_plan("abcd");
        plan.initial = file_observation(3);
        plan.final = file_observation(4);
        FakeDirectory directory(plan);
        REQUIRE_THROWS_AS(
            reader.read_child(
                directory, "document.json", app::ExactDocumentLimits{8, true}),
            std::runtime_error);
    }

    SECTION("same-size metadata mutation") {
        auto plan = stable_plan("abcd");
        plan.final =
            file_observation(4, std::byte{1}, std::byte{9});
        FakeDirectory directory(plan);
        REQUIRE_THROWS_AS(
            reader.read_child(
                directory, "document.json", app::ExactDocumentLimits{8, true}),
            std::runtime_error);
    }

    SECTION("object replacement") {
        auto plan = stable_plan("abcd");
        plan.final =
            file_observation(4, std::byte{9}, std::byte{2});
        FakeDirectory directory(plan);
        REQUIRE_THROWS_AS(
            reader.read_child(
                directory, "document.json", app::ExactDocumentLimits{8, true}),
            std::runtime_error);
    }
}

TEST_CASE("ExactDocumentReader rejects child and parent binding changes") {
    app::ExactDocumentReader reader;

    SECTION("child") {
        auto plan = stable_plan("abc");
        plan.binding = ports::PathBindingState::different_object;
        FakeDirectory directory(plan);
        REQUIRE_THROWS_AS(
            reader.read_child(
                directory, "document.json", app::ExactDocumentLimits{8, true}),
            std::runtime_error);
    }

    SECTION("parent before open") {
        auto plan = stable_plan("abc");
        FakeDirectory directory(
            plan,
            ports::PathBindingState::different_object,
            ports::PathBindingState::same_object);
        REQUIRE_THROWS_AS(
            reader.read_child(
                directory, "document.json", app::ExactDocumentLimits{8, true}),
            std::runtime_error);
        REQUIRE(directory.open_calls() == 0);
    }

    SECTION("parent after read") {
        auto plan = stable_plan("abc");
        FakeDirectory directory(
            plan,
            ports::PathBindingState::same_object,
            ports::PathBindingState::different_object);
        REQUIRE_THROWS_AS(
            reader.read_child(
                directory, "document.json", app::ExactDocumentLimits{8, true}),
            std::runtime_error);
    }
}

TEST_CASE("ExactDocumentReader accumulates short reads at exact offsets") {
    app::ExactDocumentReader reader;
    auto plan = stable_plan("abcde");
    plan.maximum_per_read = 2;
    const auto state = plan.state;
    FakeDirectory directory(plan);

    const auto document = reader.read_child(
        directory, "document.json", app::ExactDocumentLimits{10, true});

    REQUIRE(document.bytes == bytes_of("abcde"));
    REQUIRE(state->offsets ==
            std::vector<std::uint64_t>{0, 2, 4, 5});
}

TEST_CASE("ExactDocumentReader validates byte limits before child open") {
    app::ExactDocumentReader reader;

    {
        auto plan = stable_plan("x");
        FakeDirectory directory(plan);
        REQUIRE_THROWS_AS(
            reader.read_child(
                directory, "document.json", app::ExactDocumentLimits{0, true}),
            std::runtime_error);
        REQUIRE(directory.open_calls() == 0);
    }

    {
        auto plan = stable_plan("x");
        FakeDirectory directory(plan);
        REQUIRE_THROWS_AS(
            reader.read_child(
                directory,
                "document.json",
                app::ExactDocumentLimits{UINT64_C(67108865), true}),
            std::runtime_error);
        REQUIRE(directory.open_calls() == 0);
    }
}

TEST_CASE("ExactDocumentReader propagates allocation failure") {
    app::ExactDocumentReader reader;
    auto plan = stable_plan("abc");
    plan.throw_bad_alloc_on_read = true;
    FakeDirectory directory(plan);

    REQUIRE_THROWS_AS(
        reader.read_child(
            directory, "document.json", app::ExactDocumentLimits{8, true}),
        std::bad_alloc);
}

TEST_CASE("parse_bounded_json accepts objects arrays and scalar roots") {
    auto limits = json_limits();

    const auto object = app::parse_bounded_json(
        document_of(R"({"a":[null,true,false,1,-2,"x"]})"), limits);
    REQUIRE(object.is_object());
    REQUIRE(object["a"].is_array());
    REQUIRE(object["a"].size() == 6);
    REQUIRE(object["a"][0].is_null());
    REQUIRE(object["a"][1] == true);
    REQUIRE(object["a"][3] == 1);
    REQUIRE(object["a"][5] == "x");

    REQUIRE(app::parse_bounded_json(document_of("null"), limits).is_null());
    REQUIRE(app::parse_bounded_json(document_of("true"), limits) == true);
    REQUIRE(app::parse_bounded_json(document_of("7"), limits) == 7);
    REQUIRE(app::parse_bounded_json(document_of(R"("text")"), limits) ==
            "text");
    REQUIRE(app::parse_bounded_json(document_of("[]"), limits).is_array());
    REQUIRE(app::parse_bounded_json(document_of("{}"), limits).is_object());
}

TEST_CASE("parse_bounded_json rejects invalid UTF-8") {
    auto document = document_of(R"({"x":""})");
    document.bytes = {
        std::byte{'{'},
        std::byte{'"'},
        std::byte{'x'},
        std::byte{'"'},
        std::byte{':'},
        std::byte{'"'},
        std::byte{0xc3},
        std::byte{0x28},
        std::byte{'"'},
        std::byte{'}'},
    };

    const auto message = runtime_error_message([&] {
        (void)app::parse_bounded_json(document, json_limits());
    });
    REQUIRE(message.find("UTF-8") != std::string::npos);
}

TEST_CASE("parse_bounded_json rejects duplicate keys at every object depth") {
    auto limits = json_limits();

    REQUIRE_THROWS_AS(
        app::parse_bounded_json(
            document_of(R"({"a":1,"a":2})"), limits),
        std::runtime_error);
    REQUIRE_THROWS_AS(
        app::parse_bounded_json(
            document_of(R"({"outer":{"a":1,"a":2}})"), limits),
        std::runtime_error);
}

TEST_CASE("parse_bounded_json enforces depth and total-value definitions") {
    auto limits = json_limits();

    limits.maximum_depth = 2;
    REQUIRE_NOTHROW(
        app::parse_bounded_json(document_of("[[]]"), limits));
    REQUIRE_THROWS_AS(
        app::parse_bounded_json(document_of("[[0]]"), limits),
        std::runtime_error);

    limits = json_limits();
    limits.maximum_values = 3;
    REQUIRE_NOTHROW(
        app::parse_bounded_json(document_of("[1,2]"), limits));
    limits.maximum_values = 2;
    REQUIRE_THROWS_AS(
        app::parse_bounded_json(document_of("[1,2]"), limits),
        std::runtime_error);
}

TEST_CASE("parse_bounded_json enforces per-container limits") {
    auto limits = json_limits();

    limits.maximum_object_members = 1;
    REQUIRE_THROWS_AS(
        app::parse_bounded_json(
            document_of(R"({"a":1,"b":2})"), limits),
        std::runtime_error);

    limits = json_limits();
    limits.maximum_array_items = 1;
    REQUIRE_THROWS_AS(
        app::parse_bounded_json(document_of("[1,2]"), limits),
        std::runtime_error);
}

TEST_CASE("parse_bounded_json enforces key and string UTF-8 byte limits") {
    auto limits = json_limits();

    limits.maximum_key_utf8_bytes = 3;
    REQUIRE_THROWS_AS(
        app::parse_bounded_json(document_of(R"({"abcd":1})"), limits),
        std::runtime_error);

    limits = json_limits();
    limits.maximum_string_utf8_bytes = 3;
    REQUIRE_THROWS_AS(
        app::parse_bounded_json(document_of(R"({"a":"abcd"})"), limits),
        std::runtime_error);

    limits.maximum_string_utf8_bytes = 4;
    REQUIRE_NOTHROW(
        app::parse_bounded_json(
            document_of(R"({"a":"\u00e9\u00e9"})"), limits));
    limits.maximum_string_utf8_bytes = 3;
    REQUIRE_THROWS_AS(
        app::parse_bounded_json(
            document_of(R"({"a":"\u00e9\u00e9"})"), limits),
        std::runtime_error);
}

TEST_CASE("parse_bounded_json controls floating point values") {
    auto limits = json_limits();
    REQUIRE_THROWS_AS(
        app::parse_bounded_json(document_of("1.5"), limits),
        std::runtime_error);

    limits.allow_floating_point = true;
    const auto value =
        app::parse_bounded_json(document_of("1.5"), limits);
    REQUIRE(value.is_number_float());
    REQUIRE(value == 1.5);
}

TEST_CASE("parse_bounded_json rejects trailing and malformed input") {
    auto limits = json_limits();

    REQUIRE_THROWS_AS(
        app::parse_bounded_json(document_of("{} {}"), limits),
        std::runtime_error);
    REQUIRE_THROWS_AS(
        app::parse_bounded_json(document_of(R"({"a":)"), limits),
        std::runtime_error);
}

TEST_CASE("parse_bounded_json diagnostics are bounded and do not echo input") {
    auto limits = json_limits();
    const auto message = runtime_error_message([&] {
        (void)app::parse_bounded_json(
            document_of(R"({"fixture-secret":)"), limits);
    });

    REQUIRE(message.size() <= 512);
    REQUIRE(message.find("fixture-secret") == std::string::npos);
}
