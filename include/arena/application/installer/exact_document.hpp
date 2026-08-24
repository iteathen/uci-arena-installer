#pragma once

#include "arena/ports/filesystem_identity.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

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
    // Container depth counts the root container as one. Total values count
    // every scalar or container once; object keys and container-end events do
    // not count as values. Member/item limits apply per immediate container.
    std::size_t maximum_depth{64};
    std::size_t maximum_values{1000000};
    std::size_t maximum_object_members{100000};
    std::size_t maximum_array_items{100000};
    std::size_t maximum_key_utf8_bytes{4096};
    std::size_t maximum_string_utf8_bytes{1048576};
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
