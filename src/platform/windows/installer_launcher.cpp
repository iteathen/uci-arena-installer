#include "arena/platform/windows_installer_launcher.hpp"

#ifdef _WIN32

#include "arena/application/installer/result.hpp"

#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <msi.h>
#include <objbase.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <vector>

namespace arena::platform {
namespace {

namespace fs = std::filesystem;
using arena::application::installer::InstallerExitCode;
using arena::application::installer::installer_exit_category;
using nlohmann::json;

constexpr std::array<std::byte, 16> kEnvelopeMagic{
    static_cast<std::byte>('U'), static_cast<std::byte>('C'),
    static_cast<std::byte>('I'), static_cast<std::byte>('A'),
    static_cast<std::byte>('R'), static_cast<std::byte>('E'),
    static_cast<std::byte>('N'), static_cast<std::byte>('A'),
    static_cast<std::byte>('I'), static_cast<std::byte>('N'),
    static_cast<std::byte>('S'), static_cast<std::byte>('T'),
    static_cast<std::byte>('V'), static_cast<std::byte>('1'),
    static_cast<std::byte>('!'), static_cast<std::byte>(0),
};
constexpr std::uint32_t kEnvelopeVersion = 1;
constexpr std::size_t kPayloadIoBufferSize = 1024 * 1024;
constexpr wchar_t kTitle[] = L"UCI Arena Suite Setup";
constexpr wchar_t kPerUserUpgradeCode[] =
    L"{143E64C2-4153-4E8B-8C93-6FB590779E9E}";
constexpr wchar_t kPerMachineUpgradeCode[] =
    L"{930B856A-66FB-4A34-B998-8A61684EA345}";

class LauncherFailure final : public std::runtime_error {
  public:
    LauncherFailure(const InstallerExitCode code, std::string message)
        : std::runtime_error(std::move(message)), code_(code) {}

    [[nodiscard]] InstallerExitCode code() const noexcept { return code_; }

