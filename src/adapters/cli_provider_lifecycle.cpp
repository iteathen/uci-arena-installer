#include "arena/adapters/provider_lifecycle.hpp"

#include "arena/application/process_supervisor.hpp"

#include <chrono>
#include <stdexcept>

namespace arena::adapters {
namespace {

class CliProviderLifecycle final : public arena::ports::ProviderLifecycle {
  public:
    explicit CliProviderLifecycle(
        arena::application::ProcessSupervisor& process_supervisor) noexcept
        : process_supervisor_(process_supervisor) {}

  private:
    [[nodiscard]] nlohmann::json invoke_raw_validated(
        const arena::ports::ProviderLifecycleRequest& request,
        const std::chrono::steady_clock::time_point deadline) override {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            throw std::runtime_error("provider lifecycle deadline expired");
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        if (remaining < arena::ports::provider_lifecycle_timeout_minimum) {
            throw std::runtime_error("provider lifecycle deadline expired");
        }
        return process_supervisor_.run_json(
            request.executable, request.working_directory, request.arguments, remaining);
    }

    arena::application::ProcessSupervisor& process_supervisor_;
};

} // namespace

std::unique_ptr<arena::ports::ProviderLifecycle> make_cli_provider_lifecycle(
    arena::application::ProcessSupervisor& process_supervisor) {
    return std::make_unique<CliProviderLifecycle>(process_supervisor);
}

} // namespace arena::adapters
