#include "arena/ports/filesystem_identity.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
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

namespace fs = std::filesystem;
namespace ports = arena::ports;

enum class AdapterBehavior {
    return_value,
    return_null,
    throw_neutral,
    throw_unexpected,
    throw_bad_alloc,
};

[[nodiscard]] ports::FilesystemIdentityToken token(const std::byte seed) {
    ports::FilesystemIdentityToken result{};
    result.fill(seed);
    return result;
}

[[nodiscard]] ports::FilesystemObservation observation(
    const ports::FilesystemObjectKind kind,
    const std::uint64_t size = 0,
    const std::byte seed = std::byte{1}) {
    return ports::FilesystemObservation{
        kind,
        size,
        token(seed),
        token(static_cast<std::byte>(std::to_integer<unsigned int>(seed) + 1U)),
    };
}

[[nodiscard]] fs::path valid_absolute_path(const std::string_view leaf) {
    return (fs::absolute(fs::temp_directory_path()) / "arena-fi-01" /
            std::string{leaf})
        .lexically_normal();
}

[[noreturn]] void apply_throw_behavior(const AdapterBehavior behavior) {
    switch (behavior) {
    case AdapterBehavior::throw_neutral:
        throw ports::FilesystemIdentityException(
            ports::FilesystemIdentityErrorCode::access_denied,
            std::string(700, 'n'));
    case AdapterBehavior::throw_unexpected:
        throw std::runtime_error("fixture-secret-unexpected-adapter-message");
    case AdapterBehavior::throw_bad_alloc:
        throw std::bad_alloc{};
    case AdapterBehavior::return_value:
    case AdapterBehavior::return_null:
        break;
    }
    throw std::logic_error("invalid fixture behavior");
}

class FakeRegularFile final : public ports::AdmittedRegularFile {
  public:
    FakeRegularFile(fs::path path,
                    ports::FilesystemObservation initial,
                    ports::FilesystemObservation current,
                    ports::PathBindingState binding)
        : path_(std::move(path)),
          initial_(std::move(initial)),
          current_(std::move(current)),
          binding_(binding) {}

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
        last_offset = offset;
        ++read_calls;
        if (!destination.empty()) {
            destination.front() = std::byte{0x5a};
            return 1;
        }
        return 0;
    }

    [[nodiscard]] ports::FilesystemObservation
    observe_open_object() const override {
        return current_;
    }

    [[nodiscard]] ports::PathBindingState
    observe_path_binding() const override {
        return binding_;
    }

    std::uint64_t last_offset{};
    std::size_t read_calls{};

  private:
    fs::path path_;
    ports::FilesystemObservation initial_;
    ports::FilesystemObservation current_;
    ports::PathBindingState binding_;
};

class FakeDirectory final : public ports::AdmittedDirectory {
  public:
    FakeDirectory(fs::path path,
                  ports::FilesystemObservation initial)
        : ports::AdmittedDirectory(std::move(path), std::move(initial)) {}

    AdapterBehavior regular_behavior{AdapterBehavior::return_value};
    AdapterBehavior directory_behavior{AdapterBehavior::return_value};
    ports::FilesystemObservation regular_observation{
        observation(ports::FilesystemObjectKind::regular_file, 4)};
    ports::FilesystemObservation directory_observation{
        observation(ports::FilesystemObjectKind::directory)};
    mutable std::size_t regular_open_calls{};
    mutable std::size_t directory_open_calls{};

    [[nodiscard]] ports::FilesystemObservation
    observe_open_object() const override {
        return initial_observation();
    }

    [[nodiscard]] ports::PathBindingState
    observe_path_binding() const override {
        return ports::PathBindingState::same_object;
    }

  protected:
    [[nodiscard]] std::unique_ptr<ports::AdmittedRegularFile>
    open_regular_file_validated(const std::string_view leaf) const override {
        ++regular_open_calls;
        if (regular_behavior == AdapterBehavior::return_null) {
            return nullptr;
        }
        if (regular_behavior != AdapterBehavior::return_value) {
            apply_throw_behavior(regular_behavior);
        }
        const auto path = absolute_path() / std::string{leaf};
        return std::make_unique<FakeRegularFile>(
            path, regular_observation, regular_observation,
            ports::PathBindingState::same_object);
    }