  private:
    InstallerExitCode code_;
};

std::uint32_t decode_u32(const std::span<const std::byte> bytes,
                         const std::size_t offset) {
    std::uint32_t value{};
    for (std::size_t index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(std::to_integer<unsigned char>(
                     bytes[offset + index]))
                 << (index * 8U);
    }
    return value;
}

std::uint64_t decode_u64(const std::span<const std::byte> bytes,
                         const std::size_t offset) {
    std::uint64_t value{};
    for (std::size_t index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(std::to_integer<unsigned char>(
                     bytes[offset + index]))
                 << (index * 8U);
    }
    return value;
}

WindowsInstallerEnvelope parse_footer(const std::span<const std::byte> footer,
                                      const std::uint64_t total_size) {
    if (footer.size() != windows_installer_envelope_footer_size ||
        total_size < windows_installer_envelope_footer_size) {
        throw LauncherFailure(
            InstallerExitCode::invalid_manifest,
            "the installer envelope footer is missing or truncated");
    }
    if (!std::equal(kEnvelopeMagic.begin(), kEnvelopeMagic.end(), footer.begin())) {
        throw LauncherFailure(
            InstallerExitCode::invalid_manifest,
            "the installer envelope identity is invalid");
    }
    if (decode_u32(footer, 16) != kEnvelopeVersion ||
        decode_u32(footer, 20) != 0) {
        throw LauncherFailure(
            InstallerExitCode::invalid_manifest,
            "the installer envelope version is unsupported");
    }

    WindowsInstallerEnvelope envelope;
    envelope.stub_size = decode_u64(footer, 24);
    envelope.per_user.offset = decode_u64(footer, 32);
    envelope.per_user.size = decode_u64(footer, 40);
    std::copy_n(footer.begin() + 48, 32, envelope.per_user.sha256.begin());
    envelope.per_machine.offset = decode_u64(footer, 80);
    envelope.per_machine.size = decode_u64(footer, 88);
    std::copy_n(footer.begin() + 96, 32, envelope.per_machine.sha256.begin());

    const std::uint64_t footer_offset =
        total_size - windows_installer_envelope_footer_size;
    if (envelope.stub_size == 0 || envelope.per_user.size == 0 ||
        envelope.per_machine.size == 0 ||
        envelope.per_user.offset != envelope.stub_size ||
        envelope.per_user.offset >
            std::numeric_limits<std::uint64_t>::max() - envelope.per_user.size ||
        envelope.per_machine.offset !=
            envelope.per_user.offset + envelope.per_user.size ||
        envelope.per_machine.offset >
            std::numeric_limits<std::uint64_t>::max() - envelope.per_machine.size ||
        envelope.per_machine.offset + envelope.per_machine.size != footer_offset) {
        throw LauncherFailure(
            InstallerExitCode::invalid_manifest,
            "the installer envelope payload boundaries are invalid");
    }
    return envelope;
}

std::string scope_name(const WindowsInstallScope scope) {
    return scope == WindowsInstallScope::per_user ? "personal" : "all_users";
}

std::string state_name(const WindowsInstallationState state) {
    switch (state) {
    case WindowsInstallationState::none:
        return "none";
    case WindowsInstallationState::per_user:
        return "per_user";
    case WindowsInstallationState::per_machine:
        return "per_machine";
    case WindowsInstallationState::conflicting:
        return "conflicting";
    }
    return "conflicting";
}

std::string utf8_path(const fs::path& value) {
    const std::u8string encoded = value.u8string();
    return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

fs::path local_log_root() {
    PWSTR raw = nullptr;
    const HRESULT result = SHGetKnownFolderPath(
        FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &raw);
    if (FAILED(result) || raw == nullptr) {
        throw LauncherFailure(
            InstallerExitCode::invalid_configuration,
            "the interactive user's local application-data path is unavailable");
    }
    const fs::path root =
        fs::path(raw) / L"UCI Arena Manager" / L"Installer";
    CoTaskMemFree(raw);
    return root;
}

class LauncherLog final {
  public:
    LauncherLog() {
        const fs::path root = local_log_root();
        fs::create_directories(root);
        path_ = root / (L"launcher-" + std::to_wstring(GetCurrentProcessId()) +
                        L".jsonl");
        output_.open(path_, std::ios::binary | std::ios::app);
        if (!output_) {
            throw LauncherFailure(
                InstallerExitCode::invalid_configuration,
                "the installer launcher log cannot be opened");
        }
    }

    void write(json record) {
        SYSTEMTIME now{};
        GetSystemTime(&now);
        char timestamp[32]{};
        std::snprintf(timestamp, sizeof(timestamp),
                      "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
                      now.wYear, now.wMonth, now.wDay, now.wHour,
                      now.wMinute, now.wSecond, now.wMilliseconds);
        record["timestamp"] = timestamp;
        record["schema"] = "arena_windows_installer_launcher_log_v1";
        output_ << record.dump() << '\n';
        output_.flush();
    }

    [[nodiscard]] const fs::path& path() const noexcept { return path_; }

  private:
    fs::path path_;
    std::ofstream output_;
};

fs::path executable_path() {
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        throw LauncherFailure(
            InstallerExitCode::failed_preflight,
            "the installer launcher cannot resolve its executable path");
    }
    buffer.resize(length);
    return fs::path(buffer);
}

WindowsInstallerEnvelope read_envelope(const fs::path& self) {
    std::ifstream input(self, std::ios::binary);
    if (!input) {
        throw LauncherFailure(
            InstallerExitCode::package_validation_failed,
            "the installer launcher cannot open its own package envelope");
    }
    input.seekg(0, std::ios::end);
    const std::streamoff length = input.tellg();
    if (length < static_cast<std::streamoff>(windows_installer_envelope_footer_size)) {
        throw LauncherFailure(
            InstallerExitCode::invalid_manifest,
            "the installer package does not contain a complete envelope");
    }
    std::array<std::byte, windows_installer_envelope_footer_size> footer{};
    input.seekg(length - static_cast<std::streamoff>(footer.size()), std::ios::beg);
    input.read(reinterpret_cast<char*>(footer.data()),
               static_cast<std::streamsize>(footer.size()));
    if (!input) {
        throw LauncherFailure(
            InstallerExitCode::invalid_manifest,
            "the installer envelope footer cannot be read");
    }
    return parse_footer(footer, static_cast<std::uint64_t>(length));
}

class BCryptAlgorithm final {
  public:
    BCryptAlgorithm() {
        if (BCryptOpenAlgorithmProvider(
                &handle_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
            throw LauncherFailure(
                InstallerExitCode::dependency_failed,
                "Windows SHA-256 support is unavailable");
        }
    }
    ~BCryptAlgorithm() {
        if (handle_ != nullptr) BCryptCloseAlgorithmProvider(handle_, 0);
    }
    BCryptAlgorithm(const BCryptAlgorithm&) = delete;
    BCryptAlgorithm& operator=(const BCryptAlgorithm&) = delete;
    [[nodiscard]] BCRYPT_ALG_HANDLE get() const noexcept { return handle_; }

  private:
    BCRYPT_ALG_HANDLE handle_{};
};

std::array<std::byte, 32> hash_payload(
    std::ifstream& input, const WindowsInstallerPayload& payload) {
    BCryptAlgorithm algorithm;
    DWORD object_size{};
    DWORD bytes_written{};
    if (BCryptGetProperty(
            algorithm.get(), BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size),
            &bytes_written, 0) != 0 || object_size == 0) {
        throw LauncherFailure(
            InstallerExitCode::dependency_failed,
            "Windows SHA-256 state cannot be initialized");
    }
    std::vector<unsigned char> object(object_size);
    BCRYPT_HASH_HANDLE hash{};
    if (BCryptCreateHash(
            algorithm.get(), &hash, object.data(), object_size,
            nullptr, 0, 0) != 0) {
        throw LauncherFailure(
            InstallerExitCode::dependency_failed,
            "Windows SHA-256 hashing cannot be started");
    }
    const auto close_hash = [&hash]() {
        if (hash != nullptr) BCryptDestroyHash(hash);
    };
    try {
        input.clear();
        input.seekg(static_cast<std::streamoff>(payload.offset), std::ios::beg);
        std::uint64_t remaining = payload.size;
        std::vector<unsigned char> buffer(kPayloadIoBufferSize);
        while (remaining > 0) {
            const std::size_t requested = static_cast<std::size_t>(
                std::min<std::uint64_t>(remaining, buffer.size()));
            input.read(reinterpret_cast<char*>(buffer.data()),
                       static_cast<std::streamsize>(requested));
            if (input.gcount() != static_cast<std::streamsize>(requested) ||
                BCryptHashData(hash, buffer.data(),
                               static_cast<ULONG>(requested), 0) != 0) {
                throw LauncherFailure(
                    InstallerExitCode::package_validation_failed,
                    "an embedded installer payload cannot be hashed completely");
            }
            remaining -= requested;
        }
        std::array<std::byte, 32> digest{};
        if (BCryptFinishHash(
                hash, reinterpret_cast<PUCHAR>(digest.data()),
                static_cast<ULONG>(digest.size()), 0) != 0) {
            throw LauncherFailure(
                InstallerExitCode::dependency_failed,
                "Windows SHA-256 hashing cannot be completed");
        }
        close_hash();
        return digest;
    } catch (...) {
        close_hash();
        throw;
    }
}

void validate_payloads(const fs::path& self,
                       const WindowsInstallerEnvelope& envelope) {
    std::ifstream input(self, std::ios::binary);
    if (!input) {
        throw LauncherFailure(
            InstallerExitCode::package_validation_failed,
            "the installer package cannot be reopened for validation");
    }
    if (hash_payload(input, envelope.per_user) != envelope.per_user.sha256 ||
        hash_payload(input, envelope.per_machine) != envelope.per_machine.sha256) {
        throw LauncherFailure(
            InstallerExitCode::package_validation_failed,
            "an embedded fixed-scope installer digest does not match the envelope");
    }
}

bool related_product_installed(const wchar_t* upgrade_code) {
    wchar_t product_code[39]{};
    const UINT result = MsiEnumRelatedProductsW(
        upgrade_code, 0, 0, product_code);
    if (result == ERROR_SUCCESS) return true;
    if (result == ERROR_NO_MORE_ITEMS) return false;
    throw LauncherFailure(
        InstallerExitCode::failed_preflight,
        "Windows Installer could not inspect the existing installation scope");
}

std::optional<WindowsInstallScope> parse_requested_scope(
    const std::vector<std::wstring>& arguments,
    bool& dry_run,
    std::vector<std::wstring>& forwarded) {
    std::optional<WindowsInstallScope> requested;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        if (arguments[index] == L"--dry-run") {
            dry_run = true;
            continue;
        }
        if (arguments[index] != L"--scope") {
            forwarded.push_back(arguments[index]);
            continue;
        }
        if (requested || index + 1 >= arguments.size()) {
            throw LauncherFailure(
                InstallerExitCode::invalid_invocation,
                "--scope must appear once with personal or all-users");
        }
        const std::wstring& value = arguments[++index];
        if (value == L"personal") {
            requested = WindowsInstallScope::per_user;
        } else if (value == L"all-users") {
            requested = WindowsInstallScope::per_machine;
        } else {
            throw LauncherFailure(
                InstallerExitCode::invalid_invocation,
                "--scope accepts only personal or all-users");
        }
    }
    return requested;
}

std::optional<WindowsInstallScope> prompt_for_scope() {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    const TASKDIALOG_BUTTON buttons[] = {
        {100, L"Personal (recommended)\n"
              L"Installs only for your Windows account and does not request administrator approval."},
        {101, L"All users\n"
              L"Installs a Windows service for every account and requires administrator approval."},
    };
    TASKDIALOGCONFIG configuration{};
    configuration.cbSize = sizeof(configuration);
    configuration.dwFlags =
        TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
    configuration.pszWindowTitle = kTitle;
    configuration.pszMainIcon = TD_INFORMATION_ICON;
    configuration.pszMainInstruction = L"Choose who can use UCI Arena";
    configuration.pszContent =
        L"Your choice selects one fixed-scope installer. You can update or repair the same scope later.";
    configuration.cButtons = static_cast<UINT>(std::size(buttons));
    configuration.pButtons = buttons;
    configuration.nDefaultButton = 100;
    int selected{};
    const HRESULT result = TaskDialogIndirect(
        &configuration, &selected, nullptr, nullptr);
    if (FAILED(result)) {
        throw LauncherFailure(
            InstallerExitCode::failed_preflight,
            "Windows could not display the installation-scope selector");
    }
    if (selected == 100) return WindowsInstallScope::per_user;
    if (selected == 101) return WindowsInstallScope::per_machine;
    return std::nullopt;
}

std::wstring guid_name() {
    GUID value{};
    if (FAILED(CoCreateGuid(&value))) {
        throw LauncherFailure(
            InstallerExitCode::staging_failed,
            "a private installer extraction identity cannot be created");
    }
    wchar_t text[40]{};
    if (StringFromGUID2(value, text, static_cast<int>(std::size(text))) <= 0) {
        throw LauncherFailure(
            InstallerExitCode::staging_failed,
            "the installer extraction identity cannot be encoded");
    }
    std::wstring result(text);
    result.erase(std::remove_if(result.begin(), result.end(), [](wchar_t value) {
                     return value == L'{' || value == L'}';
                 }), result.end());
    return result;
}

fs::path create_temporary_directory() {
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetTempPathW(
        static_cast<DWORD>(buffer.size()), buffer.data());
    if (length == 0 || length >= buffer.size()) {
        throw LauncherFailure(
            InstallerExitCode::staging_failed,
            "the Windows temporary directory is unavailable");
    }
    buffer.resize(length);
    const fs::path directory =
        fs::path(buffer) / (L"uci-arena-installer-" + guid_name());
    if (!fs::create_directory(directory)) {
        throw LauncherFailure(
            InstallerExitCode::staging_failed,
            "the private installer extraction directory cannot be created");
    }
    return directory;
}

bool remove_temporary_directory(const fs::path& directory) {
    for (int attempt = 0; attempt < 20; ++attempt) {
        std::error_code error;
        fs::remove_all(directory, error);
        std::error_code status_error;
        const bool exists = fs::exists(directory, status_error);
        if (!error && !status_error && !exists) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

void extract_payload(const fs::path& self,
                     const WindowsInstallerPayload& payload,
                     const fs::path& destination) {
    std::ifstream input(self, std::ios::binary);
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    if (!input || !output) {
        throw LauncherFailure(
            InstallerExitCode::staging_failed,
            "the selected fixed-scope installer cannot be staged");
    }
    input.seekg(static_cast<std::streamoff>(payload.offset), std::ios::beg);
    std::uint64_t remaining = payload.size;
    std::vector<char> buffer(kPayloadIoBufferSize);
    while (remaining > 0) {
        const std::size_t requested = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, buffer.size()));
        input.read(buffer.data(), static_cast<std::streamsize>(requested));
        if (input.gcount() != static_cast<std::streamsize>(requested)) {
            throw LauncherFailure(
                InstallerExitCode::staging_failed,
                "the selected fixed-scope installer is truncated");
        }
        output.write(buffer.data(), static_cast<std::streamsize>(requested));
        if (!output) {
            throw LauncherFailure(
                InstallerExitCode::staging_failed,
                "the selected fixed-scope installer cannot be staged completely");
        }
        remaining -= requested;
    }
    output.flush();
    if (!output) {
        throw LauncherFailure(
            InstallerExitCode::staging_failed,
            "the selected fixed-scope installer stage cannot be committed");
    }
}

std::wstring quote_argument(const std::wstring& value) {
    if (!value.empty() && value.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return value;
    }
    std::wstring quoted{L"\""};
    std::size_t backslashes{};
    for (const wchar_t character : value) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'\"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'\"');
            backslashes = 0;
            continue;
        }
        quoted.append(backslashes, L'\\');
        backslashes = 0;
        quoted.push_back(character);
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'\"');
    return quoted;
}

