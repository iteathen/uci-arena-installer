#include "arena/platform/workspace_permissions.hpp"
#include "arena/platform/filesystem_identity.hpp"
#include "arena/platform/filesystem_path.hpp"
#include "arena/ports/filesystem_identity.hpp"

#include <aclapi.h>
#include <windows.h>
#include <sddl.h>

#include <array>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>
#include <memory>

namespace arena::platform {
namespace {

[[noreturn]] void throw_windows_error(const std::string& operation, const DWORD error) {
    throw std::runtime_error(operation + " failed with Windows error " +
                             std::to_string(error));
}

struct CloseHandleDeleter {
    void operator()(void* handle) const noexcept { if (handle) CloseHandle(handle); }
};
using Handle = std::unique_ptr<void, CloseHandleDeleter>;
struct LocalFreeDeleter {
    void operator()(void* value) const noexcept { if (value) LocalFree(value); }
};
using Allocation = std::unique_ptr<void, LocalFreeDeleter>;

Handle pin_directory(const std::filesystem::path& path) {
    Handle handle{CreateFileW(filesystem_api_path(path).c_str(),
        FILE_READ_ATTRIBUTES | READ_CONTROL, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr)};
    if (handle.get() == INVALID_HANDLE_VALUE) {
        static_cast<void>(handle.release());
        throw_windows_error("per-user data root directory admission", GetLastError());
    }
    FILE_ATTRIBUTE_TAG_INFO info{};
    if (!GetFileInformationByHandleEx(handle.get(), FileAttributeTagInfo, &info, sizeof(info)) ||
        (info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (info.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) != 0 ||
        GetFileType(handle.get()) != FILE_TYPE_DISK) {
        throw std::runtime_error("per-user data root requires ordinary existing ancestors");
    }
    return handle;
}

std::vector<std::byte> current_user_sid() {
    HANDLE raw{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) {
        throw_windows_error("per-user identity admission", GetLastError());
    }
    Handle token{raw};
    DWORD length{};
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &length);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || length == 0) {
        throw_windows_error("per-user identity size", GetLastError());
    }
    std::vector<std::byte> information(length);
    if (!GetTokenInformation(token.get(), TokenUser, information.data(), length, &length)) {
        throw_windows_error("per-user identity query", GetLastError());
    }
    const PSID source = reinterpret_cast<TOKEN_USER*>(information.data())->User.Sid;
    std::vector<std::byte> result(GetLengthSid(source));
    if (!CopySid(static_cast<DWORD>(result.size()), result.data(), source)) {
        throw_windows_error("per-user identity capture", GetLastError());
    }
    return result;
}

bool trusted_writer(PSID sid, PSID user) {
    return IsValidSid(sid) && (EqualSid(sid, user) ||
        IsWellKnownSid(sid, WinLocalSystemSid) || IsWellKnownSid(sid, WinBuiltinAdministratorsSid));
}

void require_private_root(HANDLE handle, PSID user) {
    PSID owner{}; PACL dacl{}; PSECURITY_DESCRIPTOR raw{};
    const DWORD status = GetSecurityInfo(handle, SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr,
        &dacl, nullptr, &raw);
    Allocation descriptor{raw};
    if (status != ERROR_SUCCESS) throw_windows_error("per-user data root permissions inspection", status);
    bool accepted = owner && trusted_writer(owner, user) && dacl && IsValidAcl(dacl);
    bool seen_allow = false;
    constexpr ACCESS_MASK write_authority = FILE_WRITE_DATA | FILE_APPEND_DATA |
        FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES | FILE_DELETE_CHILD | DELETE |
        WRITE_DAC | WRITE_OWNER | GENERIC_WRITE | GENERIC_ALL;
    for (DWORD index = 0; accepted && index < dacl->AceCount; ++index) {
        void* raw_ace{};
        if (!GetAce(dacl, index, &raw_ace)) { accepted = false; break; }
        const auto* header = static_cast<ACE_HEADER*>(raw_ace);
        if (header->AceType == ACCESS_ALLOWED_ACE_TYPE) {
            seen_allow = true;
            const auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw_ace);
            if ((ace->Mask & write_authority) != 0 &&
                !trusted_writer(const_cast<DWORD*>(&ace->SidStart), user)) accepted = false;
        } else if (header->AceType != ACCESS_DENIED_ACE_TYPE || seen_allow) {
            accepted = false;
        }
    }
    if (!accepted) throw std::runtime_error(
        "per-user data root is not private; prepare its directory permissions explicitly or select a fresh root");
}

} // namespace

