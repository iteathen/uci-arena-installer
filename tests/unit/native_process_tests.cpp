#include "arena/ports/native_process.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#endif

#ifndef ARENA_MOCK_PROCESS_TREE_PATH
#error ARENA_MOCK_PROCESS_TREE_PATH must name the process-tree test helper
#endif

namespace {

using namespace std::chrono_literals;

std::uint64_t descendant_id_from_line(const std::string& line) {
    constexpr std::string_view prefix = "spawned ";
    if (!line.starts_with(prefix)) {
        throw std::runtime_error("process-tree helper did not publish a descendant id");
    }
    return std::stoull(line.substr(prefix.size()));
}

bool descendant_active(const std::uint64_t process_id) {
#ifdef _WIN32
    const HANDLE process = OpenProcess(
        SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE,
        static_cast<DWORD>(process_id));
    if (process == nullptr) {
        return GetLastError() == ERROR_ACCESS_DENIED;
    }
    const DWORD state = WaitForSingleObject(process, 0);
    CloseHandle(process);
    return state == WAIT_TIMEOUT;
#else
    if (kill(static_cast<pid_t>(process_id), 0) != 0) {
        return errno == EPERM;
    }
    std::ifstream status(
        "/proc/" + std::to_string(process_id) + "/stat",
        std::ios::binary);
    std::string value;
    if (std::getline(status, value)) {
        const auto command_end = value.rfind(')');
        if (command_end != std::string::npos && command_end + 2 < value.size() &&
            value[command_end + 2] == 'Z') {
            return false;
        }
    }
    return true;
#endif
}

#ifndef _WIN32
class ScopedSigpipeMask final {
  public:
    ScopedSigpipeMask() {
        sigemptyset(&set_);
        sigaddset(&set_, SIGPIPE);
        const int result = pthread_sigmask(SIG_BLOCK, &set_, &previous_);
        if (result != 0) {
            throw std::runtime_error("could not block SIGPIPE in test");
        }
    }

    ~ScopedSigpipeMask() {
        pthread_sigmask(SIG_SETMASK, &previous_, nullptr);
    }

    [[nodiscard]] bool pending() const {
        sigset_t pending{};
        if (sigpending(&pending) != 0) {
            throw std::runtime_error("could not inspect pending SIGPIPE in test");
        }
        return sigismember(&pending, SIGPIPE) == 1;
    }

  private:
    sigset_t set_{};
    sigset_t previous_{};
};
#endif

TEST_CASE("native process stop terminates descendants that retain stdout") {
    const std::filesystem::path helper = ARENA_MOCK_PROCESS_TREE_PATH;
    arena::ports::NativeProcess process;
    process.start(helper, helper.parent_path(), {"--spawn-descendant"});

    const auto line = process.read_line(2s);
    REQUIRE(line.has_value());
    const std::uint64_t descendant_id = descendant_id_from_line(*line);

    const auto direct_exit_deadline = std::chrono::steady_clock::now() + 2s;
    while (process.running() && std::chrono::steady_clock::now() < direct_exit_deadline) {
        std::this_thread::sleep_for(10ms);
    }
    REQUIRE_FALSE(process.running());
    REQUIRE(descendant_active(descendant_id));

    const auto started = std::chrono::steady_clock::now();
    process.stop(250ms);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    const auto descendant_exit_deadline = std::chrono::steady_clock::now() + 2s;
    while (descendant_active(descendant_id) &&
           std::chrono::steady_clock::now() < descendant_exit_deadline) {
        std::this_thread::sleep_for(10ms);
    }

    REQUIRE(elapsed < 3s);
    REQUIRE_FALSE(descendant_active(descendant_id));
    REQUIRE_FALSE(process.running());
    REQUIRE(process.pid() == 0);
}

TEST_CASE("native process rejects a negative graceful timeout without losing ownership") {
    const std::filesystem::path helper = ARENA_MOCK_PROCESS_TREE_PATH;
    arena::ports::NativeProcess process;
    process.start(helper, helper.parent_path(), {"--hold-stdout"});
    REQUIRE(process.running());

    REQUIRE_THROWS_WITH(
        process.stop(-1ms),
        "process graceful timeout must be non-negative");
    REQUIRE(process.running());

    process.stop(0ms);
    REQUIRE_FALSE(process.running());
    REQUIRE(process.pid() == 0);
}

#ifndef _WIN32
TEST_CASE("closed child stdin does not leave a generated SIGPIPE pending") {
    const std::filesystem::path helper = ARENA_MOCK_PROCESS_TREE_PATH;
    arena::ports::NativeProcess process;
    process.start(helper, helper.parent_path(), {"--close-stdin-hold"});
    REQUIRE(process.read_line(2s) == "stdin-closed");

    ScopedSigpipeMask sigpipe;
    REQUIRE_FALSE(sigpipe.pending());
    REQUIRE_THROWS(process.write_line("probe"));
    REQUIRE_FALSE(sigpipe.pending());

    process.stop(0ms);
}
#endif

} // namespace
