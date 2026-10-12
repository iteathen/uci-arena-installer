#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include "arena/platform/windows_package_recovery.hpp"
#include "arena/platform/filesystem_identity.hpp"
#include "arena/ports/filesystem_identity.hpp"

#include <windows.h>
#include <msi.h>
#include <msiquery.h>
#include <sodium.h>

#include <array>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace arena::platform {
namespace {
using nlohmann::json;
namespace fs = std::filesystem;

class MsiHandle final {
  public:
    explicit MsiHandle(MSIHANDLE value = 0) : value_(value) {}
    ~MsiHandle() { if (value_) MsiCloseHandle(value_); }
    MsiHandle(const MsiHandle&) = delete;
    MsiHandle& operator=(const MsiHandle&) = delete;
    [[nodiscard]] MSIHANDLE get() const noexcept { return value_; }
  private:
    MSIHANDLE value_;
};

class FileHandle final {
  public:
    explicit FileHandle(HANDLE value) : value_(value) {
        if (value == INVALID_HANDLE_VALUE)
            throw std::runtime_error("manager recovery package admission failed");
    }
    ~FileHandle() { if (value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    FileHandle(FileHandle&& other) noexcept : value_(other.value_) {
        other.value_ = INVALID_HANDLE_VALUE;
    }
    [[nodiscard]] HANDLE get() const noexcept { return value_; }
  private:
    HANDLE value_;
};

std::wstring widen(const std::string& value) {
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                          value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (!count) throw std::runtime_error("invalid package identity encoding");
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                             static_cast<int>(value.size()), result.data(), count))
        throw std::runtime_error("invalid package identity encoding");
    return result;
}

std::string narrow(const std::wstring& value) {
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!count) throw std::runtime_error("invalid MSI property encoding");
    std::string result(static_cast<std::size_t>(count), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), count, nullptr, nullptr))
        throw std::runtime_error("invalid MSI property encoding");
    return result;
}

void checked(const UINT code, const char* operation) {
    if (code != ERROR_SUCCESS)
        throw std::runtime_error(std::string(operation) + "; msi_code=" + std::to_string(code));
}

bool digest_shape(const std::string& value) {
    return value.size() == 64 && value.find_first_not_of("0123456789abcdef") == std::string::npos;
}

FileHandle admit_locked_file(const fs::path& source) {
    auto identity = make_filesystem_identity();
    auto admitted = identity->admit_regular_file(source);
    // Keep the actual MSI bytes immutable while native MSI APIs reopen the path.
    FileHandle lock(CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (!GetFileInformationByHandleEx(lock.get(), FileAttributeTagInfo, &tag, sizeof(tag)) ||
        (tag.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)))
        throw std::runtime_error("manager recovery package is not an ordinary file");
    if (admitted->observe_open_object() != admitted->initial_observation() ||
        admitted->observe_path_binding() != arena::ports::PathBindingState::same_object)
        throw std::runtime_error("manager recovery package identity changed during admission");
    return lock;
}

