#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace arena::ports {

enum class NativeProcessOutputMode { merged, machine_stdout };

class NativeProcess final {
  public:
    NativeProcess();
    ~NativeProcess();

    NativeProcess(const NativeProcess&) = delete;
    NativeProcess& operator=(const NativeProcess&) = delete;

    void start(
        const std::filesystem::path& executable,
        const std::filesystem::path& working_directory,
        const std::vector<std::string>& arguments,
        NativeProcessOutputMode output_mode = NativeProcessOutputMode::merged,
        std::optional<std::uint64_t> maximum_stdout_bytes = std::nullopt);
    void write_line(const std::string& line);
    [[nodiscard]] std::optional<std::string> read_line(std::chrono::milliseconds timeout);
    [[nodiscard]] bool running() const;
    [[nodiscard]] std::uint64_t pid() const;
    [[nodiscard]] std::uint64_t dropped_observational_lines() const;
    [[nodiscard]] std::uint64_t captured_stdout_bytes() const;
    [[nodiscard]] bool stdout_limit_exceeded() const;
    [[nodiscard]] std::optional<std::uint32_t> exit_code() const;
    void stop(std::chrono::milliseconds graceful_timeout);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace arena::ports
