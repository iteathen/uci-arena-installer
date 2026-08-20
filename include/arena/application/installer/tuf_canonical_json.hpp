#pragma once

#include <string>

#include <nlohmann/json_fwd.hpp>

namespace arena::application::installer {

[[nodiscard]] std::string
encode_tuf_olpc_canonical_json(const nlohmann::json& value);

} // namespace arena::application::installer