    [[nodiscard]] std::unique_ptr<ports::AdmittedDirectory>
    open_directory_validated(const std::string_view leaf) const override {
        ++directory_open_calls;
        if (directory_behavior == AdapterBehavior::return_null) {
            return nullptr;
        }
        if (directory_behavior != AdapterBehavior::return_value) {
            apply_throw_behavior(directory_behavior);
        }
        return std::make_unique<FakeDirectory>(
            absolute_path() / std::string{leaf}, directory_observation);
    }
};

class FakeFilesystemIdentity final : public ports::FilesystemIdentity {
  public:
    AdapterBehavior regular_behavior{AdapterBehavior::return_value};
    AdapterBehavior directory_behavior{AdapterBehavior::return_value};
    ports::FilesystemObservation regular_observation{
        observation(ports::FilesystemObjectKind::regular_file, 4)};
    ports::FilesystemObservation directory_observation{
        observation(ports::FilesystemObjectKind::directory)};
    std::size_t regular_admit_calls{};
    std::size_t directory_admit_calls{};

  protected:
    [[nodiscard]] std::unique_ptr<ports::AdmittedRegularFile>
    admit_regular_file_validated(const fs::path& path) override {
        ++regular_admit_calls;
        if (regular_behavior == AdapterBehavior::return_null) {
            return nullptr;
        }
        if (regular_behavior != AdapterBehavior::return_value) {
            apply_throw_behavior(regular_behavior);
        }
        return std::make_unique<FakeRegularFile>(
            path, regular_observation, regular_observation,
            ports::PathBindingState::same_object);
    }

    [[nodiscard]] std::unique_ptr<ports::AdmittedDirectory>
    admit_directory_validated(const fs::path& path) override {
        ++directory_admit_calls;
        if (directory_behavior == AdapterBehavior::return_null) {
            return nullptr;
        }
        if (directory_behavior != AdapterBehavior::return_value) {
            apply_throw_behavior(directory_behavior);
        }
        return std::make_unique<FakeDirectory>(path, directory_observation);
    }
};

template <typename T>
concept HasWriteMethod = requires(T& value, std::span<const std::byte> bytes) {
    value.write(bytes);
};

template <typename T>
concept HasRemoveMethod = requires(T& value) {
    value.remove();
};

template <typename T>
concept HasPublishMethod = requires(T& value,
                                    std::span<const std::byte> bytes) {
    value.publish(bytes);
};

static_assert(std::is_same_v<ports::FilesystemIdentityToken,
                             std::array<std::byte, 32>>);
static_assert(!HasWriteMethod<ports::AdmittedRegularFile>);
static_assert(!HasRemoveMethod<ports::AdmittedRegularFile>);
static_assert(!HasPublishMethod<ports::AdmittedRegularFile>);
static_assert(!HasWriteMethod<ports::AdmittedDirectory>);
static_assert(!HasRemoveMethod<ports::AdmittedDirectory>);
static_assert(!HasPublishMethod<ports::AdmittedDirectory>);
static_assert(std::is_abstract_v<ports::AdmittedRegularFile>);
static_assert(std::is_abstract_v<ports::AdmittedDirectory>);
static_assert(std::is_abstract_v<ports::FilesystemIdentity>);

} // namespace

TEST_CASE("FilesystemIdentity admits valid absolute normalized paths once") {
    FakeFilesystemIdentity identity;
    const auto file_path = valid_absolute_path("document.json");
    const auto directory_path = valid_absolute_path("directory");

    auto file = identity.admit_regular_file(file_path);
    auto directory = identity.admit_directory(directory_path);

    REQUIRE(identity.regular_admit_calls == 1);
    REQUIRE(identity.directory_admit_calls == 1);
    REQUIRE(file->absolute_path() == file_path);
    REQUIRE(file->initial_observation().kind ==
            ports::FilesystemObjectKind::regular_file);
    REQUIRE(directory->absolute_path() == directory_path);
    REQUIRE(directory->initial_observation().kind ==
            ports::FilesystemObjectKind::directory);
}

TEST_CASE("FilesystemIdentity admits root directories but not root files") {
    const auto root = valid_absolute_path("object").root_path();
    REQUIRE_FALSE(root.empty());

    FakeFilesystemIdentity directory_identity;
    auto directory = directory_identity.admit_directory(root);
    REQUIRE(directory_identity.directory_admit_calls == 1);
    REQUIRE(directory->absolute_path() == root);

    FakeFilesystemIdentity file_identity;
    try {
        (void)file_identity.admit_regular_file(root);
        FAIL("filesystem root unexpectedly admitted as a regular file");
    } catch (const ports::FilesystemIdentityException& error) {
        REQUIRE(error.code() == ports::FilesystemIdentityErrorCode::invalid_path);
    }
    REQUIRE(file_identity.regular_admit_calls == 0);
}

