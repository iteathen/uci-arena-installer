#include "arena/ports/filesystem_identity.hpp"

#if defined(_WIN32)
#include "arena/platform/filesystem_identity.hpp"
#include "arena/platform/filesystem_path.hpp"

#define NOMINMAX
#include <windows.h>
#endif

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

#if defined(_WIN32)
namespace arena::platform::windows_identity_testing {
void set_query_failure_after(std::size_t successful_queries, DWORD code) noexcept;
void reset_query_failure() noexcept;
void set_file_id_mutation_after(std::size_t successful_file_id_queries) noexcept;
void reset_file_id_mutation() noexcept;
[[nodiscard]] std::filesystem::path first_component_path(
    const std::filesystem::path& absolute_path);
} // namespace arena::platform::windows_identity_testing
#endif

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

#if defined(_WIN32)
namespace {

namespace windows_testing = arena::platform::windows_identity_testing;

class NativeHandle final {
  public:
    explicit NativeHandle(HANDLE handle = INVALID_HANDLE_VALUE) noexcept
        : handle_(handle) {}

    ~NativeHandle() {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            (void)CloseHandle(handle_);
        }
    }

    NativeHandle(const NativeHandle&) = delete;
    NativeHandle& operator=(const NativeHandle&) = delete;

    [[nodiscard]] bool valid() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

    [[nodiscard]] HANDLE get() const noexcept {
        return handle_;
    }

  private:
    HANDLE handle_;
};

struct NativeTempDirectory final {
    fs::path root;

    NativeTempDirectory() {
        const auto stamp = std::chrono::high_resolution_clock::now()
                               .time_since_epoch()
                               .count();
        root = (fs::temp_directory_path() /
                ("arena-fi-02-" + std::to_string(stamp)))
                   .lexically_normal();
        fs::create_directories(root);
    }

    ~NativeTempDirectory() {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
};

struct QueryFailureReset final {
    ~QueryFailureReset() {
        windows_testing::reset_query_failure();
    }
};

struct FileIdMutationReset final {
    explicit FileIdMutationReset(
        const std::size_t successful_queries) noexcept {
        windows_testing::set_file_id_mutation_after(successful_queries);
    }
    ~FileIdMutationReset() {
        windows_testing::reset_file_id_mutation();
    }
};

void write_bytes(const fs::path& path, const std::string_view bytes) {
    const auto api_path = arena::platform::filesystem_api_path(path);
    NativeHandle file{CreateFileW(
        api_path.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
    REQUIRE(file.valid());

    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto remaining = bytes.size() - offset;
        const auto request = static_cast<DWORD>(
            (std::min)(remaining, static_cast<std::size_t>(MAXDWORD)));
        DWORD written = 0;
        REQUIRE(WriteFile(file.get(), bytes.data() + offset, request,
                          &written, nullptr) != FALSE);
        REQUIRE(written != 0);
        offset += written;
    }
    REQUIRE(FlushFileBuffers(file.get()) != FALSE);
}

[[nodiscard]] std::string read_capability(
    ports::AdmittedRegularFile& file,
    const std::size_t size) {
    std::vector<std::byte> buffer(size);
    const auto read = file.read_at(0, buffer);
    return std::string{
        reinterpret_cast<const char*>(buffer.data()), read};
}

[[nodiscard]] bool make_file_symlink(
    const fs::path& target,
    const fs::path& link) {
    std::error_code error;
    fs::create_symlink(target, link, error);
    return !error;
}

[[nodiscard]] bool make_directory_symlink(
    const fs::path& target,
    const fs::path& link) {
    std::error_code error;
    fs::create_directory_symlink(target, link, error);
    return !error;
}

[[nodiscard]] bool make_directory_junction(
    const fs::path& target,
    const fs::path& link) {
    std::wstring command = L"cmd.exe /d /c mklink /J \"";
    command += link.native();
    command += L"\" \"";
    command += target.native();
    command += L"\" >nul";

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessW(
            nullptr, command.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) == FALSE) {
        return false;
    }
    NativeHandle process_handle{process.hProcess};
    NativeHandle thread_handle{process.hThread};
    if (WaitForSingleObject(process_handle.get(), 10000) != WAIT_OBJECT_0) {
        return false;
    }
    DWORD exit_code = 1;
    return GetExitCodeProcess(process_handle.get(), &exit_code) != FALSE &&
           exit_code == 0;
}

[[nodiscard]] DWORD process_handle_count() {
    DWORD result = 0;
    REQUIRE(GetProcessHandleCount(GetCurrentProcess(), &result) != FALSE);
    return result;
}

} // namespace

