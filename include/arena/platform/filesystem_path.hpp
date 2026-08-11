#pragma once

#include <filesystem>

namespace arena::platform {

[[nodiscard]] std::filesystem::path
filesystem_api_path(const std::filesystem::path& path);

} // namespace arena::platform
