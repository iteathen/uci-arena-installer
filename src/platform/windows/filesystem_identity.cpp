#include "arena/platform/filesystem_identity.hpp"

#include "arena/platform/filesystem_path.hpp"
#include "arena/ports/filesystem_identity.hpp"
#include "filesystem_identity_private.hpp"

#define NOMINMAX
#include <windows.h>

#include <sodium.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace arena::platform {
namespace {

namespace fs = std::filesystem;
namespace ports = arena::ports;

class UniqueHandle final {
  public:
    UniqueHandle() noexcept = default;
    explicit UniqueHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~UniqueHandle() {
        if (valid()) CloseHandle(handle_);
    }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept
        : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)) {}

    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
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

struct NativeIdentity {
    std::uint64_t volume_serial{};
    std::array<std::byte, 16> file_id{};
    ports::FilesystemObjectKind kind{};

    bool operator==(const NativeIdentity&) const = default;
};

struct NativeObservation {
    NativeIdentity identity;
    std::uint64_t size_bytes{};
    std::int64_t creation_time{};
    std::int64_t change_time{};
    std::int64_t last_write_time{};
    std::uint32_t attributes{};
    std::uint32_t link_count{};
};

struct OpenedPath {
    UniqueHandle final_handle;
    std::vector<NativeIdentity> chain;
    NativeObservation native;
    ports::FilesystemObservation observation;
};

[[noreturn]] void throw_identity_error(
    const ports::FilesystemIdentityErrorCode code,
    const std::string_view operation,
    const DWORD native_code = ERROR_SUCCESS) {
    std::string message{operation};
    if (native_code != ERROR_SUCCESS) {
        message += " failed (win32=" + std::to_string(native_code) + ")";
    }
    throw ports::FilesystemIdentityException(code, std::move(message));
}

[[noreturn]] void throw_native_error(const std::string_view operation,
                                     const DWORD native_code) {
    switch (native_code) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME:
        throw_identity_error(ports::FilesystemIdentityErrorCode::missing,
                             operation, native_code);
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
    case ERROR_PRIVILEGE_NOT_HELD:
        throw_identity_error(ports::FilesystemIdentityErrorCode::access_denied,
                             operation, native_code);
    default:
        throw_identity_error(ports::FilesystemIdentityErrorCode::io_failure,
                             operation, native_code);
    }
}

void append_u32(std::vector<std::byte>& bytes, const std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
    }
}

void append_u64(std::vector<std::byte>& bytes, const std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
    }
}

void append_text(std::vector<std::byte>& bytes, const std::string_view text) {
    bytes.reserve(bytes.size() + text.size());
    for (const unsigned char character : text) {
        bytes.push_back(static_cast<std::byte>(character));
    }
}

ports::FilesystemIdentityToken sha256(const std::vector<std::byte>& bytes) {
    if (sodium_init() < 0) {
        throw_identity_error(ports::FilesystemIdentityErrorCode::io_failure,
                             "filesystem identity hashing");
    }
    ports::FilesystemIdentityToken digest{};
    if (crypto_hash_sha256(
            reinterpret_cast<unsigned char*>(digest.data()),
            reinterpret_cast<const unsigned char*>(bytes.data()),
            static_cast<unsigned long long>(bytes.size())) != 0) {
        throw_identity_error(ports::FilesystemIdentityErrorCode::io_failure,
                             "filesystem identity hashing");
    }
    return digest;
}

ports::FilesystemIdentityToken object_identity(
    const NativeIdentity& identity) {
    std::vector<std::byte> bytes;
    append_text(bytes, "arena.filesystem.object.windows.v1");
    append_u64(bytes, identity.volume_serial);
    bytes.insert(bytes.end(), identity.file_id.begin(), identity.file_id.end());
    bytes.push_back(identity.kind == ports::FilesystemObjectKind::regular_file
                        ? std::byte{1}
                        : std::byte{2});
    return sha256(bytes);
}

ports::FilesystemIdentityToken metadata_identity(
    const NativeObservation& native,
    const ports::FilesystemIdentityToken& object) {
    std::vector<std::byte> bytes;
    append_text(bytes, "arena.filesystem.metadata.windows.v1");
    bytes.insert(bytes.end(), object.begin(), object.end());
    bytes.push_back(native.identity.kind ==
                            ports::FilesystemObjectKind::regular_file
                        ? std::byte{1}
                        : std::byte{2});
    append_u64(bytes, native.size_bytes);
    append_u64(bytes, static_cast<std::uint64_t>(native.creation_time));
    append_u64(bytes, static_cast<std::uint64_t>(native.change_time));
    append_u64(bytes, static_cast<std::uint64_t>(native.last_write_time));
    append_u32(bytes, native.attributes);
    append_u32(bytes, native.link_count);
    bytes.push_back(native.identity.kind == ports::FilesystemObjectKind::directory
                        ? std::byte{1}
                        : std::byte{0});
    return sha256(bytes);
}

