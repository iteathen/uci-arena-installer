#include "arena/platform/windows_package_layout.hpp"
#include <windows.h>
#include <array>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace arena::platform {
namespace {
using arena::ports::InstallScope;
using nlohmann::json;
class RegistryKey {
public:
    HKEY handle{};
    ~RegistryKey() { if (handle) RegCloseKey(handle); }
};
bool open_key(RegistryKey& key, HKEY hive, const std::wstring& path) {
    const auto status = RegOpenKeyExW(hive, path.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &key.handle);
    if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) return false;
    if (status != ERROR_SUCCESS) throw std::runtime_error("package layout registration cannot be inspected");
    return true;
}
std::string read_text(HKEY key, const wchar_t* name) {
    DWORD type{}, size{};
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS
        || type != REG_SZ || size < sizeof(wchar_t) || size > 65536 || size % sizeof(wchar_t)) {
        throw std::runtime_error("package layout registration is incomplete or malformed");
    }
    std::vector<wchar_t> data(size / sizeof(wchar_t));
    DWORD actual = size;
    if (RegQueryValueExW(key, name, nullptr, &type,
        reinterpret_cast<BYTE*>(data.data()), &actual) != ERROR_SUCCESS
        || actual != size || type != REG_SZ || data.back() != L'\0') {
        throw std::runtime_error("package layout registration changed during observation");
    }
    const std::wstring_view text(data.data(), data.size() - 1);
    if (text.empty() || text.find(L'\0') != std::wstring_view::npos) throw std::runtime_error("package layout registration text is invalid");
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) throw std::runtime_error("package layout registration encoding is invalid");
    std::string result(bytes, '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), bytes, nullptr, nullptr) != bytes)
        throw std::runtime_error("package layout registration encoding is invalid");
    return result;
}
json read_record(HKEY key) {
    DWORD values{}, subkeys{};
    if (RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr,
        &values, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS || values != 4 || subkeys != 0)
        throw std::runtime_error("package layout registration is not a closed record");
    auto integration = read_text(key, L"IntegrationRoot");
    auto data = read_text(key, L"DataRoot");
    // CostFinalize renders an MSI directory property with one trailing separator.
    if (integration.ends_with('\\')) integration.pop_back();
    if (data.ends_with('\\')) data.pop_back();
    return {{"schema", read_text(key, L"Schema")}, {"scope", read_text(key, L"Scope")},
            {"integration_root", integration}, {"data_root", data}};
}
} // namespace

std::optional<json> read_windows_package_layout_selection(InstallScope scope,
    const std::wstring& registry_base) {
    const bool personal = scope == InstallScope::per_user;
    RegistryKey opposite;
    if (open_key(opposite, personal ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER,
        registry_base + (personal ? L"\\per-machine" : L"\\per-user")))
        throw std::runtime_error("both package layout scopes are present");
    RegistryKey key;
    if (!open_key(key, personal ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE,
        registry_base + (personal ? L"\\per-user" : L"\\per-machine"))) return std::nullopt;
    const auto first = read_record(key.handle);
    const auto second = read_record(key.handle);
    if (first != second) throw std::runtime_error("package layout registration changed during observation");
    if (first.at("schema") != "arena_package_layout_selection_v1"
        || first.at("scope") != (personal ? "per-user" : "per-machine"))
        throw std::runtime_error("package layout registration schema or scope is invalid");
    return std::optional<json>{std::in_place, first};
}
} // namespace arena::platform