DWORD launch_child(const fs::path& child,
                   const std::vector<std::wstring>& forwarded) {
    std::wstring command = quote_argument(child.wstring());
    for (const auto& argument : forwarded) {
        command += L" ";
        command += quote_argument(argument);
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(
            child.c_str(), command.data(), nullptr, nullptr, FALSE, 0,
            nullptr, child.parent_path().c_str(), &startup, &process)) {
        throw LauncherFailure(
            InstallerExitCode::deployment_failed,
            "the selected fixed-scope installer could not start");
    }
    CloseHandle(process.hThread);
    const DWORD wait = WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code{};
    const BOOL resolved = GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hProcess);
    if (wait != WAIT_OBJECT_0 || !resolved) {
        throw LauncherFailure(
            InstallerExitCode::deployment_failed,
            "the selected fixed-scope installer result could not be observed");
    }
    return exit_code;
}

void show_failure(const std::string& detail, const fs::path& log_path) {
    std::wstring message(detail.begin(), detail.end());
    if (!log_path.empty()) {
        message += L"\n\nLog: ";
        message += log_path.wstring();
    }
    MessageBoxW(nullptr, message.c_str(), kTitle, MB_OK | MB_ICONERROR);
}

} // namespace

WindowsInstallerEnvelope
parse_windows_installer_envelope(const std::span<const std::byte> bytes) {
    if (bytes.size() < windows_installer_envelope_footer_size) {
        throw LauncherFailure(
            InstallerExitCode::invalid_manifest,
            "the installer package is smaller than its envelope footer");
    }
    const auto footer = bytes.last(windows_installer_envelope_footer_size);
    return parse_footer(footer, bytes.size());
}