TEST_CASE("Windows FilesystemIdentity admits stable files directories and children") {
    NativeTempDirectory workspace;
    const auto root = workspace.root / "root";
    const auto nested = root / "nested";
    fs::create_directories(nested);
    write_bytes(root / "direct.txt", "direct");
    write_bytes(nested / "child.txt", "child");

    auto identity = arena::platform::make_filesystem_identity();
    REQUIRE(identity != nullptr);

    auto file = identity->admit_regular_file(root / "direct.txt");
    REQUIRE(file->initial_observation().kind ==
            ports::FilesystemObjectKind::regular_file);
    REQUIRE(file->initial_observation().size_bytes == 6);
    REQUIRE(file->observe_path_binding() ==
            ports::PathBindingState::same_object);
    REQUIRE(read_capability(*file, 6) == "direct");

    auto directory = identity->admit_directory(root);
    REQUIRE(directory->initial_observation().kind ==
            ports::FilesystemObjectKind::directory);
    REQUIRE(directory->initial_observation().size_bytes == 0);
    REQUIRE(directory->observe_path_binding() ==
            ports::PathBindingState::same_object);

    auto child_directory = directory->open_directory("nested");
    auto child = child_directory->open_regular_file("child.txt");
    REQUIRE(child_directory->observe_path_binding() ==
            ports::PathBindingState::same_object);
    REQUIRE(child->observe_path_binding() ==
            ports::PathBindingState::same_object);
    REQUIRE(read_capability(*child, 5) == "child");
}

TEST_CASE("Windows FilesystemIdentity admits and re-observes the volume root") {
    NativeTempDirectory workspace;
    const auto root = workspace.root.root_path();
    REQUIRE_FALSE(root.empty());

    auto identity = arena::platform::make_filesystem_identity();
    auto directory = identity->admit_directory(root);
    REQUIRE(directory->absolute_path() == root);
    REQUIRE(directory->initial_observation().kind ==
            ports::FilesystemObjectKind::directory);
    REQUIRE(directory->initial_observation().size_bytes == 0);
    REQUIRE(directory->observe_path_binding() ==
            ports::PathBindingState::same_object);
}

TEST_CASE("Windows FilesystemIdentity reports missing wrong-kind and sharing denial") {
    NativeTempDirectory workspace;
    const auto root = workspace.root / "root";
    const auto file_path = root / "file.txt";
    fs::create_directories(root);
    write_bytes(file_path, "data");

    auto identity = arena::platform::make_filesystem_identity();

    try {
        (void)identity->admit_regular_file(root / "missing.txt");
        FAIL("missing file unexpectedly admitted");
    } catch (const ports::FilesystemIdentityException& error) {
        REQUIRE(error.code() == ports::FilesystemIdentityErrorCode::missing);
    }

    try {
        (void)identity->admit_regular_file(root);
        FAIL("directory unexpectedly admitted as regular file");
    } catch (const ports::FilesystemIdentityException& error) {
        REQUIRE(error.code() == ports::FilesystemIdentityErrorCode::wrong_kind);
    }

    try {
        (void)identity->admit_directory(file_path);
        FAIL("file unexpectedly admitted as directory");
    } catch (const ports::FilesystemIdentityException& error) {
        REQUIRE(error.code() == ports::FilesystemIdentityErrorCode::wrong_kind);
    }

    const auto api_path = arena::platform::filesystem_api_path(file_path);
    NativeHandle exclusive{CreateFileW(
        api_path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr)};
    REQUIRE(exclusive.valid());
    try {
        (void)identity->admit_regular_file(file_path);
        FAIL("share-denied file unexpectedly admitted");
    } catch (const ports::FilesystemIdentityException& error) {
        REQUIRE(error.code() == ports::FilesystemIdentityErrorCode::access_denied);
    }
}

