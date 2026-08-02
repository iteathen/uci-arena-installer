#pragma once

#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "arena/application/installer/setup.hpp"

namespace arena::application::installer {

struct RemovalLifecycleHooks {
    std::function<bool(const nlohmann::json&)> startup_cleanup_required;
    std::function<nlohmann::json(const nlohmann::json&)> uninstall_startup;
};

class RemovalLifecycle final {
  public:
    [[nodiscard]] static nlohmann::json
    plan_uninstall(const SetupPaths& paths,
                   const std::vector<std::string>& component_ids, bool all,
                   const RemovalLifecycleHooks& hooks);

    [[nodiscard]] static nlohmann::json
    uninstall(const SetupPaths& paths, const nlohmann::json& plan,
              bool confirmed, const RemovalLifecycleHooks& hooks);

    [[nodiscard]] static nlohmann::json
    plan_purge(const SetupPaths& paths,
               const std::vector<std::string>& component_ids, bool all);

    [[nodiscard]] static nlohmann::json
    purge(const SetupPaths& paths, const nlohmann::json& plan,
          bool confirmed);
};

} // namespace arena::application::installer
