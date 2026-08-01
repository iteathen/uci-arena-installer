#ifdef _WIN32

#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

#include <string>
#include <vector>

#include "arena/platform/windows_installer_launcher.hpp"

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int count{};
    LPWSTR* raw = CommandLineToArgvW(GetCommandLineW(), &count);
    if (raw == nullptr) {
        return 2;
    }
    std::vector<std::wstring> arguments;
    for (int index = 1; index < count; ++index) {
        arguments.emplace_back(raw[index]);
    }
    LocalFree(raw);
    return arena::platform::run_windows_installer_launcher(arguments);
}

#endif
