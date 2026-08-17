#include "arena/platform/filesystem_identity.hpp"

#include "arena/ports/filesystem_identity.hpp"

#if defined(ARENA_INSTALLER_TESTING)
#include "filesystem_identity_test_seam.hpp"
#endif

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <sodium.h>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(ARENA_INSTALLER_TESTING)
namespace arena::platform::linux_identity_testing {
namespace {
std::atomic<PreadFunction> g_pread_function{nullptr};
std::atomic<OpenAtFunction> g_openat_function{nullptr};
}

void set_pread_function(const PreadFunction function) noexcept {
    g_pread_function.store(function, std::memory_order_relaxed);
}

void reset_pread_function() noexcept {
    g_pread_function.store(nullptr, std::memory_order_relaxed);
}

[[nodiscard]] PreadFunction pread_function() noexcept {
    return g_pread_function.load(std::memory_order_relaxed);
}

void set_openat_function(const OpenAtFunction function) noexcept {
    g_openat_function.store(function, std::memory_order_relaxed);
}

void reset_openat_function() noexcept {
    g_openat_function.store(nullptr, std::memory_order_relaxed);
}

[[nodiscard]] OpenAtFunction openat_function() noexcept {
    return g_openat_function.load(std::memory_order_relaxed);
}

} // namespace arena::platform::linux_identity_testing
#endif

namespace {

namespace ports = arena::ports;

constexpr std::string_view kObjectIdentityDomain =
    "arena.filesystem.object.linux.v1";
constexpr std::string_view kMetadataIdentityDomain =
    "arena.filesystem.metadata.linux.v1";

class UniqueFd final {
  public:
    UniqueFd() noexcept = default;
    explicit UniqueFd(const int descriptor) noexcept : descriptor_(descriptor) {}

    ~UniqueFd() {
        reset();
    }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    UniqueFd(UniqueFd&& other) noexcept
        : descriptor_(std::exchange(other.descriptor_, -1)) {}

    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            reset();
            descriptor_ = std::exchange(other.descriptor_, -1);
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept {
        return descriptor_;
    }

    [[nodiscard]] bool valid() const noexcept {
        return descriptor_ >= 0;
    }

    void reset() noexcept {
        if (valid()) {
            // POSIX permits close() to have released the descriptor even when it
            // reports EINTR. Retrying could close an unrelated reused descriptor.
            (void)::close(descriptor_);
        }
        descriptor_ = -1;
    }

  private:
    int descriptor_{-1};
};

struct NativeIdentity final {
    std::uint64_t device{};
    std::uint64_t inode{};

    [[nodiscard]] bool operator==(const NativeIdentity&) const noexcept = default;
};

struct ChainEntry final {
    ports::FilesystemObjectKind kind{};
    NativeIdentity identity{};

