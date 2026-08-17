#pragma once

#include "arena/ports/filesystem_identity.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace arena::application::installer {

struct ExactDocumentLimits {
    std::uint64_t maximum_bytes;
    bool require_nonempty{true};
};

struct ExactDocument {
    std::vector<std::byte> bytes;
    std::string sha256;
    arena::ports::FilesystemObservation initial_observation;
    arena::ports::FilesystemObservation final_observation;
};

struct BoundedJsonLimits {
    std::size_t maximum_depth;
    std::size_t maximum_values;
    std::size_t maximum_object_members;
    std::size_t maximum_array_items;
    std::size_t maximum_key_utf8_bytes;
    std::size_t maximum_string_utf8_bytes;
    bool allow_floating_point{false};
};

class ExactDocumentReader final {
  public:
    [[nodiscard]] ExactDocument read_child(
        const arena::ports::AdmittedDirectory& directory,
        std::string_view leaf,
        const ExactDocumentLimits& limits) const;
};

[[nodiscard]] nlohmann::json parse_bounded_json(
    const ExactDocument& document,
    const BoundedJsonLimits& limits);

} // namespace arena::application::installer
