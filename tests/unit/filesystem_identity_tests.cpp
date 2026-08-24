#include "arena/ports/filesystem_identity.hpp"

#ifdef _WIN32
#include "arena/platform/filesystem_identity.hpp"
#include "arena/platform/filesystem_path.hpp"
#include "../../src/platform/windows/filesystem_identity_private.hpp"

#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#endif

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
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

#ifdef _WIN32

class TestHandle final {
  public:
    TestHandle() noexcept = default;
    explicit TestHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~TestHandle() {
        if (valid()) CloseHandle(handle_);
    }

    TestHandle(const TestHandle&) = delete;
    TestHandle& operator=(const TestHandle&) = delete;

    TestHandle(TestHandle&& other) noexcept
        : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)) {}

    TestHandle& operator=(TestHandle&& other) noexcept {
        if (this == &other) return *this;
        if (valid()) CloseHandle(handle_);
        handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
        return *this;
    }

    [[nodiscard]] bool valid() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }

  private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
};

class WindowsTempRoot final {
  public:
    WindowsTempRoot() {
        static std::atomic_uint64_t sequence{};
        const auto parent = fs::temp_directory_path();
        for (int attempt = 0; attempt < 100; ++attempt) {
            const auto value = sequence.fetch_add(1, std::memory_order_relaxed);
            auto candidate =
                parent /
                ("arena-fi-02-" + std::to_string(GetCurrentProcessId()) + "-" +
                 std::to_string(value));
            std::error_code error;
            if (fs::create_directory(candidate, error)) {
                path_ = std::move(candidate);
                return;
            }
            if (error) {
                throw std::runtime_error("temporary directory creation failed");
            }
        }
        throw std::runtime_error("temporary directory allocation failed");
    }

    ~WindowsTempRoot() {
        std::error_code ignored;
        fs::remove_all(arena::platform::filesystem_api_path(path_), ignored);
    }

    WindowsTempRoot(const WindowsTempRoot&) = delete;
    WindowsTempRoot& operator=(const WindowsTempRoot&) = delete;

    [[nodiscard]] const fs::path& path() const noexcept { return path_; }

  private:
    fs::path path_;
};

void write_file(const fs::path& path, const std::string_view bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream || !stream.write(bytes.data(),
                                 static_cast<std::streamsize>(bytes.size()))) {
        throw std::runtime_error("test file write failed");
    }
}

std::string read_file(ports::AdmittedRegularFile& file,
                      const std::uint64_t offset = 0) {
    std::array<std::byte, 64> buffer{};
    const auto count = file.read_at(offset, buffer);
    return {reinterpret_cast<const char*>(buffer.data()), count};
}

