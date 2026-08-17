#include "arena/platform/filesystem_identity.hpp"

#include "arena/platform/filesystem_path.hpp"
#include "arena/ports/filesystem_identity.hpp"

#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <sodium.h>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace ports = arena::ports;

constexpr std::string_view kObjectIdentityDomain =
    "arena.filesystem.object.windows.v1";
constexpr std::string_view kMetadataIdentityDomain =
    "arena.filesystem.metadata.windows.v1";

class UniqueHandle final {
  public:
    UniqueHandle() noexcept = default;
    explicit UniqueHandle(const HANDLE handle) noexcept : handle_(handle) {}

    ~UniqueHandle() {
        reset();
    }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept
        : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)) {}

    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept {
        return handle_;
    }

    [[nodiscard]] bool valid() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

    void reset() noexcept {
        if (valid()) {
            (void)CloseHandle(handle_);
        }
        handle_ = INVALID_HANDLE_VALUE;
    }

  private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
};

struct NativeIdentity final {
    std::uint64_t volume_serial{};
    std::array<std::byte, 16> file_id{};

    [[nodiscard]] bool operator==(const NativeIdentity&) const noexcept = default;
};

struct ChainEntry final {
    ports::FilesystemObjectKind kind{};
    NativeIdentity identity{};

    [[nodiscard]] bool operator==(const ChainEntry&) const noexcept = default;
};

struct NativeObservation final {
    FILE_ID_INFO file_id{};
    FILE_BASIC_INFO basic{};
    FILE_STANDARD_INFO standard{};
};

struct AdmittedNativeObject final {
    std::filesystem::path path;
    UniqueHandle handle;
    std::vector<ChainEntry> chain;
    NativeObservation native_observation;
};

[[nodiscard]] std::string error_message(
    const std::string_view category,
    const DWORD code) {
    return std::string{category} + " (win32=" + std::to_string(code) + ")";
}

[[noreturn]] void throw_open_error(const DWORD code) {
    switch (code) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME:
        throw ports::FilesystemIdentityException(
            ports::FilesystemIdentityErrorCode::missing,
            error_message("filesystem object is missing", code));
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
        throw ports::FilesystemIdentityException(
            ports::FilesystemIdentityErrorCode::access_denied,
            error_message("filesystem object is not accessible", code));
    default:
        throw ports::FilesystemIdentityException(
            ports::FilesystemIdentityErrorCode::io_failure,
            error_message("filesystem object open failed", code));
    }
}