WindowsInstallScopePlan plan_windows_install_scope(
    const WindowsInstallationSnapshot& snapshot,
    const std::optional<WindowsInstallScope> requested_scope) {
    WindowsInstallScopePlan plan;
    if (snapshot.per_user_installed && snapshot.per_machine_installed) {
        plan.state = WindowsInstallationState::conflicting;
        plan.allowed = false;
        plan.detail =
            "both Personal and All users Manager scopes are installed; remove one before maintenance";
        return plan;
    }
    if (snapshot.per_user_installed) {
        plan.state = WindowsInstallationState::per_user;
        plan.selected_scope = WindowsInstallScope::per_user;
    } else if (snapshot.per_machine_installed) {
        plan.state = WindowsInstallationState::per_machine;
        plan.selected_scope = WindowsInstallScope::per_machine;
    } else {
        plan.state = WindowsInstallationState::none;
        plan.selected_scope = requested_scope;
        plan.selection_required = !requested_scope.has_value();
        plan.allowed = true;
        return plan;
    }
    if (requested_scope && requested_scope != plan.selected_scope) {
        plan.allowed = false;
        plan.detail =
            "the requested scope conflicts with the installed scope; uninstall explicitly before changing scope";
        return plan;
    }
    plan.allowed = true;
    return plan;
}

