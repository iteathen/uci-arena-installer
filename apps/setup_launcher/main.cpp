#include <windows.h>
#include <shellapi.h>

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

constexpr std::wstring_view private_setup =
    L"..\\libexec\\uci-arena-installer\\bin\\arena-setup.exe";

std::wstring quote_argument(std::wstring_view argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\"") == std::wstring_view::npos) {
        return std::wstring(argument);
    }

    std::wstring quoted{L'"'};
    std::size_t backslashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'"');
        } else {
            quoted.append(backslashes, L'\\');
            quoted.push_back(character);
        }
        backslashes = 0;
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

std::filesystem::path executable_path() {
    std::wstring buffer(1024, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            throw std::runtime_error("GetModuleFileNameW failed");
        }
        if (length < buffer.size() - 1) {
            buffer.resize(length);
            return std::filesystem::path(buffer);
        }
        if (buffer.size() >= 32768) {
            throw std::runtime_error("setup launcher path exceeds the Windows limit");
        }
        buffer.resize(buffer.size() * 2);
    }
}

int fail(std::wstring_view message, DWORD error = 0) {
    std::wcerr << L"arena-setup launcher: " << message;
    if (error != 0) {
        std::wcerr << L" (Windows error " << error << L')';
    }
    std::wcerr << L'\n';
    return 2;
}

}  // namespace

int wmain() {
    try {
        const std::filesystem::path target =
            (executable_path().parent_path() / private_setup).lexically_normal();
        std::error_code status_error;
        if (!std::filesystem::is_regular_file(target, status_error) || status_error) {
            return fail(L"the private Installer runtime is unavailable; repair UCI Arena Suite Setup");
        }

        int argument_count = 0;
        LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
        if (arguments == nullptr || argument_count < 1) {
            return fail(L"the Windows command line could not be decoded", GetLastError());
        }
        std::wstring command_line = quote_argument(target.native());
        for (int index = 1; index < argument_count; ++index) {
            command_line.push_back(L' ');
            command_line.append(quote_argument(arguments[index]));
        }
        LocalFree(arguments);

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(
                target.c_str(), command_line.data(), nullptr, nullptr, TRUE,
                CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr, &startup, &process)) {
            return fail(L"the private Installer runtime could not be started", GetLastError());
        }
        CloseHandle(process.hThread);
        const DWORD wait_result = WaitForSingleObject(process.hProcess, INFINITE);
        if (wait_result != WAIT_OBJECT_0) {
            const DWORD error = GetLastError();
            CloseHandle(process.hProcess);
            return fail(L"waiting for the private Installer runtime failed", error);
        }
        DWORD exit_code = 2;
        if (!GetExitCodeProcess(process.hProcess, &exit_code)) {
            const DWORD error = GetLastError();
            CloseHandle(process.hProcess);
            return fail(L"the private Installer runtime exit code is unavailable", error);
        }
        CloseHandle(process.hProcess);
        return static_cast<int>(exit_code);
    } catch (const std::exception& error) {
        std::cerr << "arena-setup launcher: " << error.what() << '\n';
        return 2;
    }
}