void prepare_per_user_data_root(const std::filesystem::path& root) {
    if (!root.is_absolute() || root.filename().empty() || root != root.lexically_normal()) {
        throw std::runtime_error("per-user data root requires an absolute normalized directory");
    }
    auto identity = make_filesystem_identity();
    // Reuse ordinary ancestor admission, then retain native no-delete handles
    // through creation/admission so pathname components cannot be renamed.
    const auto parent = identity->admit_directory(root.parent_path());
    std::vector<Handle> pinned;
    auto current = root.root_path();
    pinned.push_back(pin_directory(current));
    for (const auto& part : root.parent_path().relative_path()) {
        current /= part;
        pinned.push_back(pin_directory(current));
    }
    if (parent->observe_path_binding() != arena::ports::PathBindingState::same_object) {
        throw std::runtime_error("per-user data root ancestor binding changed");
    }
    auto user = current_user_sid();
    LPWSTR raw_sid{};
    if (!ConvertSidToStringSidW(user.data(), &raw_sid)) {
        throw_windows_error("per-user identity encoding", GetLastError());
    }
    Allocation sid_text{raw_sid};
    const std::wstring sid{raw_sid};
    const std::wstring sddl = L"O:" + sid + L"D:P(A;OICI;FA;;;" + sid +
        L")(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FRFX;;;BU)";
    PSECURITY_DESCRIPTOR raw_descriptor{};
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),
        SDDL_REVISION_1, &raw_descriptor, nullptr)) {
        throw_windows_error("per-user data root permissions construction", GetLastError());
    }
    Allocation descriptor{raw_descriptor};
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), raw_descriptor, FALSE};
    if (!CreateDirectoryW(filesystem_api_path(root).c_str(), &attributes)) {
        const DWORD error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS) throw_windows_error("per-user data root creation", error);
    }
    const auto admitted = identity->admit_directory(root);
    auto held = pin_directory(root);
    require_private_root(held.get(), user.data());
    if (admitted->observe_path_binding() != arena::ports::PathBindingState::same_object ||
        parent->observe_path_binding() != arena::ports::PathBindingState::same_object) {
        throw std::runtime_error("per-user data root binding changed");
    }
}

void grant_manager_workspace_access(
    const std::filesystem::path& workspace,
    const bool manager_service_access) {
    if (!std::filesystem::is_directory(workspace) ||
        std::filesystem::is_symlink(workspace)) {
        throw std::runtime_error(
            "managed service workspace is unavailable for permission setup");
    }

    if (!manager_service_access) {
        return;
    }

    std::array<unsigned char, SECURITY_MAX_SID_SIZE> sid_buffer{};
    DWORD sid_size = static_cast<DWORD>(sid_buffer.size());
    if (!CreateWellKnownSid(WinLocalServiceSid, nullptr, sid_buffer.data(), &sid_size)) {
        throw_windows_error("CreateWellKnownSid", GetLastError());
    }

    PSECURITY_DESCRIPTOR security_descriptor = nullptr;
    PACL current_dacl = nullptr;
    std::wstring path = workspace.native();
    const DWORD read_result = GetNamedSecurityInfoW(
        path.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
        &current_dacl, nullptr, &security_descriptor);
    if (read_result != ERROR_SUCCESS) {
        throw_windows_error("GetNamedSecurityInfoW", read_result);
    }

    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions =
        FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_GENERIC_EXECUTE | DELETE;
    access.grfAccessMode = GRANT_ACCESS;
    access.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_USER;
    access.Trustee.ptstrName =
        reinterpret_cast<LPWSTR>(sid_buffer.data());

    PACL updated_dacl = nullptr;
    const DWORD merge_result =
        SetEntriesInAclW(1, &access, current_dacl, &updated_dacl);
    if (merge_result != ERROR_SUCCESS) {
        LocalFree(security_descriptor);
        throw_windows_error("SetEntriesInAclW", merge_result);
    }

    const DWORD write_result = SetNamedSecurityInfoW(
        path.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
        updated_dacl, nullptr);
    LocalFree(updated_dacl);
    LocalFree(security_descriptor);
    if (write_result != ERROR_SUCCESS) {
        throw_windows_error("SetNamedSecurityInfoW", write_result);
    }
}

} // namespace arena::platform