[[noreturn]] void throw_query_error(const DWORD code) {
    if (code == ERROR_ACCESS_DENIED) {
        throw ports::FilesystemIdentityException(
            ports::FilesystemIdentityErrorCode::access_denied,
            error_message("filesystem identity query was denied", code));
    }
    throw ports::FilesystemIdentityException(
        ports::FilesystemIdentityErrorCode::identity_unavailable,
        error_message("filesystem identity is unavailable", code));
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

[[nodiscard]] std::filesystem::path path_from_utf8(
    const std::string_view value) {
    std::u8string encoded;
    encoded.reserve(value.size());
    for (const unsigned char byte : value) {
        encoded.push_back(static_cast<char8_t>(byte));
    }
    return std::filesystem::path{encoded};
}

[[nodiscard]] std::vector<std::filesystem::path> component_paths(
    const std::filesystem::path& absolute_path) {
    std::vector<std::filesystem::path> result;
    const auto root = absolute_path.root_path();
    if (root.empty()) {
        return result;
    }

    auto current = root;
    result.push_back(current);
    for (const auto& component : absolute_path.relative_path()) {
        if (component.empty()) {
            continue;
        }
        current /= component;
        result.push_back(current);
    }
    return result;
}

[[nodiscard]] UniqueHandle open_component(
    const std::filesystem::path& path,
    const ports::FilesystemObjectKind expected_kind,
    const bool retain_for_read) {
    DWORD access = FILE_READ_ATTRIBUTES | SYNCHRONIZE;
    DWORD flags = FILE_FLAG_OPEN_REPARSE_POINT;
    if (expected_kind == ports::FilesystemObjectKind::directory) {
        flags |= FILE_FLAG_BACKUP_SEMANTICS;
    } else {
        access |= FILE_READ_DATA;
        if (retain_for_read) {
            flags |= FILE_FLAG_OVERLAPPED;
        }
    }

    const auto api_path = arena::platform::filesystem_api_path(path);
    UniqueHandle handle{CreateFileW(
        api_path.c_str(),
        access,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        flags,
        nullptr)};
    if (!handle.valid()) {
        throw_open_error(GetLastError());
    }
    return handle;
}

[[nodiscard]] NativeObservation query_native_observation(
    const HANDLE handle,
    const ports::FilesystemObjectKind expected_kind) {
    SetLastError(ERROR_SUCCESS);
    const DWORD file_type = GetFileType(handle);
    if (file_type != FILE_TYPE_DISK) {
        const DWORD error = GetLastError();
        if (file_type == FILE_TYPE_UNKNOWN && error != ERROR_SUCCESS) {
            throw_query_error(error);
        }
        throw ports::FilesystemIdentityException(
            ports::FilesystemIdentityErrorCode::special_object,
            "filesystem object is not an ordinary disk object");
    }

    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (!GetFileInformationByHandleEx(
            handle, FileAttributeTagInfo, &tag, sizeof(tag))) {
        throw_query_error(GetLastError());
    }
    if ((tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ||
        tag.ReparseTag != 0U) {
        throw ports::FilesystemIdentityException(
            ports::FilesystemIdentityErrorCode::link_or_reparse,
            "filesystem object is a reparse point");
    }

    NativeObservation result{};
    if (!GetFileInformationByHandleEx(
            handle, FileIdInfo, &result.file_id, sizeof(result.file_id)) ||
        !GetFileInformationByHandleEx(
            handle, FileBasicInfo, &result.basic, sizeof(result.basic)) ||
        !GetFileInformationByHandleEx(
            handle, FileStandardInfo, &result.standard,
            sizeof(result.standard))) {
        throw_query_error(GetLastError());
    }

    const bool directory_attribute =
        (result.basic.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U;
    const bool directory_standard = result.standard.Directory != FALSE;
    if (directory_attribute != directory_standard) {
        throw ports::FilesystemIdentityException(
            ports::FilesystemIdentityErrorCode::identity_unavailable,
            "filesystem kind observations are inconsistent");
    }

    const bool expected_directory =
        expected_kind == ports::FilesystemObjectKind::directory;
    if (directory_attribute != expected_directory) {
        throw ports::FilesystemIdentityException(
            ports::FilesystemIdentityErrorCode::wrong_kind,
            "filesystem object has the wrong kind");
    }
    if (!expected_directory && result.standard.EndOfFile.QuadPart < 0) {
        throw ports::FilesystemIdentityException(
            ports::FilesystemIdentityErrorCode::identity_unavailable,
            "filesystem size is not representable");
    }
    return result;
}

[[nodiscard]] NativeIdentity native_identity(
    const NativeObservation& observation) {
    NativeIdentity result{};
    result.volume_serial = observation.file_id.VolumeSerialNumber;
    std::memcpy(
        result.file_id.data(),
        observation.file_id.FileId.Identifier,
        result.file_id.size());
    return result;
}

[[nodiscard]] ports::FilesystemObservation make_observation(
    const NativeObservation& native,
    const ports::FilesystemObjectKind kind) {
    std::vector<std::byte> object_payload;
    object_payload.reserve(sizeof(std::uint64_t) + 16U + 1U);
    append_u64(object_payload, native.file_id.VolumeSerialNumber);
    append_bytes(
        object_payload,
        std::span<const std::byte>{
            reinterpret_cast<const std::byte*>(native.file_id.FileId.Identifier),
            sizeof(native.file_id.FileId.Identifier)});
    append_kind(object_payload, kind);
    const auto object = hash_identity(kObjectIdentityDomain, object_payload);

    const auto size = kind == ports::FilesystemObjectKind::directory
                          ? UINT64_C(0)
                          : static_cast<std::uint64_t>(
                                native.standard.EndOfFile.QuadPart);

    std::vector<std::byte> metadata_payload;
    metadata_payload.reserve(object.size() + 8U * 6U + 2U);
    append_bytes(metadata_payload, object);
    append_kind(metadata_payload, kind);
    append_u64(metadata_payload, size);
    append_u64(metadata_payload,
               static_cast<std::uint64_t>(native.basic.CreationTime.QuadPart));
    append_u64(metadata_payload,
               static_cast<std::uint64_t>(native.basic.ChangeTime.QuadPart));
    append_u64(metadata_payload,
               static_cast<std::uint64_t>(native.basic.LastWriteTime.QuadPart));
    append_u64(metadata_payload,
               static_cast<std::uint64_t>(native.basic.FileAttributes));
    append_u64(metadata_payload,
               static_cast<std::uint64_t>(native.standard.NumberOfLinks));
    append_kind(metadata_payload,
                native.standard.Directory != FALSE
                    ? ports::FilesystemObjectKind::directory
                    : ports::FilesystemObjectKind::regular_file);

    return ports::FilesystemObservation{
        kind,
        size,
        object,
        hash_identity(kMetadataIdentityDomain, metadata_payload),
    };
}

[[nodiscard]] AdmittedNativeObject admit_native_object(
    const std::filesystem::path& absolute_path,
    const ports::FilesystemObjectKind expected_kind) {
    const auto paths = component_paths(absolute_path);
    if (paths.empty()) {
        throw ports::FilesystemIdentityException(
            ports::FilesystemIdentityErrorCode::identity_unavailable,
            "filesystem path has no physical components");
    }

    AdmittedNativeObject result{};
    result.path = absolute_path;
    result.chain.reserve(paths.size());

    for (std::size_t index = 0; index < paths.size(); ++index) {
        const bool final = index + 1U == paths.size();
        const auto kind = final ? expected_kind
                                : ports::FilesystemObjectKind::directory;
        auto handle = open_component(paths[index], kind,
                                     final && kind ==
                                         ports::FilesystemObjectKind::regular_file);
        const auto native = query_native_observation(handle.get(), kind);
        result.chain.push_back(ChainEntry{kind, native_identity(native)});
        if (final) {
            result.handle = std::move(handle);
            result.native_observation = native;
        }
    }
    return result;
}

[[nodiscard]] ports::PathBindingState observe_chain_binding(
    const std::filesystem::path& absolute_path,
    const std::vector<ChainEntry>& expected_chain) noexcept {
    try {
        const auto paths = component_paths(absolute_path);
        if (paths.size() != expected_chain.size() || paths.empty()) {
            return ports::PathBindingState::different_object;
        }
        for (std::size_t index = 0; index < paths.size(); ++index) {
            const auto& expected = expected_chain[index];
            auto handle = open_component(paths[index], expected.kind, false);
            const auto observed =
                query_native_observation(handle.get(), expected.kind);
            if (!(ChainEntry{expected.kind, native_identity(observed)} ==
                  expected)) {
                return ports::PathBindingState::different_object;
            }
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
    const auto binding = observe_chain_binding(object.path, object.chain);
    if (binding == ports::PathBindingState::same_object) {
        return;
    }
    if (binding == ports::PathBindingState::missing) {
        throw ports::FilesystemIdentityException(
            ports::FilesystemIdentityErrorCode::missing,
            "filesystem object disappeared during admission");
    }
    if (binding == ports::PathBindingState::different_object) {
        throw ports::FilesystemIdentityException(
            ports::FilesystemIdentityErrorCode::identity_changed,
            "filesystem identity changed during admission");
    }
    throw ports::FilesystemIdentityException(
        ports::FilesystemIdentityErrorCode::identity_unavailable,
        "filesystem identity could not be revalidated");
}

[[nodiscard]] std::size_t read_at_handle(
    const HANDLE handle,
    const std::uint64_t offset,
    const std::span<std::byte> destination) {
    if (destination.empty()) {
        return 0;
    }
    if (destination.size() > std::numeric_limits<DWORD>::max() ||
        offset > static_cast<std::uint64_t>(
                     std::numeric_limits<std::int64_t>::max())) {
        throw ports::FilesystemIdentityException(
            ports::FilesystemIdentityErrorCode::io_failure,
            "filesystem read request is outside supported bounds");
    }

    UniqueHandle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!event.valid()) {
        throw ports::FilesystemIdentityException(
            ports::FilesystemIdentityErrorCode::io_failure,
            error_message("filesystem read event creation failed", GetLastError()));
    }

    OVERLAPPED overlapped{};
    overlapped.Offset = static_cast<DWORD>(offset & UINT64_C(0xffffffff));
    overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32U);
    overlapped.hEvent = event.get();

    DWORD transferred = 0;
    if (!ReadFile(
            handle,
            destination.data(),
            static_cast<DWORD>(destination.size()),
            &transferred,
            &overlapped)) {
        const DWORD initial_error = GetLastError();
        if (initial_error == ERROR_HANDLE_EOF) {
            return 0;
        }
        if (initial_error != ERROR_IO_PENDING ||
            !GetOverlappedResult(handle, &overlapped, &transferred, TRUE)) {
            const DWORD final_error = initial_error == ERROR_IO_PENDING
                                          ? GetLastError()
                                          : initial_error;
            if (final_error == ERROR_HANDLE_EOF) {
                return 0;
            }
            if (final_error == ERROR_ACCESS_DENIED) {
                throw ports::FilesystemIdentityException(
                    ports::FilesystemIdentityErrorCode::access_denied,
                    error_message("filesystem descriptor read was denied",
                                  final_error));
            }
            throw ports::FilesystemIdentityException(
                ports::FilesystemIdentityErrorCode::io_failure,
                error_message("filesystem descriptor read failed", final_error));
        }
    }
    return transferred;
}

class WindowsRegularFile final : public ports::AdmittedRegularFile {
  public:
    WindowsRegularFile(
        std::filesystem::path path,
        UniqueHandle handle,
        std::vector<ChainEntry> chain,
        ports::FilesystemObservation initial)
        : path_(std::move(path)),
          handle_(std::move(handle)),
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
        return read_at_handle(handle_.get(), offset, destination);
    }

    [[nodiscard]] ports::FilesystemObservation
    observe_open_object() const override {
        return make_observation(
            query_native_observation(
                handle_.get(), ports::FilesystemObjectKind::regular_file),
            ports::FilesystemObjectKind::regular_file);
    }

    [[nodiscard]] ports::PathBindingState
    observe_path_binding() const override {
        return observe_chain_binding(path_, chain_);
    }

  private:
    std::filesystem::path path_;
    UniqueHandle handle_;
    std::vector<ChainEntry> chain_;
    ports::FilesystemObservation initial_;
};

class WindowsDirectory final : public ports::AdmittedDirectory {
  public:
    WindowsDirectory(
        std::filesystem::path path,
        UniqueHandle handle,
        std::vector<ChainEntry> chain,
        ports::FilesystemObservation initial)
        : ports::AdmittedDirectory(path, initial),
          handle_(std::move(handle)),
          chain_(std::move(chain)) {}

    [[nodiscard]] ports::FilesystemObservation
    observe_open_object() const override {
        return make_observation(
            query_native_observation(
                handle_.get(), ports::FilesystemObjectKind::directory),
            ports::FilesystemObjectKind::directory);
    }

    [[nodiscard]] ports::PathBindingState
    observe_path_binding() const override {
        return observe_chain_binding(absolute_path(), chain_);
    }

  protected:
    [[nodiscard]] std::unique_ptr<ports::AdmittedRegularFile>
    open_regular_file_validated(const std::string_view leaf) const override {
        auto child = admit_child(leaf, ports::FilesystemObjectKind::regular_file);
        const auto initial = make_observation(
            child.native_observation,
            ports::FilesystemObjectKind::regular_file);
        return std::make_unique<WindowsRegularFile>(
            std::move(child.path),
            std::move(child.handle),
            std::move(child.chain),
            initial);
    }

    [[nodiscard]] std::unique_ptr<ports::AdmittedDirectory>
    open_directory_validated(const std::string_view leaf) const override {
        auto child = admit_child(leaf, ports::FilesystemObjectKind::directory);
        const auto initial = make_observation(
            child.native_observation,
            ports::FilesystemObjectKind::directory);
        return std::make_unique<WindowsDirectory>(
            std::move(child.path),
            std::move(child.handle),
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

        auto child = admit_native_object(
            absolute_path() / path_from_utf8(leaf), kind);
        if (child.chain.size() != chain_.size() + 1U ||
            !std::equal(chain_.begin(), chain_.end(), child.chain.begin())) {
            throw ports::FilesystemIdentityException(
                ports::FilesystemIdentityErrorCode::identity_changed,
                "filesystem parent identity changed during child admission");
        }
        require_stable_admission(child);
        return child;
    }

    UniqueHandle handle_;
    std::vector<ChainEntry> chain_;
};

class WindowsFilesystemIdentity final : public ports::FilesystemIdentity {
  protected:
    [[nodiscard]] std::unique_ptr<ports::AdmittedRegularFile>
    admit_regular_file_validated(
        const std::filesystem::path& absolute_path) override {
        auto object = admit_native_object(
            absolute_path, ports::FilesystemObjectKind::regular_file);
        require_stable_admission(object);
        const auto initial = make_observation(
            object.native_observation,
            ports::FilesystemObjectKind::regular_file);
        return std::make_unique<WindowsRegularFile>(
            std::move(object.path),
            std::move(object.handle),
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
            object.native_observation,
            ports::FilesystemObjectKind::directory);
        return std::make_unique<WindowsDirectory>(
            std::move(object.path),
            std::move(object.handle),
            std::move(object.chain),
            initial);
    }
};

} // namespace

namespace arena::platform {

std::unique_ptr<arena::ports::FilesystemIdentity>
make_filesystem_identity() {
    return std::make_unique<WindowsFilesystemIdentity>();
}

} // namespace arena::platform
