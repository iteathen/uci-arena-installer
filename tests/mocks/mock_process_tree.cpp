#include <chrono>
#include <iostream>
#include <string_view>
#include <thread>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

int hold_stdout() {
    std::this_thread::sleep_for(std::chrono::seconds(30));
    return 0;
}

int close_stdin_and_hold() {
#ifdef _WIN32
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    if (input != nullptr && input != INVALID_HANDLE_VALUE) {
        CloseHandle(input);
        SetStdHandle(STD_INPUT_HANDLE, nullptr);
    }
#else
    close(STDIN_FILENO);
#endif
    std::cout << "stdin-closed" << std::endl;
    return hold_stdout();
}

#ifdef _WIN32
int spawn_descendant() {
    std::wstring executable(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size()) {
        return 10;
    }
    executable.resize(length);
    std::wstring command_line = L"\"" + executable + L"\" --hold-stdout";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(
            executable.c_str(), command_line.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr,
            &startup, &process)) {
        return 11;
    }
    const DWORD descendant_id = process.dwProcessId;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    std::cout << "spawned " << descendant_id << std::endl;
    return 0;
}
#else
int spawn_descendant() {
    const pid_t child = fork();
    if (child < 0) {
        return 10;
    }
    if (child == 0) {
        return hold_stdout();
    }
    std::cout << "spawned " << child << std::endl;
    return 0;
}
#endif

} // namespace

int main(const int argc, char** argv) {
    if (argc != 2) {
        return 2;
    }
    const std::string_view operation = argv[1];
    if (operation == "--hold-stdout") {
        return hold_stdout();
    }
    if (operation == "--close-stdin-hold") {
        return close_stdin_and_hold();
    }
    if (operation == "--spawn-descendant") {
        return spawn_descendant();
    }
    return 3;
}