    [[nodiscard]] bool operator==(const ChainEntry&) const noexcept = default;
};

struct AdmittedNativeObject final {
    std::filesystem::path path;
    UniqueFd descriptor;
    std::vector<ChainEntry> chain;
    struct stat status {};
};

[[nodiscard]] std::string error_message(
    const std::string_view category,
    const int code) {
    return std::string{category} + " (errno=" + std::to_string(code) + ")";
}

[[noreturn]] void throw_query_error(const int code) {
    if (code == EACCES || code == EPERM) {
        throw ports::FilesystemIdentityException(
            ports::FilesystemIdentityErrorCode::access_denied,
            error_message("filesystem identity query was denied", code));
    }
    throw ports::FilesystemIdentityException(
        ports::FilesystemIdentityErrorCode::identity_unavailable,
        error_message("filesystem identity is unavailable", code));
}

[[nodiscard]] bool stat_at_no_follow(
    const int parent_descriptor,
    const char* leaf,
    struct stat& status) noexcept {
    int result = -1;
    do {
        result = ::fstatat(
            parent_descriptor, leaf, &status, AT_SYMLINK_NOFOLLOW);
    } while (result != 0 && errno == EINTR);
    return result == 0;
}

[[noreturn]] void throw_open_error(
    const int parent_descriptor,
    const char* leaf,
    const int code,
    const ports::FilesystemObjectKind expected_kind) {
    if (code == ENOENT) {
        throw ports::FilesystemIdentityException(
            ports::FilesystemIdentityErrorCode::missing,
            error_message("filesystem object is missing", code));
    }
    if (code == EACCES || code == EPERM) {
        throw ports::FilesystemIdentityException(
            ports::FilesystemIdentityErrorCode::access_denied,
            error_message("filesystem object is not accessible", code));
    }
    if (code == ELOOP) {
        throw ports::FilesystemIdentityException(
            ports::FilesystemIdentityErrorCode::link_or_reparse,
            error_message("filesystem object is a symbolic link", code));
    }

    struct stat status {};
    if (stat_at_no_follow(parent_descriptor, leaf, status)) {
        if (S_ISLNK(status.st_mode)) {
            throw ports::FilesystemIdentityException(
                ports::FilesystemIdentityErrorCode::link_or_reparse,
                error_message("filesystem object is a symbolic link", code));
        }

        const bool expected_directory =
            expected_kind == ports::FilesystemObjectKind::directory;
        if ((expected_directory && S_ISREG(status.st_mode)) ||
            (!expected_directory && S_ISDIR(status.st_mode))) {
            throw ports::FilesystemIdentityException(
                ports::FilesystemIdentityErrorCode::wrong_kind,
                error_message("filesystem object has the wrong kind", code));
        }
        if (!S_ISREG(status.st_mode) && !S_ISDIR(status.st_mode)) {
            throw ports::FilesystemIdentityException(
                ports::FilesystemIdentityErrorCode::special_object,
                error_message("filesystem object is special", code));
        }
    }

    if (code == ENOTDIR || code == EISDIR) {
        throw ports::FilesystemIdentityException(
            ports::FilesystemIdentityErrorCode::wrong_kind,
            error_message("filesystem path has the wrong kind", code));
    }
    throw ports::FilesystemIdentityException(
        ports::FilesystemIdentityErrorCode::io_failure,
        error_message("filesystem object open failed", code));
}

template <typename Value>
[[nodiscard]] std::uint64_t stable_u64(
    const Value value,
    const std::string_view field) {
    static_assert(std::is_integral_v<Value>);
    using Unsigned = std::make_unsigned_t<Value>;

    if constexpr (std::is_signed_v<Value>) {
        if (value < 0) {
            throw ports::FilesystemIdentityException(
                ports::FilesystemIdentityErrorCode::identity_unavailable,
                std::string{"filesystem identity field is negative: "} +
                    std::string{field});
        }
    }

    const auto unsigned_value = static_cast<Unsigned>(value);
    if constexpr (sizeof(Unsigned) > sizeof(std::uint64_t)) {
        if (unsigned_value >
            static_cast<Unsigned>(std::numeric_limits<std::uint64_t>::max())) {
            throw ports::FilesystemIdentityException(
                ports::FilesystemIdentityErrorCode::identity_unavailable,
                std::string{"filesystem identity field is too wide: "} +
                    std::string{field});
        }
    }
    return static_cast<std::uint64_t>(unsigned_value);
}

template <typename Value>
[[nodiscard]] std::uint64_t signed_bits(
    const Value value,
    const std::string_view field) {
    static_assert(std::is_integral_v<Value>);
    if constexpr (sizeof(Value) > sizeof(std::int64_t)) {
        if (value < static_cast<Value>(std::numeric_limits<std::int64_t>::min()) ||
            value > static_cast<Value>(std::numeric_limits<std::int64_t>::max())) {
            throw ports::FilesystemIdentityException(
                ports::FilesystemIdentityErrorCode::identity_unavailable,
                std::string{"filesystem timestamp is too wide: "} +
                    std::string{field});
        }
    }
    return std::bit_cast<std::uint64_t>(static_cast<std::int64_t>(value));
}

void append_u64(std::vector<std::byte>& output, const std::uint64_t value) {
    for (unsigned int shift = 0; shift < 64; shift += 8) {
        output.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
    }
}

void append_bytes(
    std::vector<std::byte>& output,
    const std::span<const std::byte> bytes) {
    output.insert(output.end(), bytes.begin(), bytes.end());
}

void append_kind(
    std::vector<std::byte>& output,
    const ports::FilesystemObjectKind kind) {
    output.push_back(static_cast<std::byte>(
        kind == ports::FilesystemObjectKind::directory ? 1U : 0U));
}

[[nodiscard]] ports::FilesystemIdentityToken hash_identity(
    const std::string_view domain,
    const std::span<const std::byte> payload) {
    if (sodium_init() < 0) {
        throw ports::FilesystemIdentityException(
            ports::FilesystemIdentityErrorCode::identity_unavailable,
            "filesystem identity hashing is unavailable");
    }

    crypto_hash_sha256_state state{};
    if (crypto_hash_sha256_init(&state) != 0) {
        throw ports::FilesystemIdentityException(
            ports::FilesystemIdentityErrorCode::identity_unavailable,
            "filesystem identity hashing initialization failed");
    }
    if (crypto_hash_sha256_update(
            &state,
            reinterpret_cast<const unsigned char*>(domain.data()),
            static_cast<unsigned long long>(domain.size())) != 0) {
        throw ports::FilesystemIdentityException(
            ports::FilesystemIdentityErrorCode::identity_unavailable,
            "filesystem identity hashing failed");
    }
    constexpr unsigned char separator = 0;
    if (crypto_hash_sha256_update(&state, &separator, 1) != 0) {
        throw ports::FilesystemIdentityException(
            ports::FilesystemIdentityErrorCode::identity_unavailable,
            "filesystem identity hashing failed");
    }
    if (!payload.empty() &&
        crypto_hash_sha256_update(
            &state,
            reinterpret_cast<const unsigned char*>(payload.data()),
            static_cast<unsigned long long>(payload.size())) != 0) {
        throw ports::FilesystemIdentityException(
            ports::FilesystemIdentityErrorCode::identity_unavailable,
            "filesystem identity hashing failed");
    }

    ports::FilesystemIdentityToken result{};
    if (crypto_hash_sha256_final(
            &state,
            reinterpret_cast<unsigned char*>(result.data())) != 0) {
        throw ports::FilesystemIdentityException(
            ports::FilesystemIdentityErrorCode::identity_unavailable,
            "filesystem identity hashing finalization failed");
    }
    return result;
}

[[nodiscard]] ports::FilesystemObjectKind require_kind(
    const struct stat& status,
    const ports::FilesystemObjectKind expected_kind) {
    if (S_ISLNK(status.st_mode)) {
        throw ports::FilesystemIdentityException(
            ports::FilesystemIdentityErrorCode::link_or_reparse,
            "filesystem object is a symbolic link");
    }
    if (S_ISREG(status.st_mode)) {
        if (expected_kind != ports::FilesystemObjectKind::regular_file) {
            throw ports::FilesystemIdentityException(
                ports::FilesystemIdentityErrorCode::wrong_kind,
                "filesystem object has the wrong kind");
        }
        return ports::FilesystemObjectKind::regular_file;
    }
    if (S_ISDIR(status.st_mode)) {
        if (expected_kind != ports::FilesystemObjectKind::directory) {
            throw ports::FilesystemIdentityException(
                ports::FilesystemIdentityErrorCode::wrong_kind,
                "filesystem object has the wrong kind");
        }
        return ports::FilesystemObjectKind::directory;
    }
    throw ports::FilesystemIdentityException(
        ports::FilesystemIdentityErrorCode::special_object,
        "filesystem object is not a regular file or directory");
}

[[nodiscard]] struct stat query_status(
    const int descriptor,
    const ports::FilesystemObjectKind expected_kind) {
    struct stat status {};
    int result = -1;
    do {
        result = ::fstat(descriptor, &status);
    } while (result != 0 && errno == EINTR);
    if (result != 0) {
        throw_query_error(errno);
    }
    (void)require_kind(status, expected_kind);
    return status;
}

[[nodiscard]] NativeIdentity native_identity(const struct stat& status) {
    return NativeIdentity{
        stable_u64(status.st_dev, "device"),
        stable_u64(status.st_ino, "inode"),
    };
}

[[nodiscard]] ports::FilesystemObservation make_observation(
    const struct stat& status,
    const ports::FilesystemObjectKind kind) {
    const auto identity = native_identity(status);

    std::vector<std::byte> object_payload;
    object_payload.reserve(17);
    append_kind(object_payload, kind);
    append_u64(object_payload, identity.device);
    append_u64(object_payload, identity.inode);
    const auto object_identity =
        hash_identity(kObjectIdentityDomain, object_payload);

    const auto size = kind == ports::FilesystemObjectKind::directory
                          ? UINT64_C(0)
                          : stable_u64(status.st_size, "size");

    std::vector<std::byte> metadata_payload;
    metadata_payload.reserve(object_identity.size() + 57U);
    append_bytes(
        metadata_payload,
        std::span<const std::byte>{object_identity.data(), object_identity.size()});
    append_kind(metadata_payload, kind);
    append_u64(metadata_payload, size);
    append_u64(metadata_payload, stable_u64(status.st_mode, "mode"));
    append_u64(metadata_payload, stable_u64(status.st_nlink, "link_count"));
    append_u64(
        metadata_payload,
        signed_bits(status.st_mtim.tv_sec, "mtime_seconds"));
    append_u64(
        metadata_payload,
        stable_u64(status.st_mtim.tv_nsec, "mtime_nanoseconds"));
    append_u64(
        metadata_payload,
        signed_bits(status.st_ctim.tv_sec, "ctime_seconds"));
    append_u64(
        metadata_payload,
        stable_u64(status.st_ctim.tv_nsec, "ctime_nanoseconds"));

    return ports::FilesystemObservation{
        kind,
        size,
        object_identity,
        hash_identity(kMetadataIdentityDomain, metadata_payload),
    };
}

[[nodiscard]] int invoke_openat(
    const int parent_descriptor,
    const char* leaf,
    const int flags) {
#if defined(ARENA_INSTALLER_TESTING)
    if (const auto function =
            arena::platform::linux_identity_testing::openat_function()) {
        return function(parent_descriptor, leaf, flags);
    }
#endif
    return ::openat(parent_descriptor, leaf, flags);
}

[[nodiscard]] UniqueFd open_at(
    const int parent_descriptor,
    const char* leaf,
    const ports::FilesystemObjectKind expected_kind) {
    int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
    if (expected_kind == ports::FilesystemObjectKind::directory) {
        flags |= O_DIRECTORY;
    }

    int descriptor = -1;
    do {
        descriptor = invoke_openat(parent_descriptor, leaf, flags);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0) {
        throw_open_error(
            parent_descriptor, leaf, errno, expected_kind);
    }
    return UniqueFd{descriptor};
}

[[nodiscard]] UniqueFd open_root() {
    return open_at(
        AT_FDCWD, "/", ports::FilesystemObjectKind::directory);
}

[[nodiscard]] std::vector<std::filesystem::path> path_components(
    const std::filesystem::path& absolute_path) {
    std::vector<std::filesystem::path> result;
    for (const auto& component : absolute_path.relative_path()) {
        if (!component.empty()) {
            result.push_back(component);
        }
    }
    return result;
}

[[nodiscard]] std::filesystem::path path_from_utf8(
    const std::string_view value) {
    std::u8string encoded;
    encoded.reserve(value.size());
    for (const unsigned char byte : value) {
        encoded.push_back(static_cast<char8_t>(byte));
    }
    return std::filesystem::path{encoded};
}

[[nodiscard]] AdmittedNativeObject admit_native_object(
    const std::filesystem::path& absolute_path,
    const ports::FilesystemObjectKind expected_kind) {
    const auto components = path_components(absolute_path);

    AdmittedNativeObject result{};
    result.path = absolute_path;
    result.chain.reserve(components.size() + 1U);

    auto current = open_root();
    const auto root_status = query_status(
        current.get(), ports::FilesystemObjectKind::directory);
    result.chain.push_back(ChainEntry{
        ports::FilesystemObjectKind::directory,
        native_identity(root_status),
    });

    if (components.empty()) {
        if (expected_kind != ports::FilesystemObjectKind::directory) {
            throw ports::FilesystemIdentityException(
                ports::FilesystemIdentityErrorCode::wrong_kind,
                "filesystem root cannot satisfy regular-file admission");
        }
        result.status = root_status;
        result.descriptor = std::move(current);
        return result;
    }

    for (std::size_t index = 0; index < components.size(); ++index) {
        const bool final = index + 1U == components.size();
        const auto kind = final
                              ? expected_kind
                              : ports::FilesystemObjectKind::directory;
        const auto native_leaf = components[index].native();
        auto next = open_at(current.get(), native_leaf.c_str(), kind);
        const auto status = query_status(next.get(), kind);
        result.chain.push_back(ChainEntry{kind, native_identity(status)});
        current = std::move(next);
        if (final) {
            result.status = status;
        }
    }

    result.descriptor = std::move(current);
    return result;
}

[[nodiscard]] ports::PathBindingState observe_chain_binding(
    const std::filesystem::path& absolute_path,
    const std::vector<ChainEntry>& expected_chain) noexcept {
    try {
        const auto components = path_components(absolute_path);
        if (expected_chain.size() != components.size() + 1U) {
            return ports::PathBindingState::different_object;
        }

        auto current = open_root();
        const auto root_status = query_status(
            current.get(), ports::FilesystemObjectKind::directory);
        if (!(ChainEntry{
                  ports::FilesystemObjectKind::directory,
                  native_identity(root_status)} == expected_chain.front())) {
            return ports::PathBindingState::different_object;
        }

        for (std::size_t index = 0; index < components.size(); ++index) {
            const auto& expected = expected_chain[index + 1U];
            const auto native_leaf = components[index].native();
            auto next = open_at(
                current.get(), native_leaf.c_str(), expected.kind);
            const auto status = query_status(next.get(), expected.kind);
            if (!(ChainEntry{expected.kind, native_identity(status)} == expected)) {
                return ports::PathBindingState::different_object;
            }
            current = std::move(next);
        }
        return ports::PathBindingState::same_object;
    } catch (const ports::FilesystemIdentityException& error) {
        switch (error.code()) {
        case ports::FilesystemIdentityErrorCode::missing:
            return ports::PathBindingState::missing;
        case ports::FilesystemIdentityErrorCode::wrong_kind:
        case ports::FilesystemIdentityErrorCode::link_or_reparse:
        case ports::FilesystemIdentityErrorCode::special_object:
        case ports::FilesystemIdentityErrorCode::identity_changed:
            return ports::PathBindingState::different_object;
        case ports::FilesystemIdentityErrorCode::invalid_path:
        case ports::FilesystemIdentityErrorCode::access_denied:
        case ports::FilesystemIdentityErrorCode::identity_unavailable:
        case ports::FilesystemIdentityErrorCode::io_failure:
            return ports::PathBindingState::uninspectable;
        }
    } catch (...) {
        return ports::PathBindingState::uninspectable;
    }
    return ports::PathBindingState::uninspectable;
}

void require_stable_admission(const AdmittedNativeObject& object) {
    switch (observe_chain_binding(object.path, object.chain)) {
    case ports::PathBindingState::same_object:
        return;
    case ports::PathBindingState::missing:
        throw ports::FilesystemIdentityException(
            ports::FilesystemIdentityErrorCode::missing,
            "filesystem object disappeared during admission");
    case ports::PathBindingState::different_object:
        throw ports::FilesystemIdentityException(
            ports::FilesystemIdentityErrorCode::identity_changed,
            "filesystem identity changed during admission");
    case ports::PathBindingState::uninspectable:
        throw ports::FilesystemIdentityException(
            ports::FilesystemIdentityErrorCode::identity_unavailable,
            "filesystem identity could not be revalidated");
    }
}

[[nodiscard]] ssize_t invoke_pread(
    const int descriptor,
    void* destination,
    const std::size_t size,
    const off_t offset) {
#if defined(ARENA_INSTALLER_TESTING)
    if (const auto function =
            arena::platform::linux_identity_testing::pread_function()) {
        return function(descriptor, destination, size, offset);
    }
#endif
    return ::pread(descriptor, destination, size, offset);
}

[[nodiscard]] std::size_t read_at_descriptor(
    const int descriptor,
    const std::uint64_t offset,
    const std::span<std::byte> destination) {
    if (destination.empty()) {
        return 0;
    }
    if (destination.size() >
            static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()) ||
        offset > static_cast<std::uint64_t>(
                     std::numeric_limits<off_t>::max())) {
        throw ports::FilesystemIdentityException(
            ports::FilesystemIdentityErrorCode::io_failure,
            "filesystem read request is outside supported bounds");
    }