TEST_CASE("Windows FilesystemIdentity rejects a live named pipe as special") {
    const std::wstring pipe_name =
        L"\\\\.\\pipe\\arena-fi-02-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(
            std::chrono::high_resolution_clock::now()
                .time_since_epoch()
                .count());
    NativeHandle pipe{CreateNamedPipeW(
        pipe_name.c_str(), PIPE_ACCESS_INBOUND,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0,
        nullptr)};
    REQUIRE(pipe.valid());

    auto identity = arena::platform::make_filesystem_identity();
    try {
        (void)identity->admit_regular_file(fs::path{pipe_name});
        FAIL("named pipe unexpectedly admitted as a regular file");
    } catch (const ports::FilesystemIdentityException& error) {
        REQUIRE(error.code() == ports::FilesystemIdentityErrorCode::special_object);
    }
}

TEST_CASE("Windows FilesystemIdentity UNC decomposition begins from the share root") {
    const fs::path unc{LR"(\\server\share\directory\file.txt)"};
    const auto first = windows_testing::first_component_path(unc);
    REQUIRE_FALSE(first.empty());
    REQUIRE(arena::platform::filesystem_api_path(first).native() ==
            LR"(\\?\UNC\server\share)");
}

TEST_CASE("Windows FilesystemIdentity rejects final and ancestor reparse points") {
    NativeTempDirectory workspace;
    const auto root = workspace.root / "root";
    const auto target_directory = root / "target-directory";
    const auto target_file = root / "target.txt";
    fs::create_directories(target_directory);
    write_bytes(target_file, "target");
    write_bytes(target_directory / "child.txt", "child");

    const auto file_link = root / "file-link.txt";
    const auto directory_link = root / "directory-link";
    if (!make_file_symlink(target_file, file_link) ||
        !make_directory_symlink(target_directory, directory_link)) {
        SUCCEED("Windows symlink creation is unavailable in this test environment");
        return;
    }

    auto identity = arena::platform::make_filesystem_identity();
    try {
        (void)identity->admit_regular_file(file_link);
        FAIL("final reparse point unexpectedly admitted");
    } catch (const ports::FilesystemIdentityException& error) {
        REQUIRE(error.code() ==
                ports::FilesystemIdentityErrorCode::link_or_reparse);
    }

    try {
        (void)identity->admit_regular_file(directory_link / "child.txt");
        FAIL("reparse ancestor unexpectedly admitted");
    } catch (const ports::FilesystemIdentityException& error) {
        REQUIRE(error.code() ==
                ports::FilesystemIdentityErrorCode::link_or_reparse);
    }
}

TEST_CASE("Windows FilesystemIdentity rejects directory junctions") {
    NativeTempDirectory workspace;
    const auto root = workspace.root / "root";
    const auto target = root / "target";
    const auto junction = root / "junction";
    fs::create_directories(target);
    write_bytes(target / "child.txt", "child");

    if (!make_directory_junction(target, junction)) {
        FAIL("Windows junction creation failed; mount-point reparse coverage is required");
    }

    auto identity = arena::platform::make_filesystem_identity();
    try {
        (void)identity->admit_directory(junction);
        FAIL("junction unexpectedly admitted as a directory");
    } catch (const ports::FilesystemIdentityException& error) {
        REQUIRE(error.code() ==
                ports::FilesystemIdentityErrorCode::link_or_reparse);
    }

    try {
        (void)identity->admit_regular_file(junction / "child.txt");
        FAIL("junction ancestor unexpectedly admitted");
    } catch (const ports::FilesystemIdentityException& error) {
        REQUIRE(error.code() ==
                ports::FilesystemIdentityErrorCode::link_or_reparse);
    }
}

TEST_CASE("Windows FilesystemIdentity identity-query failures are neutral and release handles") {
    NativeTempDirectory workspace;
    const auto root = workspace.root / "root";
    const auto path = root / "payload.bin";
    fs::create_directories(root);
    write_bytes(path, "data");

    auto identity = arena::platform::make_filesystem_identity();
    const auto baseline = process_handle_count();
    QueryFailureReset reset;

    for (std::size_t failure_after = 0; failure_after < 8; ++failure_after) {
        windows_testing::set_query_failure_after(
            failure_after, ERROR_NOT_SUPPORTED);
        try {
            (void)identity->admit_regular_file(path);
            FAIL("injected identity query failure unexpectedly succeeded");
        } catch (const ports::FilesystemIdentityException& error) {
            REQUIRE(error.code() ==
                    ports::FilesystemIdentityErrorCode::identity_unavailable);
        }
        windows_testing::reset_query_failure();
    }

    const auto after = process_handle_count();
    REQUIRE(after <= baseline + 2U);
}

