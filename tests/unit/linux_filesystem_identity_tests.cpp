#include "arena/platform/filesystem_identity.hpp"
#include "arena/ports/filesystem_identity.hpp"
#include "filesystem_identity_test_seam.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace ports = arena::ports;
namespace linux_testing = arena::platform::linux_identity_testing;

class UniqueFd final {
  public:
    explicit UniqueFd(const int value = -1) noexcept : value_(value) {}
    ~UniqueFd() {
        if (value_ >= 0) {
            (void)::close(value_);
        }
    }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    [[nodiscard]] int get() const noexcept {
        return value_;
    }

    [[nodiscard]] bool valid() const noexcept {
        return value_ >= 0;
    }

  private:
    int value_;
};

struct NativeTempDirectory final {
    fs::path root;

    NativeTempDirectory() {
        auto pattern =
            (fs::temp_directory_path() / "arena-fi-03-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const char* created = ::mkdtemp(writable.data());
        REQUIRE(created != nullptr);
        root = fs::path{created}.lexically_normal();
    }

    ~NativeTempDirectory() {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
};

void write_all(const int descriptor, const std::string_view bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        ssize_t written = -1;
        do {
            written = ::write(
                descriptor, bytes.data() + offset, bytes.size() - offset);
        } while (written < 0 && errno == EINTR);
        REQUIRE(written > 0);
        offset += static_cast<std::size_t>(written);
    }
    REQUIRE(::fsync(descriptor) == 0);
}

void write_file(const fs::path& path, const std::string_view bytes) {
    UniqueFd descriptor{::open(
        path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600)};
    REQUIRE(descriptor.valid());
    write_all(descriptor.get(), bytes);
}

[[nodiscard]] std::string read_capability(
    ports::AdmittedRegularFile& file,
    const std::size_t requested) {
    std::vector<std::byte> bytes(requested);
    const auto transferred = file.read_at(0, bytes);
    return std::string{
        reinterpret_cast<const char*>(bytes.data()), transferred};
}

template <typename Operation>
void require_error_code(
    Operation&& operation,
    const ports::FilesystemIdentityErrorCode expected) {
    try {
        operation();
        FAIL("filesystem identity operation unexpectedly succeeded");
    } catch (const ports::FilesystemIdentityException& error) {
        REQUIRE(error.code() == expected);
    }
}

[[nodiscard]] std::size_t open_descriptor_count() {
    std::size_t count = 0;
    std::error_code error;
    for (fs::directory_iterator iterator{"/proc/self/fd", error}, end;
         !error && iterator != end;
         iterator.increment(error)) {
        ++count;
    }
    REQUIRE_FALSE(error);
    return count;
}

std::atomic<int> g_pread_calls{0};

ssize_t pread_with_one_eintr(
    const int descriptor,
    void* destination,
    const std::size_t size,
    const off_t offset) {
    if (g_pread_calls.fetch_add(1, std::memory_order_relaxed) == 0) {
        errno = EINTR;
        return -1;
    }
    return ::pread(descriptor, destination, size, offset);
}

int openat_access_denied(const int, const char*, const int) {
    errno = EACCES;
    return -1;
}

struct PreadReset final {
    ~PreadReset() {
        linux_testing::reset_pread_function();
    }
};

struct OpenAtReset final {
    ~OpenAtReset() {
        linux_testing::reset_openat_function();
    }
};

} // namespace