bool create_file_symlink(const fs::path& link,
                         const fs::path& target,
                         DWORD& native_error) {
    if (CreateSymbolicLinkW(
            link.c_str(), target.c_str(),
            SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != FALSE) {
        native_error = ERROR_SUCCESS;
        return true;
    }
    native_error = GetLastError();
    return false;
}

struct MountPointReparseHeader {
    ULONG tag;
    USHORT data_length;
    USHORT reserved;
    USHORT substitute_offset;
    USHORT substitute_length;
    USHORT print_offset;
    USHORT print_length;
};

static_assert(sizeof(MountPointReparseHeader) == 16);

bool create_junction(const fs::path& link,
                     const fs::path& target,
                     DWORD& native_error) {
    std::error_code directory_error;
    if (!fs::create_directory(link, directory_error)) {
        native_error = directory_error
                           ? static_cast<DWORD>(directory_error.value())
                           : ERROR_ALREADY_EXISTS;
        return false;
    }

    TestHandle handle{CreateFileW(
        link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
    if (!handle.valid()) {
        native_error = GetLastError();
        return false;
    }

    const std::wstring print_name = target.native();
    const std::wstring substitute_name = L"\\??\\" + print_name;
    const auto substitute_bytes = substitute_name.size() * sizeof(wchar_t);
    const auto print_bytes = print_name.size() * sizeof(wchar_t);
    const auto path_bytes = substitute_bytes + sizeof(wchar_t) + print_bytes +
                            sizeof(wchar_t);
    if (path_bytes + 8 > (std::numeric_limits<USHORT>::max)()) {
        native_error = ERROR_BUFFER_OVERFLOW;
        return false;
    }

    std::vector<std::byte> buffer(sizeof(MountPointReparseHeader) + path_bytes);
    auto* header =
        reinterpret_cast<MountPointReparseHeader*>(buffer.data());
    header->tag = IO_REPARSE_TAG_MOUNT_POINT;
    header->data_length = static_cast<USHORT>(8 + path_bytes);
    header->reserved = 0;
    header->substitute_offset = 0;
    header->substitute_length = static_cast<USHORT>(substitute_bytes);
    header->print_offset =
        static_cast<USHORT>(substitute_bytes + sizeof(wchar_t));
    header->print_length = static_cast<USHORT>(print_bytes);

    auto* path_buffer = reinterpret_cast<wchar_t*>(
        buffer.data() + sizeof(MountPointReparseHeader));
    std::memcpy(path_buffer, substitute_name.c_str(), substitute_bytes);
    path_buffer[substitute_name.size()] = L'\0';
    auto* print_buffer = reinterpret_cast<wchar_t*>(
        reinterpret_cast<std::byte*>(path_buffer) + header->print_offset);
    std::memcpy(print_buffer, print_name.c_str(), print_bytes);
    print_buffer[print_name.size()] = L'\0';

    DWORD returned{};
    if (DeviceIoControl(handle.get(), FSCTL_SET_REPARSE_POINT, buffer.data(),
                        static_cast<DWORD>(8 + header->data_length), nullptr, 0,
                        &returned, nullptr) == FALSE) {
        native_error = GetLastError();
        return false;
    }
    native_error = ERROR_SUCCESS;
    return true;
}

TestHandle open_exclusive(const fs::path& path, const bool directory = false) {
    return TestHandle{CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES, 0, nullptr, OPEN_EXISTING,
        directory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL,
        nullptr)};
}

template <typename T>
concept HasNativeHandle = requires(T& value) {
    value.native_handle();
};

static_assert(!HasNativeHandle<ports::AdmittedRegularFile>);
static_assert(!HasNativeHandle<ports::AdmittedDirectory>);
static_assert(!HasNativeHandle<ports::FilesystemIdentity>);

#endif

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

#ifdef _WIN32

TEST_CASE("Windows FilesystemIdentity admits stable files directories and children") {
    WindowsTempRoot temporary;
    const auto nested_path = temporary.path() / "nested";
    REQUIRE(fs::create_directory(nested_path));

    const std::string utf8_leaf{"r\xc3\xa9sum\xc3\xa9.bin"};
    std::u8string encoded_leaf;
    for (const unsigned char byte : utf8_leaf) {
        encoded_leaf.push_back(static_cast<char8_t>(byte));
    }
    const auto file_path = temporary.path() / fs::path{encoded_leaf};
    write_file(file_path, "original-bytes");

    auto identity = arena::platform::make_filesystem_identity();
    REQUIRE(identity != nullptr);
    auto directory = identity->admit_directory(temporary.path());
    auto direct_file = identity->admit_regular_file(file_path);
    auto child_file = directory->open_regular_file(utf8_leaf);
    auto child_directory = directory->open_directory("nested");

    REQUIRE(directory->initial_observation().kind ==
            ports::FilesystemObjectKind::directory);
    REQUIRE(directory->initial_observation().size_bytes == 0);
    REQUIRE(directory->observe_open_object() ==
            directory->initial_observation());
    REQUIRE(directory->observe_path_binding() ==
            ports::PathBindingState::same_object);
    REQUIRE(child_directory->observe_path_binding() ==
            ports::PathBindingState::same_object);

    REQUIRE(direct_file->initial_observation().kind ==
            ports::FilesystemObjectKind::regular_file);
    REQUIRE(direct_file->initial_observation().size_bytes == 14);
    REQUIRE(direct_file->initial_observation().object_identity ==
            child_file->initial_observation().object_identity);
    REQUIRE(direct_file->initial_observation().metadata_identity ==
            child_file->initial_observation().metadata_identity);
    REQUIRE(direct_file->observe_path_binding() ==
            ports::PathBindingState::same_object);
    REQUIRE(child_file->observe_path_binding() ==
            ports::PathBindingState::same_object);
    REQUIRE(read_file(*child_file) == "original-bytes");
}

TEST_CASE("Windows FilesystemIdentity admits extended-length paths") {
    WindowsTempRoot temporary;
    auto directory = temporary.path();
    for (int index = 0; index < 6; ++index) {
        directory /= std::string(48, static_cast<char>('a' + index));
        const auto api_directory =
            arena::platform::filesystem_api_path(directory);
        REQUIRE(CreateDirectoryW(api_directory.c_str(), nullptr) != FALSE);
    }
    const auto file_path = directory / "payload.bin";
    const auto api_file = arena::platform::filesystem_api_path(file_path);
    TestHandle output{CreateFileW(
        api_file.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
    REQUIRE(output.valid());
    constexpr std::string_view payload{"long-path"};
    DWORD written{};
    REQUIRE(WriteFile(output.get(), payload.data(),
                      static_cast<DWORD>(payload.size()), &written,
                      nullptr) != FALSE);
    REQUIRE(written == payload.size());
    output = TestHandle{};
    REQUIRE(file_path.native().size() > MAX_PATH);

    auto identity = arena::platform::make_filesystem_identity();
    auto admitted = identity->admit_regular_file(file_path);
    REQUIRE(read_file(*admitted) == payload);
    REQUIRE(admitted->observe_path_binding() ==
            ports::PathBindingState::same_object);
}

TEST_CASE("Windows FilesystemIdentity maps missing and wrong-kind admissions") {
    WindowsTempRoot temporary;
    const auto file_path = temporary.path() / "file.bin";
    const auto directory_path = temporary.path() / "directory";
    write_file(file_path, "data");
    REQUIRE(fs::create_directory(directory_path));
    auto identity = arena::platform::make_filesystem_identity();

    auto missing_code = ports::FilesystemIdentityErrorCode::io_failure;
    try {
        (void)identity->admit_regular_file(temporary.path() / "missing.bin");
        FAIL("missing file admission unexpectedly succeeded");
    } catch (const ports::FilesystemIdentityException& error) {
        missing_code = error.code();
    }
    REQUIRE(static_cast<int>(missing_code) ==
            static_cast<int>(ports::FilesystemIdentityErrorCode::missing));
    auto file_kind_code = ports::FilesystemIdentityErrorCode::io_failure;
    try {
        (void)identity->admit_regular_file(directory_path);
        FAIL("directory admitted as a regular file");
    } catch (const ports::FilesystemIdentityException& error) {
        file_kind_code = error.code();
    }
    REQUIRE(static_cast<int>(file_kind_code) ==
            static_cast<int>(ports::FilesystemIdentityErrorCode::wrong_kind));
    auto directory_kind_code = ports::FilesystemIdentityErrorCode::io_failure;
    try {
        (void)identity->admit_directory(file_path);
        FAIL("regular file admitted as a directory");
    } catch (const ports::FilesystemIdentityException& error) {
        directory_kind_code = error.code();
    }
    REQUIRE(static_cast<int>(directory_kind_code) ==
            static_cast<int>(ports::FilesystemIdentityErrorCode::wrong_kind));
}

TEST_CASE("Windows FilesystemIdentity rejects a final symbolic link") {
    WindowsTempRoot temporary;
    const auto target = temporary.path() / "target.bin";
    const auto link = temporary.path() / "link.bin";
    write_file(target, "target");
    DWORD native_error{};
    if (!create_file_symlink(link, target, native_error)) {
        SKIP("symbolic-link creation is unavailable (win32=" << native_error
                                                              << ")");
    }

    auto identity = arena::platform::make_filesystem_identity();
    try {
        (void)identity->admit_regular_file(link);
        FAIL("symbolic link admission unexpectedly succeeded");
    } catch (const ports::FilesystemIdentityException& error) {
        REQUIRE(error.code() ==
                ports::FilesystemIdentityErrorCode::link_or_reparse);
    }
}

TEST_CASE("Windows FilesystemIdentity rejects a junction ancestor") {
    WindowsTempRoot temporary;
    const auto target = temporary.path() / "target";
    const auto junction = temporary.path() / "junction";
    REQUIRE(fs::create_directory(target));
    write_file(target / "payload.bin", "payload");
    DWORD native_error{};
    if (!create_junction(junction, target, native_error)) {
        SKIP("junction creation is unavailable (win32=" << native_error << ")");
    }

    auto identity = arena::platform::make_filesystem_identity();
    try {
        (void)identity->admit_directory(junction);
        FAIL("final junction admission unexpectedly succeeded");
    } catch (const ports::FilesystemIdentityException& error) {
        REQUIRE(error.code() ==
                ports::FilesystemIdentityErrorCode::link_or_reparse);
    }
    try {
        (void)identity->admit_regular_file(junction / "payload.bin");
        FAIL("junction ancestor admission unexpectedly succeeded");
    } catch (const ports::FilesystemIdentityException& error) {
        REQUIRE(error.code() ==
                ports::FilesystemIdentityErrorCode::link_or_reparse);
    }
}

TEST_CASE("Windows FilesystemIdentity rejects device namespaces") {
    auto identity = arena::platform::make_filesystem_identity();
    const fs::path device_path{LR"(\\.\pipe\arena-fi-02-special)"};
    try {
        (void)identity->admit_regular_file(device_path);
        FAIL("device namespace admission unexpectedly succeeded");
    } catch (const ports::FilesystemIdentityException& error) {
        REQUIRE(error.code() ==
                ports::FilesystemIdentityErrorCode::special_object);
    }
}

TEST_CASE("Windows FilesystemIdentity maps access and stable-identity failures") {
    WindowsTempRoot temporary;
    const auto file_path = temporary.path() / "blocked.bin";
    write_file(file_path, "blocked");
    TestHandle blocker{CreateFileW(file_path.c_str(), GENERIC_READ, 0, nullptr,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                   nullptr)};
    REQUIRE(blocker.valid());

    auto identity = arena::platform::make_filesystem_identity();
    try {
        (void)identity->admit_regular_file(file_path);
        FAIL("share-denied admission unexpectedly succeeded");
    } catch (const ports::FilesystemIdentityException& error) {
        REQUIRE(error.code() ==
                ports::FilesystemIdentityErrorCode::access_denied);
        REQUIRE(std::string_view{error.what()}.size() <= 512);
    }

    try {
        arena::platform::windows_detail::throw_stable_identity_unavailable(
            ERROR_INVALID_FUNCTION);
        FAIL("stable-identity failure mapping unexpectedly returned");
    } catch (const ports::FilesystemIdentityException& error) {
        REQUIRE(error.code() ==
                ports::FilesystemIdentityErrorCode::identity_unavailable);
        REQUIRE(std::string_view{error.what()}.size() <= 512);
    }
}

TEST_CASE("Windows descriptor reads survive pathname rename and replacement") {
    WindowsTempRoot temporary;
    const auto file_path = temporary.path() / "current.bin";
    const auto moved_path = temporary.path() / "original.bin";
    write_file(file_path, "original");
    auto identity = arena::platform::make_filesystem_identity();
    auto admitted = identity->admit_regular_file(file_path);
    const auto initial = admitted->initial_observation();

    fs::rename(file_path, moved_path);
    REQUIRE(admitted->observe_path_binding() ==
            ports::PathBindingState::missing);
    write_file(file_path, "replaced");

    REQUIRE(read_file(*admitted) == "original");
    REQUIRE(admitted->observe_open_object().object_identity ==
            initial.object_identity);
    REQUIRE(admitted->observe_path_binding() ==
            ports::PathBindingState::different_object);
    REQUIRE(read_file(*admitted, UINT64_C(0x100000000)).empty());
}

TEST_CASE("Windows path binding detects ancestor replacement") {
    WindowsTempRoot temporary;
    const auto parent = temporary.path() / "parent";
    const auto prior_parent = temporary.path() / "prior-parent";
    REQUIRE(fs::create_directory(parent));
    const auto file_path = parent / "payload.bin";
    write_file(file_path, "original");
    auto identity = arena::platform::make_filesystem_identity();
    auto admitted = identity->admit_regular_file(file_path);
    const auto initial = admitted->initial_observation();

    REQUIRE(fs::create_directory(prior_parent));
    fs::rename(file_path, prior_parent / "payload.bin");
    REQUIRE(fs::remove(parent));
    REQUIRE(fs::create_directory(parent));
    write_file(parent / "payload.bin", "original");

    REQUIRE(admitted->observe_open_object().object_identity ==
            initial.object_identity);
    const auto binding = admitted->observe_path_binding();
    REQUIRE((binding == ports::PathBindingState::different_object ||
             binding == ports::PathBindingState::uninspectable));
}

TEST_CASE("Windows open-object metadata detects same-size modification") {
    WindowsTempRoot temporary;
    const auto file_path = temporary.path() / "mutable.bin";
    write_file(file_path, "first");
    auto identity = arena::platform::make_filesystem_identity();
    auto admitted = identity->admit_regular_file(file_path);
    const auto initial = admitted->initial_observation();

    write_file(file_path, "other");
    TestHandle attributes{CreateFileW(
        file_path.c_str(), FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    REQUIRE(attributes.valid());
    FILETIME write_time{};
    GetSystemTimeAsFileTime(&write_time);
    ULARGE_INTEGER adjusted{};
    adjusted.LowPart = write_time.dwLowDateTime;
    adjusted.HighPart = write_time.dwHighDateTime;
    adjusted.QuadPart += UINT64_C(20000000);
    write_time.dwLowDateTime = adjusted.LowPart;
    write_time.dwHighDateTime = adjusted.HighPart;
    REQUIRE(SetFileTime(attributes.get(), nullptr, nullptr, &write_time) != FALSE);
    attributes = TestHandle{};

    const auto changed = admitted->observe_open_object();
    REQUIRE(changed.object_identity == initial.object_identity);
    REQUIRE(changed.size_bytes == initial.size_bytes);
    REQUIRE(changed.metadata_identity != initial.metadata_identity);
}

TEST_CASE("Windows FilesystemIdentity closes handles after success and failure") {
    WindowsTempRoot temporary;
    const auto file_path = temporary.path() / "success.bin";
    const auto directory_path = temporary.path() / "wrong-kind";
    write_file(file_path, "success");
    REQUIRE(fs::create_directory(directory_path));
    auto identity = arena::platform::make_filesystem_identity();

    {
        auto admitted = identity->admit_regular_file(file_path);
        REQUIRE(admitted != nullptr);
    }
    auto exclusive_file = open_exclusive(file_path);
    REQUIRE(exclusive_file.valid());
    exclusive_file = TestHandle{};

    try {
        (void)identity->admit_regular_file(directory_path);
        FAIL("wrong-kind admission unexpectedly succeeded");
    } catch (const ports::FilesystemIdentityException& error) {
        REQUIRE(error.code() == ports::FilesystemIdentityErrorCode::wrong_kind);
    }
    auto exclusive_directory = open_exclusive(directory_path, true);
    REQUIRE(exclusive_directory.valid());
}

#endif