TEST_CASE("Windows FilesystemIdentity file capability remains descriptor-bound after pathname replacement") {
    NativeTempDirectory workspace;
    const auto root = workspace.root / "root";
    const auto path = root / "payload.bin";
    const auto old_path = root / "payload.old";
    fs::create_directories(root);
    write_bytes(path, "original");

    auto identity = arena::platform::make_filesystem_identity();
    auto file = identity->admit_regular_file(path);
    const auto initial = file->initial_observation();

    fs::rename(path, old_path);
    write_bytes(path, "replaced");

    REQUIRE(read_capability(*file, 8) == "original");
    REQUIRE(file->observe_open_object().object_identity ==
            initial.object_identity);
    REQUIRE(file->observe_path_binding() ==
            ports::PathBindingState::different_object);
}

TEST_CASE("Windows FilesystemIdentity child capability detects ancestor identity mismatch during binding") {
    NativeTempDirectory workspace;
    const auto root = workspace.root / "root";
    fs::create_directories(root);
    write_bytes(root / "child.txt", "original-child");

    auto identity = arena::platform::make_filesystem_identity();
    auto directory = identity->admit_directory(root);
    auto child = directory->open_regular_file("child.txt");
    const auto initial = child->observe_open_object();

    {
        FileIdMutationReset mutation{0};
        REQUIRE(child->observe_path_binding() ==
                ports::PathBindingState::different_object);
    }

    REQUIRE(read_capability(*child, 14) == "original-child");
    REQUIRE(child->observe_open_object().object_identity ==
            initial.object_identity);
}

TEST_CASE("Windows FilesystemIdentity open-object metadata changes after same-size mutation") {
    NativeTempDirectory workspace;
    const auto root = workspace.root / "root";
    const auto path = root / "payload.bin";
    fs::create_directories(root);
    write_bytes(path, "abcd");

    auto identity = arena::platform::make_filesystem_identity();
    auto file = identity->admit_regular_file(path);
    const auto initial = file->initial_observation();

    write_bytes(path, "wxyz");
    const auto changed = file->observe_open_object();

    REQUIRE(changed.object_identity == initial.object_identity);
    REQUIRE(changed.metadata_identity != initial.metadata_identity);
    REQUIRE(changed.size_bytes == initial.size_bytes);
}

TEST_CASE("Windows FilesystemIdentity read_at uses explicit 64-bit offsets and reports EOF") {
    NativeTempDirectory workspace;
    const auto root = workspace.root / "root";
    const auto path = root / "payload.bin";
    fs::create_directories(root);
    write_bytes(path, "0123456789");

    auto identity = arena::platform::make_filesystem_identity();
    auto file = identity->admit_regular_file(path);
    std::array<std::byte, 4> bytes{};

    REQUIRE(file->read_at(3, bytes) == 4);
    REQUIRE(bytes[0] == std::byte{'3'});
    REQUIRE(bytes[1] == std::byte{'4'});
    REQUIRE(bytes[2] == std::byte{'5'});
    REQUIRE(bytes[3] == std::byte{'6'});
    REQUIRE(file->read_at(UINT64_C(0x100000000), bytes) == 0);
}

TEST_CASE("Windows FilesystemIdentity admitted handles are released exactly by capability lifetime") {
    NativeTempDirectory workspace;
    const auto root = workspace.root / "root";
    const auto path = root / "payload.bin";
    fs::create_directories(root);
    write_bytes(path, "data");

    auto identity = arena::platform::make_filesystem_identity();
    const auto baseline = process_handle_count();
    const auto api_path = arena::platform::filesystem_api_path(path);

    for (std::size_t i = 0; i < 24; ++i) {
        {
            auto file = identity->admit_regular_file(path);
            REQUIRE(file != nullptr);
            REQUIRE(file->initial_observation().kind ==
                    ports::FilesystemObjectKind::regular_file);
        }
        REQUIRE(process_handle_count() <= baseline + 2U);
    }

    NativeHandle exclusive{
        CreateFileW(api_path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, nullptr)};
    REQUIRE(exclusive.valid());
    REQUIRE(process_handle_count() <= baseline + 2U);
}

#endif
