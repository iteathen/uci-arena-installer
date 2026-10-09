#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

namespace arena::ports {

enum class ProcessCommandFailureKind { start_failed, timeout, output_limit, invalid_json, nonzero_exit };

// Only closed transport categories and numeric facts cross this boundary.
// Arbitrary child bytes and arbitrary exception text are never constructor inputs.
class ProcessCommandFailure final : public std::runtime_error {
  public:
    ProcessCommandFailure(ProcessCommandFailureKind kind, std::uint64_t stdout_bytes = 0,
                          std::optional<std::uint32_t> exit_code = std::nullopt,
                          std::optional<std::uint32_t> os_code = std::nullopt)
        : std::runtime_error(message(kind, stdout_bytes, exit_code, os_code)) {}

  private:
    static std::string message(ProcessCommandFailureKind kind, std::uint64_t bytes,
                               std::optional<std::uint32_t> exit_code,
                               std::optional<std::uint32_t> os_code) {
        const char* category = "start_failed";
        switch (kind) {
        case ProcessCommandFailureKind::start_failed: break;
        case ProcessCommandFailureKind::timeout: category = "timeout"; break;
        case ProcessCommandFailureKind::output_limit: category = "output_limit"; break;
        case ProcessCommandFailureKind::invalid_json: category = "invalid_json"; break;
        case ProcessCommandFailureKind::nonzero_exit: category = "nonzero_exit"; break;
        }
        return std::string("category=") + category + "; stdout_bytes=" + std::to_string(bytes) +
               "; exit_code=" + (exit_code ? std::to_string(*exit_code) : "unavailable") +
               "; os_code=" + (os_code ? std::to_string(*os_code) : "unavailable") +
               "; stderr=discarded";
    }
};

} // namespace arena::ports
