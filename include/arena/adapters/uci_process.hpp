#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace arena::adapters {

struct UciOption final {
    std::string name;
    std::string type;
    std::string default_value;
    std::optional<std::int64_t> minimum;
    std::optional<std::int64_t> maximum;
    std::vector<std::string> variants;
};

struct UciProbeResult {
    bool ok{false};
    std::string engine_name;
    std::string engine_author;
    std::string fingerprint;
    std::string runtime_model_fingerprint;
    std::string runtime_provider;
    std::string runtime_provider_fingerprint;
    std::vector<UciOption> options;
    std::string bestmove;
    std::uint64_t elapsed_ms{};
    std::uint64_t dropped_info_lines{};
    std::string error;
};

class UciProcess final {
  public:
    [[nodiscard]] static UciProbeResult probe(
        const std::filesystem::path& executable,
        const std::filesystem::path& working_directory,
        const std::vector<std::string>& arguments,
        std::chrono::milliseconds timeout = std::chrono::seconds(10),
        const std::map<std::string, std::string, std::less<>>& option_overrides = {});
};

}  // namespace arena::adapters