TEST_CASE("FilesystemIdentity rejects invalid paths before adapter invocation") {
    const auto base = valid_absolute_path("object");
    const std::array<fs::path, 5> invalid_paths{
        fs::path{},
        fs::path{"relative/object"},
        base.parent_path() / "segment" / ".." / "object",
        base.parent_path() / std::string{"bad\nobject"},
        base.root_path() / std::string(131073, 'x'),
    };

    for (const auto& path : invalid_paths) {
        FakeFilesystemIdentity regular_identity;
        REQUIRE_THROWS_AS(regular_identity.admit_regular_file(path),
                          ports::FilesystemIdentityException);
        REQUIRE(regular_identity.regular_admit_calls == 0);

        FakeFilesystemIdentity directory_identity;
        REQUIRE_THROWS_AS(directory_identity.admit_directory(path),
                          ports::FilesystemIdentityException);
        REQUIRE(directory_identity.directory_admit_calls == 0);
    }
}

TEST_CASE("AdmittedDirectory validates one child leaf before adapter invocation") {
    FakeDirectory directory(
        valid_absolute_path("directory"),
        observation(ports::FilesystemObjectKind::directory));

    auto file = directory.open_regular_file("child.json");
    auto child_directory = directory.open_directory("nested");

    REQUIRE(file != nullptr);
    REQUIRE(child_directory != nullptr);
    REQUIRE(directory.regular_open_calls == 1);
    REQUIRE(directory.directory_open_calls == 1);

    const std::array<std::string, 9> invalid_leaves{
        "", ".", "..", "a/b", "a\\b", "/absolute", "bad\nleaf",
        std::string(4097, 'x'), "C:\\absolute",
    };
    for (const auto& leaf : invalid_leaves) {
        const auto prior_regular_calls = directory.regular_open_calls;
        REQUIRE_THROWS_AS(directory.open_regular_file(leaf),
                          ports::FilesystemIdentityException);
        REQUIRE(directory.regular_open_calls == prior_regular_calls);

        const auto prior_directory_calls = directory.directory_open_calls;
        REQUIRE_THROWS_AS(directory.open_directory(leaf),
                          ports::FilesystemIdentityException);
        REQUIRE(directory.directory_open_calls == prior_directory_calls);
    }
}

TEST_CASE("FilesystemIdentity rejects wrong-kind direct and child results") {
    FakeFilesystemIdentity file_identity;
    file_identity.regular_observation =
        observation(ports::FilesystemObjectKind::directory);
    try {
        (void)file_identity.admit_regular_file(valid_absolute_path("file"));
        FAIL("wrong-kind direct file admission unexpectedly succeeded");
    } catch (const ports::FilesystemIdentityException& error) {
        REQUIRE(error.code() == ports::FilesystemIdentityErrorCode::wrong_kind);
    }

    FakeFilesystemIdentity directory_identity;
    directory_identity.directory_observation =
        observation(ports::FilesystemObjectKind::regular_file);
    try {
        (void)directory_identity.admit_directory(valid_absolute_path("directory"));
        FAIL("wrong-kind direct directory admission unexpectedly succeeded");
    } catch (const ports::FilesystemIdentityException& error) {
        REQUIRE(error.code() == ports::FilesystemIdentityErrorCode::wrong_kind);
    }

    FakeDirectory directory(
        valid_absolute_path("parent"),
        observation(ports::FilesystemObjectKind::directory));
    directory.regular_observation =
        observation(ports::FilesystemObjectKind::directory);
    try {
        (void)directory.open_regular_file("child-file");
        FAIL("wrong-kind child file admission unexpectedly succeeded");
    } catch (const ports::FilesystemIdentityException& error) {
        REQUIRE(error.code() == ports::FilesystemIdentityErrorCode::wrong_kind);
    }

    directory.directory_observation =
        observation(ports::FilesystemObjectKind::regular_file);
    try {
        (void)directory.open_directory("child-directory");
        FAIL("wrong-kind child directory admission unexpectedly succeeded");
    } catch (const ports::FilesystemIdentityException& error) {
        REQUIRE(error.code() == ports::FilesystemIdentityErrorCode::wrong_kind);
    }
}