    ssize_t transferred = -1;
    do {
        transferred = invoke_pread(
            descriptor,
            destination.data(),
            destination.size(),
            static_cast<off_t>(offset));
    } while (transferred < 0 && errno == EINTR);

    if (transferred < 0) {
        const int code = errno;
        if (code == EACCES || code == EPERM) {
            throw ports::FilesystemIdentityException(
                ports::FilesystemIdentityErrorCode::access_denied,
                error_message("filesystem descriptor read was denied", code));
        }
        throw ports::FilesystemIdentityException(
            ports::FilesystemIdentityErrorCode::io_failure,
            error_message("filesystem descriptor read failed", code));
    }
    return static_cast<std::size_t>(transferred);
}

class LinuxRegularFile final : public ports::AdmittedRegularFile {
  public:
    LinuxRegularFile(
        std::filesystem::path path,
        UniqueFd descriptor,
        std::vector<ChainEntry> chain,
        ports::FilesystemObservation initial)
        : path_(std::move(path)),
          descriptor_(std::move(descriptor)),
          chain_(std::move(chain)),
          initial_(std::move(initial)) {}

    [[nodiscard]] const std::filesystem::path&
    absolute_path() const noexcept override {
        return path_;
    }

    [[nodiscard]] const ports::FilesystemObservation&
    initial_observation() const noexcept override {
        return initial_;
    }

