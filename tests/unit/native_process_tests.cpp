#include "arena/ports/native_process.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <thread>

#ifndef ARENA_MOCK_PROCESS_TREE_PATH
#error ARENA_MOCK_PROCESS_TREE_PATH must name the process-tree test helper
#endif

namespace {

using namespace std::chrono_literals;

TEST_CASE("native process stop terminates descendants that retain stdout") {
    const std::filesystem::path helper = ARENA_MOCK_PROCESS_TREE_PATH;
    arena::ports::NativeProcess process;
    process.start(helper, helper.parent_path(), {"--spawn-descendant"});

    const auto line = process.read_line(2s);
    REQUIRE(line.has_value());
    REQUIRE(*line == "spawned");

    const auto direct_exit_deadline = std::chrono::steady_clock::now() + 2s;
    while (process.running() && std::chrono::steady_clock::now() < direct_exit_deadline) {
        std::this_thread::sleep_for(10ms);
    }
    REQUIRE_FALSE(process.running());

    const auto started = std::chrono::steady_clock::now();
    process.stop(250ms);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    REQUIRE(elapsed < 3s);
    REQUIRE_FALSE(process.running());
    REQUIRE(process.pid() == 0);
}

} // namespace