NativeObservation query_native(HANDLE handle) {
    FILE_ATTRIBUTE_TAG_INFO attribute_tag{};
    if (!GetFileInformationByHandleEx(
            handle, FileAttributeTagInfo, &attribute_tag,
            static_cast<DWORD>(sizeof(attribute_tag)))) {
        throw_native_error("filesystem attribute query", GetLastError());
    }
    if ((attribute_tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        throw_identity_error(
            ports::FilesystemIdentityErrorCode::link_or_reparse,
            "filesystem component is a reparse point");
    }
    if ((attribute_tag.FileAttributes & FILE_ATTRIBUTE_DEVICE) != 0) {
        throw_identity_error(ports::FilesystemIdentityErrorCode::special_object,
                             "filesystem component is a device");
    }

    SetLastError(ERROR_SUCCESS);
    const DWORD file_type = GetFileType(handle);
    const DWORD file_type_error = GetLastError();
    if (file_type != FILE_TYPE_DISK) {
        if (file_type == FILE_TYPE_UNKNOWN &&
            file_type_error != ERROR_SUCCESS) {
            throw_native_error("filesystem type query", file_type_error);
        }
        throw_identity_error(ports::FilesystemIdentityErrorCode::special_object,
                             "filesystem object type is unsupported");
    }

    FILE_ID_INFO id{};
    if (!GetFileInformationByHandleEx(
            handle, FileIdInfo, &id, static_cast<DWORD>(sizeof(id)))) {
        windows_detail::throw_stable_identity_unavailable(GetLastError());
    }
    FILE_BASIC_INFO basic{};
    if (!GetFileInformationByHandleEx(
            handle, FileBasicInfo, &basic, static_cast<DWORD>(sizeof(basic)))) {
        throw_native_error("filesystem basic metadata query", GetLastError());
    }
    FILE_STANDARD_INFO standard{};
    if (!GetFileInformationByHandleEx(
            handle, FileStandardInfo, &standard,
            static_cast<DWORD>(sizeof(standard)))) {
        throw_native_error("filesystem standard metadata query", GetLastError());
    }
    if (standard.EndOfFile.QuadPart < 0) {
        throw_identity_error(ports::FilesystemIdentityErrorCode::io_failure,
                             "filesystem size is invalid");
    }

    NativeObservation observation;
    observation.identity.volume_serial = id.VolumeSerialNumber;
    std::transform(
        std::begin(id.FileId.Identifier), std::end(id.FileId.Identifier),
        observation.identity.file_id.begin(),
        [](const unsigned char value) { return static_cast<std::byte>(value); });
    observation.identity.kind = standard.Directory
                                    ? ports::FilesystemObjectKind::directory
                                    : ports::FilesystemObjectKind::regular_file;
    observation.size_bytes = standard.Directory
                                 ? 0
                                 : static_cast<std::uint64_t>(
                                       standard.EndOfFile.QuadPart);
    observation.creation_time = basic.CreationTime.QuadPart;
    observation.change_time = basic.ChangeTime.QuadPart;
    observation.last_write_time = basic.LastWriteTime.QuadPart;
    observation.attributes = basic.FileAttributes;
    observation.link_count = standard.NumberOfLinks;
    return observation;
}

ports::FilesystemObservation project_observation(
    const NativeObservation& native) {
    const auto object = object_identity(native.identity);
    return {native.identity.kind, native.size_bytes, object,
            metadata_identity(native, object)};
}

bool special_namespace(const fs::path& path) {
    const std::wstring value = path.native();
    return value.starts_with(L"\\\\.\\");
}

UniqueHandle open_component(const fs::path& path,
                            const bool descriptor_read) {
    if (special_namespace(path)) {
        throw_identity_error(ports::FilesystemIdentityErrorCode::special_object,
                             "filesystem device namespaces are unsupported");
    }
    const auto api_path = filesystem_api_path(path).native();
    DWORD flags = FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS;
    if (descriptor_read) flags |= FILE_FLAG_OVERLAPPED;
    const DWORD access = descriptor_read ? GENERIC_READ : FILE_READ_ATTRIBUTES;
    UniqueHandle handle{CreateFileW(
        api_path.c_str(), access,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, flags, nullptr)};
    if (!handle.valid()) {
        throw_native_error("filesystem component open", GetLastError());
    }
    return handle;
}

OpenedPath open_path(const fs::path& absolute_path,
                     const ports::FilesystemObjectKind expected_kind) {
    if (special_namespace(absolute_path)) {
        throw_identity_error(ports::FilesystemIdentityErrorCode::special_object,
                             "filesystem device namespaces are unsupported");
    }

    std::vector<fs::path> components;
    for (const auto& component : absolute_path.relative_path()) {
        components.push_back(component);
    }
    fs::path current = absolute_path.root_path();
    if (current.empty()) {
        throw_identity_error(ports::FilesystemIdentityErrorCode::invalid_path,
                             "filesystem path root is unavailable");
    }

    OpenedPath opened;
    const bool root_is_final = components.empty();
    {
        UniqueHandle root = open_component(
            current, root_is_final &&
                         expected_kind == ports::FilesystemObjectKind::regular_file);
        auto native = query_native(root.get());
        if (native.identity.kind != ports::FilesystemObjectKind::directory ||
            (root_is_final && native.identity.kind != expected_kind)) {
            throw_identity_error(ports::FilesystemIdentityErrorCode::wrong_kind,
                                 "filesystem component kind does not match");
        }
        opened.chain.push_back(native.identity);
        if (root_is_final) {
            opened.native = native;
            opened.observation = project_observation(native);
            opened.final_handle = std::move(root);
            return opened;
        }
    }

    for (std::size_t index = 0; index < components.size(); ++index) {
        current /= components[index];
        const bool final = index + 1 == components.size();
        UniqueHandle handle = open_component(
            current, final &&
                         expected_kind == ports::FilesystemObjectKind::regular_file);
        auto native = query_native(handle.get());
        const auto required_kind = final
                                       ? expected_kind
                                       : ports::FilesystemObjectKind::directory;
        if (native.identity.kind != required_kind) {
            throw_identity_error(ports::FilesystemIdentityErrorCode::wrong_kind,
                                 "filesystem component kind does not match");
        }
        opened.chain.push_back(native.identity);
        if (final) {
            opened.native = native;
            opened.observation = project_observation(native);
            opened.final_handle = std::move(handle);
        }
    }
    return opened;
}

ports::FilesystemObservation observe_handle(
    HANDLE handle, const NativeIdentity& admitted_identity) {
    const auto native = query_native(handle);
    if (native.identity != admitted_identity) {
        throw_identity_error(ports::FilesystemIdentityErrorCode::identity_changed,
                             "open filesystem object identity changed");
    }
    return project_observation(native);
}

ports::PathBindingState observe_binding(
    const fs::path& path,
    const ports::FilesystemObjectKind kind,
    const std::vector<NativeIdentity>& admitted_chain) noexcept {
    try {
        const auto current = open_path(path, kind);
        return current.chain == admitted_chain
                   ? ports::PathBindingState::same_object
                   : ports::PathBindingState::different_object;
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

fs::path leaf_path(const std::string_view leaf) {
    std::u8string encoded;
    encoded.reserve(leaf.size());
    for (const unsigned char byte : leaf) {
        encoded.push_back(static_cast<char8_t>(byte));
    }
    return fs::path{encoded};
}

class WindowsRegularFile;
class WindowsDirectory;

std::unique_ptr<ports::AdmittedRegularFile> make_admitted_regular(
    const fs::path& path,
    const std::vector<NativeIdentity>* required_parent_chain = nullptr);
std::unique_ptr<ports::AdmittedDirectory> make_admitted_directory(
    const fs::path& path,
    const std::vector<NativeIdentity>* required_parent_chain = nullptr);

void require_parent_chain(
    const std::vector<NativeIdentity>& actual,
    const std::vector<NativeIdentity>* expected) {
    if (expected == nullptr) return;
    if (actual.size() != expected->size() + 1 ||
        !std::equal(expected->begin(), expected->end(), actual.begin())) {
        throw_identity_error(ports::FilesystemIdentityErrorCode::identity_changed,
                             "admitted directory binding changed");
    }
}

class WindowsRegularFile final : public ports::AdmittedRegularFile {
  public:
    WindowsRegularFile(fs::path path, OpenedPath opened)
        : path_(std::move(path)),
          handle_(std::move(opened.final_handle)),
          chain_(std::move(opened.chain)),
          identity_(opened.native.identity),
          initial_(std::move(opened.observation)) {}

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
        if (destination.empty()) return 0;
        const DWORD requested = static_cast<DWORD>((std::min)(
            destination.size(),
            static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        UniqueHandle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        if (!event.valid()) {
            throw_native_error("filesystem read event creation", GetLastError());
        }
        OVERLAPPED overlapped{};
        overlapped.Offset = static_cast<DWORD>(offset & 0xffffffffULL);
        overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32U);
        overlapped.hEvent = event.get();
        DWORD read{};
        if (!ReadFile(handle_.get(), destination.data(), requested, &read,
                      &overlapped)) {
            const DWORD error = GetLastError();
            if (error == ERROR_HANDLE_EOF) return 0;
            if (error != ERROR_IO_PENDING) {
                throw_native_error("descriptor-bound filesystem read", error);
            }
            if (!GetOverlappedResult(handle_.get(), &overlapped, &read, TRUE)) {
                const DWORD completion_error = GetLastError();
                if (completion_error == ERROR_HANDLE_EOF) return 0;
                throw_native_error("descriptor-bound filesystem read",
                                   completion_error);
            }
        }
        return read;
    }

    [[nodiscard]] ports::FilesystemObservation
    observe_open_object() const override {
        return observe_handle(handle_.get(), identity_);
    }

    [[nodiscard]] ports::PathBindingState
    observe_path_binding() const override {
        return observe_binding(path_, ports::FilesystemObjectKind::regular_file,
                               chain_);
    }

  private:
    fs::path path_;
    UniqueHandle handle_;
    std::vector<NativeIdentity> chain_;
    NativeIdentity identity_;
    ports::FilesystemObservation initial_;
};

class WindowsDirectory final : public ports::AdmittedDirectory {
  public:
    WindowsDirectory(fs::path path, OpenedPath opened)
        : ports::AdmittedDirectory(path, std::move(opened.observation)),
          handle_(std::move(opened.final_handle)),
          chain_(std::move(opened.chain)),
          identity_(opened.native.identity) {}

    [[nodiscard]] ports::FilesystemObservation
    observe_open_object() const override {
        return observe_handle(handle_.get(), identity_);
    }

    [[nodiscard]] ports::PathBindingState
    observe_path_binding() const override {
        return observe_binding(absolute_path(),
                               ports::FilesystemObjectKind::directory, chain_);
    }

  protected:
    [[nodiscard]] std::unique_ptr<ports::AdmittedRegularFile>
    open_regular_file_validated(const std::string_view leaf) const override {
        return make_admitted_regular(absolute_path() / leaf_path(leaf), &chain_);
    }

    [[nodiscard]] std::unique_ptr<ports::AdmittedDirectory>
    open_directory_validated(const std::string_view leaf) const override {
        return make_admitted_directory(absolute_path() / leaf_path(leaf),
                                       &chain_);
    }

  private:
    UniqueHandle handle_;
    std::vector<NativeIdentity> chain_;
    NativeIdentity identity_;
};

std::unique_ptr<ports::AdmittedRegularFile> make_admitted_regular(
    const fs::path& path,
    const std::vector<NativeIdentity>* required_parent_chain) {
    auto opened = open_path(path, ports::FilesystemObjectKind::regular_file);
    require_parent_chain(opened.chain, required_parent_chain);
    return std::make_unique<WindowsRegularFile>(path, std::move(opened));
}

std::unique_ptr<ports::AdmittedDirectory> make_admitted_directory(
    const fs::path& path,
    const std::vector<NativeIdentity>* required_parent_chain) {
    auto opened = open_path(path, ports::FilesystemObjectKind::directory);
    require_parent_chain(opened.chain, required_parent_chain);
    return std::make_unique<WindowsDirectory>(path, std::move(opened));
}

class WindowsFilesystemIdentity final : public ports::FilesystemIdentity {
  protected:
    [[nodiscard]] std::unique_ptr<ports::AdmittedRegularFile>
    admit_regular_file_validated(const fs::path& path) override {
        return make_admitted_regular(path);
    }

    [[nodiscard]] std::unique_ptr<ports::AdmittedDirectory>
    admit_directory_validated(const fs::path& path) override {
        return make_admitted_directory(path);
    }
};

} // namespace

namespace windows_detail {

void throw_stable_identity_unavailable(const std::uint32_t native_code) {
    throw_identity_error(
        ports::FilesystemIdentityErrorCode::identity_unavailable,
        "stable filesystem identity is unavailable",
        static_cast<DWORD>(native_code));
}

} // namespace windows_detail

std::unique_ptr<arena::ports::FilesystemIdentity> make_filesystem_identity() {
    return std::make_unique<WindowsFilesystemIdentity>();
}

} // namespace arena::platform