TEST_CASE("FilesystemIdentity requires public directory size zero") {
    FakeFilesystemIdentity identity;
    identity.directory_observation =
        observation(ports::FilesystemObjectKind::directory, 1);

    try {
        (void)identity.admit_directory(valid_absolute_path("directory"));
        FAIL("nonzero directory size unexpectedly succeeded");
    } catch (const ports::FilesystemIdentityException& error) {
        REQUIRE(error.code() == ports::FilesystemIdentityErrorCode::wrong_kind);
    }
}

TEST_CASE("FilesystemIdentity preserves neutral errors and bounds diagnostics") {
    FakeFilesystemIdentity identity;
    identity.regular_behavior = AdapterBehavior::throw_neutral;

    try {
        (void)identity.admit_regular_file(valid_absolute_path("file"));
        FAIL("neutral fixture error unexpectedly succeeded");
    } catch (const ports::FilesystemIdentityException& error) {
        REQUIRE(error.code() == ports::FilesystemIdentityErrorCode::access_denied);
        REQUIRE(std::string_view{error.what()}.size() == 512);
    }
}

TEST_CASE("FilesystemIdentity translates unexpected adapter failures") {
    FakeFilesystemIdentity identity;
    identity.regular_behavior = AdapterBehavior::throw_unexpected;

    try {
        (void)identity.admit_regular_file(valid_absolute_path("file"));
        FAIL("unexpected fixture error unexpectedly succeeded");
    } catch (const ports::FilesystemIdentityException& error) {
        REQUIRE(error.code() == ports::FilesystemIdentityErrorCode::io_failure);
        REQUIRE(std::string_view{error.what()}.size() <= 512);
        REQUIRE(std::string_view{error.what()}.find("fixture-secret") ==
                std::string_view::npos);
    }
}

TEST_CASE("FilesystemIdentity does not disguise allocation failure") {
    FakeFilesystemIdentity identity;
    identity.regular_behavior = AdapterBehavior::throw_bad_alloc;

    REQUIRE_THROWS_AS(
        identity.admit_regular_file(valid_absolute_path("file")),
        std::bad_alloc);
}

TEST_CASE("FilesystemIdentity values are opaque equality-comparable evidence") {
    auto first = observation(ports::FilesystemObjectKind::regular_file, 9,
                             std::byte{7});
    auto same = first;
    auto different = first;
    different.metadata_identity[0] = std::byte{9};

    REQUIRE(first == same);
    REQUIRE_FALSE(first == different);

    const auto first_token = token(std::byte{3});
    const auto same_token = first_token;
    auto different_token = first_token;
    different_token[0] = std::byte{4};
    REQUIRE(first_token == same_token);
    REQUIRE_FALSE(first_token == different_token);

    FakeRegularFile file(valid_absolute_path("file"), first, same,
                         ports::PathBindingState::same_object);
    std::array<std::byte, 1> destination{};
    REQUIRE(file.read_at(UINT64_C(0x100000000), destination) == 1);
    REQUIRE(file.last_offset == UINT64_C(0x100000000));
    REQUIRE(file.observe_open_object() == same);
    REQUIRE(file.observe_path_binding() ==
            ports::PathBindingState::same_object);
}

TEST_CASE("FilesystemIdentity enum vocabulary exposes all neutral states") {
    constexpr std::array binding_states{
        ports::PathBindingState::same_object,
        ports::PathBindingState::missing,
        ports::PathBindingState::different_object,
        ports::PathBindingState::uninspectable,
    };
    constexpr std::array error_codes{
        ports::FilesystemIdentityErrorCode::invalid_path,
        ports::FilesystemIdentityErrorCode::missing,
        ports::FilesystemIdentityErrorCode::wrong_kind,
        ports::FilesystemIdentityErrorCode::link_or_reparse,
        ports::FilesystemIdentityErrorCode::special_object,
        ports::FilesystemIdentityErrorCode::access_denied,
        ports::FilesystemIdentityErrorCode::identity_unavailable,
        ports::FilesystemIdentityErrorCode::identity_changed,
        ports::FilesystemIdentityErrorCode::io_failure,
    };

    REQUIRE(binding_states.size() == 4);
    REQUIRE(error_codes.size() == 9);
}