    [[nodiscard]] std::size_t read_at(
        const std::uint64_t offset,
        const std::span<std::byte> destination) override {
        return read_at_descriptor(descriptor_.get(), offset, destination);
    }

    [[nodiscard]] ports::FilesystemObservation
    observe_open_object() const override {
        return make_observation(
            query_status(
                descriptor_.get(),
                ports::FilesystemObjectKind::regular_file),
            ports::FilesystemObjectKind::regular_file);
    }

    [[nodiscard]] ports::PathBindingState
    observe_path_binding() const override {
        return observe_chain_binding(path_, chain_);
    }

  private:
    std::filesystem::path path_;
    UniqueFd descriptor_;
    std::vector<ChainEntry> chain_;
    ports::FilesystemObservation initial_;
};

class LinuxDirectory final : public ports::AdmittedDirectory {
  public:
    LinuxDirectory(
        std::filesystem::path path,
        UniqueFd descriptor,
        std::vector<ChainEntry> chain,
        ports::FilesystemObservation initial)
        : ports::AdmittedDirectory(path, initial),
          descriptor_(std::move(descriptor)),
          chain_(std::move(chain)) {}

    [[nodiscard]] ports::FilesystemObservation
    observe_open_object() const override {
        return make_observation(
            query_status(
                descriptor_.get(),
                ports::FilesystemObjectKind::directory),
            ports::FilesystemObjectKind::directory);
    }