std::string stream_sha256(HANDLE input, HANDLE output = INVALID_HANDLE_VALUE) {
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(input, zero, nullptr, FILE_BEGIN))
        throw std::runtime_error("cannot read manager recovery package");
    crypto_hash_sha256_state hash{};
    crypto_hash_sha256_init(&hash);
    std::array<unsigned char, 65536> buffer{};
    for (;;) {
        DWORD read{};
        if (!ReadFile(input, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
            throw std::runtime_error("cannot read manager recovery package");
        if (!read) break;
        crypto_hash_sha256_update(&hash, buffer.data(), read);
        if (output != INVALID_HANDLE_VALUE) {
            DWORD written{};
            if (!WriteFile(output, buffer.data(), read, &written, nullptr) || written != read)
                throw std::runtime_error("cannot retain manager recovery package");
        }
    }
    std::array<unsigned char, crypto_hash_sha256_BYTES> bytes{};
    crypto_hash_sha256_final(&hash, bytes.data());
    std::array<char, crypto_hash_sha256_BYTES * 2 + 1> text{};
    sodium_bin2hex(text.data(), text.size(), bytes.data(), bytes.size());
    return text.data();
}

std::string property(MSIHANDLE database, const wchar_t* name) {
    const std::wstring query = std::wstring(L"SELECT `Value` FROM `Property` WHERE `Property`='") + name + L"'";
    MSIHANDLE raw_view{};
    checked(MsiDatabaseOpenViewW(database, query.c_str(), &raw_view), "read MSI property");
    MsiHandle view(raw_view);
    checked(MsiViewExecute(view.get(), 0), "execute MSI property query");
    MSIHANDLE raw_record{};
    const UINT fetch = MsiViewFetch(view.get(), &raw_record);
    if (fetch == ERROR_NO_MORE_ITEMS) return {};
    checked(fetch, "fetch MSI property");
    MsiHandle record(raw_record);
    std::array<wchar_t, 4097> value{};
    DWORD count = static_cast<DWORD>(value.size());
    checked(MsiRecordGetStringW(record.get(), 1, value.data(), &count), "bounded MSI property");
    return narrow(std::wstring(value.data(), count));
}

json inspect_locked(const fs::path& source) {
    MSIHANDLE raw_database{};
    checked(MsiOpenDatabaseW(source.c_str(), MSIDBOPEN_READONLY, &raw_database), "open original MSI");
    MsiHandle database(raw_database);
    MSIHANDLE raw_summary{};
    checked(MsiGetSummaryInformationW(database.get(), nullptr, 0, &raw_summary), "open MSI summary");
    MsiHandle summary(raw_summary);
    UINT type{};
    INT integer{};
    FILETIME time{};
    std::array<wchar_t, 129> package{};
    DWORD count = static_cast<DWORD>(package.size());
    checked(MsiSummaryInfoGetPropertyW(summary.get(), 9, &type, &integer, &time,
        package.data(), &count), "read MSI package identity");
    if (type != VT_LPSTR) throw std::runtime_error("unsupported MSI package identity");
    const std::string all_users = property(database.get(), L"ALLUSERS");
    const std::string install_per_user = property(database.get(), L"MSIINSTALLPERUSER");
    const bool per_machine = all_users == "1";
    if (!per_machine && all_users != "" && !(all_users == "2" && install_per_user == "1"))
        throw std::runtime_error("MSI has no fixed supported installation scope");
    json identity{{"product_code", property(database.get(), L"ProductCode")},
                  {"package_code", narrow(std::wstring(package.data(), count))},
                  {"upgrade_code", property(database.get(), L"UpgradeCode")},
                  {"version", property(database.get(), L"ProductVersion")},
                  {"scope", per_machine ? "per-machine" : "per-user"}};
    for (const auto* key : {"product_code", "package_code", "upgrade_code"}) {
        GUID parsed{};
        const auto value = widen(identity.at(key).get<std::string>());
        if (FAILED(CLSIDFromString(value.c_str(), &parsed)))
            throw std::runtime_error("invalid MSI package identity");
    }
    // The reviewed builders embed every cabinet; a standalone retained MSI
    // must not silently depend on an unavailable external payload.
    MSIHANDLE raw_view{};
    checked(MsiDatabaseOpenViewW(database.get(), L"SELECT `Cabinet` FROM `Media`", &raw_view), "read MSI media");
    MsiHandle view(raw_view);
    checked(MsiViewExecute(view.get(), 0), "read MSI cabinets");
    for (;;) {
        MSIHANDLE raw_record{};
        const UINT result = MsiViewFetch(view.get(), &raw_record);
        if (result == ERROR_NO_MORE_ITEMS) break;
        checked(result, "fetch MSI cabinet");
        MsiHandle record(raw_record);
        std::array<wchar_t, 257> cabinet{};
        DWORD cabinet_count = static_cast<DWORD>(cabinet.size());
        checked(MsiRecordGetStringW(record.get(), 1, cabinet.data(), &cabinet_count), "bounded MSI cabinet");
        if (!cabinet_count || cabinet[0] != L'#')
            throw std::runtime_error("original MSI recovery requires embedded cabinet media");
    }
    return identity;
}

std::string installed_property(const std::wstring& product, MSIINSTALLCONTEXT context, const wchar_t* name) {
    std::array<wchar_t, 4097> value{};
    DWORD count = static_cast<DWORD>(value.size());
    checked(MsiGetProductInfoExW(product.c_str(), nullptr, context, name, value.data(), &count), "read installed MSI identity");
    return narrow(std::wstring(value.data(), count));
}

std::wstring quoted_property(const wchar_t* name, const fs::path& value) {
    const auto text = value.native();
    if (!value.is_absolute() || text.find_first_of(L"\"\r\n") != std::wstring::npos)
        throw std::runtime_error("invalid frozen MSI root");
    return std::wstring(name) + L"=\"" + text + L"\" ";
}
} // namespace

json inspect_windows_original_msi(const fs::path& source, const std::string& expected_sha256) {
    if (!digest_shape(expected_sha256)) throw std::runtime_error("manager recovery package digest is required");
    auto lock = admit_locked_file(source);
    if (stream_sha256(lock.get()) != expected_sha256)
        throw std::runtime_error("manager recovery package digest mismatch");
    return inspect_locked(source);
}