WindowsInstallationSnapshot detect_windows_installation() {
    return {
        .per_user_installed = related_product_installed(kPerUserUpgradeCode),
        .per_machine_installed = related_product_installed(kPerMachineUpgradeCode),
    };
}

int run_windows_installer_launcher(
    const std::vector<std::wstring>& arguments) {
    std::unique_ptr<LauncherLog> log;
    fs::path temporary_directory;
    try {
        log = std::make_unique<LauncherLog>();
        const fs::path self = executable_path();
        const WindowsInstallerEnvelope envelope = read_envelope(self);
        validate_payloads(self, envelope);

        bool dry_run = false;
        std::vector<std::wstring> forwarded;
        const auto requested_scope =
            parse_requested_scope(arguments, dry_run, forwarded);
        const WindowsInstallationSnapshot snapshot =
            detect_windows_installation();
        WindowsInstallScopePlan plan =
            plan_windows_install_scope(snapshot, requested_scope);
        log->write({
            {"event", "plan"},
            {"package", utf8_path(self)},
            {"existing_state", state_name(plan.state)},
            {"requested_scope", requested_scope
                                    ? scope_name(*requested_scope)
                                    : ""},
            {"selection_required", plan.selection_required},
            {"allowed", plan.allowed},
            {"forwarded_argument_count", forwarded.size()},
            {"dry_run", dry_run},
        });
        if (!plan.allowed) {
            throw LauncherFailure(
                InstallerExitCode::unsupported_state, plan.detail);
        }
        if (plan.selection_required) {
            if (dry_run) {
                throw LauncherFailure(
                    InstallerExitCode::invalid_invocation,
                    "--dry-run requires --scope personal or --scope all-users when no installation exists");
            }
            plan.selected_scope = prompt_for_scope();
            if (!plan.selected_scope) {
                log->write({
                    {"event", "result"},
                    {"exit_code", static_cast<int>(InstallerExitCode::operator_cancelled)},
                    {"exit_category", installer_exit_category(
                        InstallerExitCode::operator_cancelled)},
                });
                return static_cast<int>(InstallerExitCode::operator_cancelled);
            }
        }
        if (!plan.selected_scope) {
            throw LauncherFailure(
                InstallerExitCode::internal_failure,
                "the installation scope planner returned no selected scope");
        }
        log->write({
            {"event", "scope_selected"},
            {"scope", scope_name(*plan.selected_scope)},
        });
        if (dry_run) {
            log->write({
                {"event", "result"},
                {"exit_code", 0},
                {"exit_category", installer_exit_category(
                    InstallerExitCode::success)},
            });
            return 0;
        }

        temporary_directory = create_temporary_directory();
        const fs::path child =
            temporary_directory / L"UCI-Arena-Fixed-Scope-Setup.exe";
        const WindowsInstallerPayload& payload =
            *plan.selected_scope == WindowsInstallScope::per_user
                ? envelope.per_user
                : envelope.per_machine;
        extract_payload(self, payload, child);
        log->write({
            {"event", "child_staged"},
            {"scope", scope_name(*plan.selected_scope)},
            {"temporary_directory", utf8_path(temporary_directory)},
        });
        const DWORD child_exit = launch_child(child, forwarded);
        const bool cleaned = remove_temporary_directory(temporary_directory);
        if (cleaned) temporary_directory.clear();
        log->write({
            {"event", "result"},
            {"child_exit_code", child_exit},
            {"temporary_cleanup", cleaned},
            {"exit_code", cleaned
                              ? static_cast<std::uint64_t>(child_exit)
                              : static_cast<std::uint64_t>(
                                    InstallerExitCode::deployment_failed)},
        });
        if (!cleaned) {
            throw LauncherFailure(
                InstallerExitCode::deployment_failed,
                "installation completed but the temporary child installer could not be removed");
        }
        return static_cast<int>(child_exit);
    } catch (const LauncherFailure& error) {
        if (!temporary_directory.empty()) {
            static_cast<void>(remove_temporary_directory(temporary_directory));
        }
        const fs::path log_path = log ? log->path() : fs::path{};
        if (log) {
            log->write({
                {"event", "failure"},
                {"component", "windows_installer_launcher"},
                {"expected", "a validated fixed-scope child installer can be delegated safely"},
                {"observed", error.what()},
                {"rollback", "not_applicable_before_child_or_child_owned_after_launch"},
                {"remediation", "review the log and use the installer matching the existing scope"},
                {"exit_code", static_cast<int>(error.code())},
                {"exit_category", installer_exit_category(error.code())},
            });
        }
        show_failure(error.what(), log_path);
        return static_cast<int>(error.code());
    } catch (const std::exception& error) {
        if (!temporary_directory.empty()) {
            static_cast<void>(remove_temporary_directory(temporary_directory));
        }
        const fs::path log_path = log ? log->path() : fs::path{};
        if (log) {
            log->write({
                {"event", "failure"},
                {"component", "windows_installer_launcher"},
                {"observed", error.what()},
                {"exit_code", static_cast<int>(InstallerExitCode::internal_failure)},
                {"exit_category", installer_exit_category(
                    InstallerExitCode::internal_failure)},
            });
        }
        show_failure(error.what(), log_path);
        return static_cast<int>(InstallerExitCode::internal_failure);
    }
}

} // namespace arena::platform

#endif