TEST_CASE("Linux FilesystemIdentity admits stable files directories and children") {
    NativeTempDirectory workspace;
    const auto root = workspace.root / "root";
    const auto nested = root / "nested";
    fs::create_directories(nested);
    write_file(root / "direct.txt", "direct");
    write_file(nested / "child.txt", "child");

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

TEST_CASE("Linux FilesystemIdentity admits the filesystem root as a directory") {
    auto identity = arena::platform::make_filesystem_identity();
    const fs::path root{"/"};

    auto directory = identity->admit_directory(root);
    REQUIRE(directory->absolute_path() == root);
    REQUIRE(directory->initial_observation().kind ==
            ports::FilesystemObjectKind::directory);
    REQUIRE(directory->initial_observation().size_bytes == 0);
    REQUIRE(directory->observe_open_object().object_identity ==
            directory->initial_observation().object_identity);
    REQUIRE(directory->observe_path_binding() ==
            ports::PathBindingState::same_object);
}

TEST_CASE("Linux FilesystemIdentity reports missing and wrong-kind objects") {
    NativeTempDirectory workspace;
    const auto root = workspace.root / "root";
    const auto file_path = root / "file.txt";
    fs::create_directories(root);
    write_file(file_path, "data");

    auto identity = arena::platform::make_filesystem_identity();

    require_error_code(
        [&] { (void)identity->admit_regular_file(root / "missing.txt"); },
        ports::FilesystemIdentityErrorCode::missing);
    require_error_code(
        [&] { (void)identity->admit_regular_file(root); },
        ports::FilesystemIdentityErrorCode::wrong_kind);
    require_error_code(
        [&] { (void)identity->admit_directory(file_path); },
        ports::FilesystemIdentityErrorCode::wrong_kind);
}

TEST_CASE("Linux FilesystemIdentity rejects final and ancestor symbolic links") {
    NativeTempDirectory workspace;
    const auto root = workspace.root / "root";
    const auto target_directory = root / "target-directory";
    const auto target_file = root / "target.txt";
    fs::create_directories(target_directory);
    write_file(target_file, "target");
    write_file(target_directory / "child.txt", "child");

    const auto file_link = root / "file-link.txt";
    const auto directory_link = root / "directory-link";
    fs::create_symlink(target_file, file_link);
    fs::create_directory_symlink(target_directory, directory_link);

    auto identity = arena::platform::make_filesystem_identity();
    require_error_code(
        [&] { (void)identity->admit_regular_file(file_link); },
        ports::FilesystemIdentityErrorCode::link_or_reparse);
    require_error_code(
        [&] {
            (void)identity->admit_regular_file(
                directory_link / "child.txt");
        },
        ports::FilesystemIdentityErrorCode::link_or_reparse);
}

TEST_CASE("Linux FilesystemIdentity rejects FIFOs without blocking") {
    NativeTempDirectory workspace;
    const auto fifo = workspace.root / "pipe";
    REQUIRE(::mkfifo(fifo.c_str(), 0600) == 0);

    auto identity = arena::platform::make_filesystem_identity();
    const auto started = std::chrono::steady_clock::now();
    require_error_code(
        [&] { (void)identity->admit_regular_file(fifo); },
        ports::FilesystemIdentityErrorCode::special_object);
    REQUIRE(std::chrono::steady_clock::now() - started <
            std::chrono::seconds{2});
}

TEST_CASE("Linux FilesystemIdentity rejects Unix sockets as special objects") {
    NativeTempDirectory workspace;
    const auto socket_path = workspace.root / "socket";
    const auto native_path = socket_path.string();
    REQUIRE(native_path.size() < sizeof(sockaddr_un::sun_path));

    UniqueFd socket_descriptor{
        ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)};
    REQUIRE(socket_descriptor.valid());

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::copy(native_path.begin(), native_path.end(), address.sun_path);
    address.sun_path[native_path.size()] = '\0';
    const auto address_size = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + native_path.size() + 1U);
    REQUIRE(::bind(
                socket_descriptor.get(),
                reinterpret_cast<const sockaddr*>(&address),
                address_size) == 0);

    auto identity = arena::platform::make_filesystem_identity();
    require_error_code(
        [&] { (void)identity->admit_regular_file(socket_path); },
        ports::FilesystemIdentityErrorCode::special_object);
}

TEST_CASE("Linux openat access failures stay neutral and binding is uninspectable") {
    NativeTempDirectory workspace;
    const auto path = workspace.root / "payload.bin";
    write_file(path, "data");

    auto identity = arena::platform::make_filesystem_identity();
    auto file = identity->admit_regular_file(path);

    linux_testing::set_openat_function(&openat_access_denied);
    OpenAtReset reset;

    require_error_code(
        [&] { (void)identity->admit_regular_file(path); },
        ports::FilesystemIdentityErrorCode::access_denied);
    REQUIRE(file->observe_path_binding() ==
            ports::PathBindingState::uninspectable);
}

TEST_CASE("Linux file capability remains descriptor-bound after path replacement") {
    NativeTempDirectory workspace;
    const auto root = workspace.root / "root";
    const auto path = root / "payload.bin";
    const auto old_path = root / "payload.old";
    fs::create_directories(root);
    write_file(path, "original");

    auto identity = arena::platform::make_filesystem_identity();
    auto file = identity->admit_regular_file(path);
    const auto initial = file->initial_observation();

    fs::rename(path, old_path);
    write_file(path, "replaced");

    REQUIRE(read_capability(*file, 8) == "original");
    REQUIRE(file->observe_open_object().object_identity ==
            initial.object_identity);
    REQUIRE(file->observe_path_binding() ==
            ports::PathBindingState::different_object);
}

TEST_CASE("Linux child capability survives parent path rebinding") {
    NativeTempDirectory workspace;
    const auto root = workspace.root / "root";
    const auto old_root = workspace.root / "root-old";
    fs::create_directories(root);
    write_file(root / "child.txt", "original-child");

    auto identity = arena::platform::make_filesystem_identity();
    auto directory = identity->admit_directory(root);
    auto child = directory->open_regular_file("child.txt");

    fs::rename(root, old_root);
    fs::create_directories(root);
    write_file(root / "child.txt", "replacement!!");

    REQUIRE(read_capability(*child, 14) == "original-child");
    REQUIRE(child->observe_path_binding() ==
            ports::PathBindingState::different_object);
    const auto parent_binding = directory->observe_path_binding();
    REQUIRE((parent_binding == ports::PathBindingState::different_object ||
             parent_binding == ports::PathBindingState::missing));
}

