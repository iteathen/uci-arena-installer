#pragma once

#include <filesystem>

namespace arena::platform {

// Windows per-user setup: create one missing final root privately, or inspect
// an existing root without changing its security descriptor or descendants.
#ifdef _WIN32
void prepare_per_user_data_root(const std::filesystem::path& root);
#endif

void grant_manager_workspace_access(
    const std::filesystem::path& workspace,
    bool manager_service_access = true);

} // namespace arena::platform