    [[nodiscard]] ports::PathBindingState
    observe_path_binding() const override {
        return observe_chain_binding(absolute_path(), chain_);
    }

  protected:
    [[nodiscard]] std::unique_ptr<ports::AdmittedRegularFile>
    open_regular_file_validated(const std::string_view leaf) const override {
        auto child = admit_child(
            leaf, ports::FilesystemObjectKind::regular_file);
        const auto initial = make_observation(
            child.status, ports::FilesystemObjectKind::regular_file);
        return std::make_unique<LinuxRegularFile>(
            std::move(child.path),
            std::move(child.descriptor),
            std::move(child.chain),
            initial);
    }

    [[nodiscard]] std::unique_ptr<ports::AdmittedDirectory>
    open_directory_validated(const std::string_view leaf) const override {
        auto child = admit_child(
            leaf, ports::FilesystemObjectKind::directory);
        const auto initial = make_observation(
            child.status, ports::FilesystemObjectKind::directory);
        return std::make_unique<LinuxDirectory>(
            std::move(child.path),
            std::move(child.descriptor),
            std::move(child.chain),
            initial);
    }

  private:
    [[nodiscard]] AdmittedNativeObject admit_child(
        const std::string_view leaf,
        const ports::FilesystemObjectKind kind) const {
        if (observe_path_binding() != ports::PathBindingState::same_object) {
            throw ports::FilesystemIdentityException(
                ports::FilesystemIdentityErrorCode::identity_changed,
                "filesystem parent binding changed before child admission");
        }

        const auto leaf_path = path_from_utf8(leaf);
        const auto native_leaf = leaf_path.native();
        auto descriptor = open_at(
            descriptor_.get(), native_leaf.c_str(), kind);
        const auto status = query_status(descriptor.get(), kind);

        AdmittedNativeObject child{};
        child.path = absolute_path() / leaf_path;
        child.descriptor = std::move(descriptor);
        child.chain = chain_;
        child.chain.push_back(ChainEntry{kind, native_identity(status)});
        child.status = status;
        require_stable_admission(child);
        return child;
    }