TEST_CASE("Linux open-object metadata changes after same-size mutation") {
    NativeTempDirectory workspace;
    const auto path = workspace.root / "payload.bin";
    write_file(path, "abcd");

    auto identity = arena::platform::make_filesystem_identity();
    auto file = identity->admit_regular_file(path);
    const auto initial = file->initial_observation();

    auto changed = initial;
    for (int attempt = 0;
         attempt < 20 && changed.metadata_identity == initial.metadata_identity;
         ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
        write_file(path, attempt % 2 == 0 ? "wxyz" : "qrst");
        changed = file->observe_open_object();
    }

    REQUIRE(changed.object_identity == initial.object_identity);
    REQUIRE(changed.metadata_identity != initial.metadata_identity);
    REQUIRE(changed.size_bytes == initial.size_bytes);
}

TEST_CASE("Linux read_at retries EINTR and uses explicit offsets") {
    NativeTempDirectory workspace;
    const auto path = workspace.root / "payload.bin";
    write_file(path, "0123456789");

    auto identity = arena::platform::make_filesystem_identity();
    auto file = identity->admit_regular_file(path);

    g_pread_calls.store(0, std::memory_order_relaxed);
    linux_testing::set_pread_function(&pread_with_one_eintr);
    PreadReset reset;

    std::array<std::byte, 4> bytes{};
    REQUIRE(file->read_at(3, bytes) == 4);
    REQUIRE(g_pread_calls.load(std::memory_order_relaxed) == 2);
    REQUIRE(bytes[0] == std::byte{'3'});
    REQUIRE(bytes[1] == std::byte{'4'});
    REQUIRE(bytes[2] == std::byte{'5'});
    REQUIRE(bytes[3] == std::byte{'6'});
}

TEST_CASE("Linux read_at supports sparse offsets beyond four GiB") {
    if constexpr (sizeof(off_t) < 8) {
        SUCCEED("platform off_t is narrower than 64 bits");
        return;
    }

    NativeTempDirectory workspace;
    const auto path = workspace.root / "sparse.bin";
    UniqueFd descriptor{::open(
        path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600)};
    REQUIRE(descriptor.valid());
    constexpr off_t offset = static_cast<off_t>(UINT64_C(0x100000000));
    const char marker = 'Z';
    REQUIRE(::pwrite(descriptor.get(), &marker, 1, offset) == 1);
    REQUIRE(::fsync(descriptor.get()) == 0);

    auto identity = arena::platform::make_filesystem_identity();
    auto file = identity->admit_regular_file(path);
    std::array<std::byte, 1> byte{};
    REQUIRE(file->read_at(static_cast<std::uint64_t>(offset), byte) == 1);
    REQUIRE(byte[0] == std::byte{'Z'});
    REQUIRE(file->read_at(
                static_cast<std::uint64_t>(offset) + UINT64_C(1), byte) == 0);
}

TEST_CASE("Linux identity permits a normal file below a mount boundary") {
    const fs::path mounted_file{"/proc/version"};
    std::error_code error;
    if (!fs::is_regular_file(mounted_file, error) || error) {
        SUCCEED("a readable regular mounted file is unavailable");
        return;
    }

    auto identity = arena::platform::make_filesystem_identity();
    auto file = identity->admit_regular_file(mounted_file);
    REQUIRE(file->initial_observation().kind ==
            ports::FilesystemObjectKind::regular_file);
    REQUIRE(file->observe_path_binding() ==
            ports::PathBindingState::same_object);
}

TEST_CASE("Linux admitted descriptors are released by capability lifetime") {
    NativeTempDirectory workspace;
    const auto path = workspace.root / "payload.bin";
    write_file(path, "data");

    auto identity = arena::platform::make_filesystem_identity();
    const auto baseline = open_descriptor_count();
    for (int iteration = 0; iteration < 64; ++iteration) {
        auto file = identity->admit_regular_file(path);
        REQUIRE(file != nullptr);
    }
    const auto after = open_descriptor_count();

    REQUIRE(after <= baseline + 1U);
}

TEST_CASE("Linux admission failures release opened descriptors") {
    NativeTempDirectory workspace;
    const auto fifo = workspace.root / "failure-pipe";
    REQUIRE(::mkfifo(fifo.c_str(), 0600) == 0);

    auto identity = arena::platform::make_filesystem_identity();
    const auto baseline = open_descriptor_count();
    for (int iteration = 0; iteration < 64; ++iteration) {
        require_error_code(
            [&] { (void)identity->admit_regular_file(fifo); },
            ports::FilesystemIdentityErrorCode::special_object);
    }
    const auto after = open_descriptor_count();

    REQUIRE(after <= baseline + 1U);
}