std::vector<json> installed_windows_msi_products(const std::string& upgrade_code, bool per_machine) {
    const auto upgrade = widen(upgrade_code);
    GUID parsed{};
    if (FAILED(CLSIDFromString(upgrade.c_str(), &parsed))) throw std::runtime_error("invalid MSI upgrade identity");
    const auto context = per_machine ? MSIINSTALLCONTEXT_MACHINE : MSIINSTALLCONTEXT_USERUNMANAGED;
    std::vector<json> products;
    for (DWORD index = 0;; ++index) {
        std::array<wchar_t, 39> product{};
        const UINT result = MsiEnumRelatedProductsW(upgrade.c_str(), 0, index, product.data());
        if (result == ERROR_NO_MORE_ITEMS) break;
        checked(result, "enumerate installed MSI identity");
        DWORD count{};
        const UINT available = MsiGetProductInfoExW(product.data(), nullptr, context, INSTALLPROPERTY_PACKAGECODE, nullptr, &count);
        if (available == ERROR_UNKNOWN_PRODUCT) continue;
        if (available != ERROR_MORE_DATA && available != ERROR_SUCCESS) checked(available, "admit installed MSI scope");
        products.push_back({{"product_code", narrow(product.data())},
                            {"package_code", installed_property(product.data(), context, INSTALLPROPERTY_PACKAGECODE)},
                            {"version", installed_property(product.data(), context, INSTALLPROPERTY_VERSIONSTRING)},
                            {"scope", per_machine ? "per-machine" : "per-user"}});
    }
    return products;
}

void retain_windows_original_msi(const fs::path& source, const fs::path& destination,
                                 const std::string& expected_sha256) {
    if (!digest_shape(expected_sha256)) throw std::runtime_error("manager recovery package digest is required");
    auto input = admit_locked_file(source);
    auto identity = make_filesystem_identity();
    auto parent = identity->admit_directory(destination.parent_path());
    FileHandle output(CreateFileW(destination.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (stream_sha256(input.get(), output.get()) != expected_sha256 || !FlushFileBuffers(output.get()))
        throw std::runtime_error("manager recovery package could not be retained with exact digest");
    if (parent->observe_path_binding() != arena::ports::PathBindingState::same_object)
        throw std::runtime_error("manager recovery media parent identity changed");
}

void restore_windows_msi_package(const fs::path& source, const json& expected_identity,
                                 const std::string& expected_sha256,
                                 const fs::path& integration_root, const fs::path& data_root,
                                 const fs::path& log_path) {
    auto lock = admit_locked_file(source);
    if (!digest_shape(expected_sha256) || stream_sha256(lock.get()) != expected_sha256 ||
        inspect_locked(source) != expected_identity)
        throw std::runtime_error("retained manager recovery media identity changed");
    const bool per_machine = expected_identity.at("scope") == "per-machine";
    const auto current = installed_windows_msi_products(expected_identity.at("upgrade_code"), per_machine);
    if (current.size() > 1 || (!current.empty() &&
        current.front().at("product_code") != expected_identity.at("product_code")))
        throw std::runtime_error("manager recovery refuses unexpected installed product authority");
    MsiSetInternalUI(INSTALLUILEVEL_NONE, nullptr);
    checked(MsiEnableLogW(INSTALLLOGMODE_VERBOSE | INSTALLLOGMODE_ERROR | INSTALLLOGMODE_WARNING |
        INSTALLLOGMODE_INFO | INSTALLLOGMODE_ACTIONSTART | INSTALLLOGMODE_ACTIONDATA |
        INSTALLLOGMODE_PROPERTYDUMP, log_path.c_str(), INSTALLLOGATTRIBUTES_FLUSHEACHLINE), "open MSI recovery log");
    std::wstring arguments = quoted_property(L"INSTALL_ROOT", integration_root) +
        quoted_property(L"ARENA_DATA_ROOT", data_root) +
        L"ARPSYSTEMCOMPONENT=1 REINSTALLMODE=amus REBOOT=ReallySuppress";
    if (!current.empty()) arguments += L" REINSTALL=ALL";
    const UINT result = MsiInstallProductW(source.c_str(), arguments.c_str());
    MsiEnableLogW(0, nullptr, 0);
    if (result != ERROR_SUCCESS && result != ERROR_SUCCESS_REBOOT_REQUIRED)
        checked(result, "restore exact manager MSI");
    const auto observed = installed_windows_msi_products(expected_identity.at("upgrade_code"), per_machine);
    if (observed.size() != 1 || observed.front().at("product_code") != expected_identity.at("product_code") ||
        observed.front().at("package_code") != expected_identity.at("package_code") ||
        observed.front().at("version") != expected_identity.at("version"))
        throw std::runtime_error("MSI recovery result did not establish the exact decided package");
}
} // namespace arena::platform