    UniqueFd descriptor_;
    std::vector<ChainEntry> chain_;
};

class LinuxFilesystemIdentity final : public ports::FilesystemIdentity {
  protected:
    [[nodiscard]] std::unique_ptr<ports::AdmittedRegularFile>
    admit_regular_file_validated(
        const std::filesystem::path& absolute_path) override {
        auto object = admit_native_object(
            absolute_path, ports::FilesystemObjectKind::regular_file);
        require_stable_admission(object);
        const auto initial = make_observation(
            object.status, ports::FilesystemObjectKind::regular_file);
        return std::make_unique<LinuxRegularFile>(
            std::move(object.path),
            std::move(object.descriptor),
            std::move(object.chain),
            initial);
    }

    [[nodiscard]] std::unique_ptr<ports::AdmittedDirectory>
    admit_directory_validated(
        const std::filesystem::path& absolute_path) override {
        auto object = admit_native_object(
            absolute_path, ports::FilesystemObjectKind::directory);
        require_stable_admission(object);
        const auto initial = make_observation(
            object.status, ports::FilesystemObjectKind::directory);
        return std::make_unique<LinuxDirectory>(
            std::move(object.path),
            std::move(object.descriptor),
            std::move(object.chain),
            initial);
    }
};

} // namespace

namespace arena::platform {

std::unique_ptr<arena::ports::FilesystemIdentity>
make_filesystem_identity() {
    return std::make_unique<LinuxFilesystemIdentity>();
}

} // namespace arena::platform
