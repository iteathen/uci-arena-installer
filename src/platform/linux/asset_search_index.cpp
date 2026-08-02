#include "arena/ports/asset_search_index.hpp"

#include "arena/ports/native_process.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace arena::ports {
namespace {

using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

std::optional<IndexedAssetKind> classify(const fs::path& path) {
    std::string extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(), [](const unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    std::string filename = path.filename().string();
    std::ranges::transform(filename, filename.begin(), [](const unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    if (extension == ".rtbw" || extension == ".rtbz") {
        return IndexedAssetKind::syzygy;
    }
    if (extension == ".book" || extension == ".bin") {
        return IndexedAssetKind::opening_book;
    }
    const bool named_time_profile =
        filename.starts_with("uci_time_profile_v") && filename.ends_with(".json");
    const bool descriptive_time_profile =
        extension == ".json" && filename.find("profile") != std::string::npos &&
        (filename.find("timing") != std::string::npos ||
         filename.find("time") != std::string::npos);
    if (named_time_profile || descriptive_time_profile) {
        return IndexedAssetKind::timing_profile;
    }
    return std::nullopt;
}

bool execute_locate_query(
    const fs::path& executable,
    const std::string_view expression,
    const IndexedAssetKind expected_kind,
    const std::size_t limit,
    const Clock::time_point deadline,
    std::vector<IndexedAssetCandidate>& candidates,
    bool& truncated) {
    if (Clock::now() >= deadline) {
        return false;
    }
    NativeProcess process;
    process.start(
        executable, executable.parent_path(),
        {"--ignore-case", "--limit", std::to_string(limit),
         "--regex", std::string(expression)});
    std::size_t accepted{};
    while (accepted < limit && Clock::now() < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - Clock::now());
        if (const auto line = process.read_line(
                std::min(remaining, std::chrono::milliseconds(100)));
            line.has_value()) {
            const fs::path path = *line;
            if (classify(path) == expected_kind) {
                candidates.push_back({expected_kind, path});
                ++accepted;
            }
            continue;
        }
        if (!process.running()) {
            break;
        }
    }
    const bool timed_out = process.running() && Clock::now() >= deadline;
    if (process.running()) {
        process.stop(std::chrono::milliseconds(250));
    } else {
        while (accepted < limit) {
            const auto line = process.read_line(std::chrono::milliseconds(0));
            if (!line.has_value()) {
                break;
            }
            const fs::path path = *line;
            if (classify(path) == expected_kind) {
                candidates.push_back({expected_kind, path});
                ++accepted;
            }
        }
        process.stop(std::chrono::milliseconds(0));
    }
    if (accepted >= limit) {
        truncated = true;
    }
    return !timed_out;
}

class LocateAssetSearchIndex final : public AssetSearchIndex {
  public:
    [[nodiscard]] IndexedAssetSearchResult
    search(const IndexedAssetQuery& query) override {
        IndexedAssetSearchResult result;
        result.adapter = "linux_locate_database";
        result.maximum_results = query.maximum_results;
        const auto started = Clock::now();
        const auto finish = [&](const IndexedSearchState state,
                                const std::string_view detail) {
            result.state = state;
            result.detail_code = detail;
            result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::now() - started);
            return result;
        };
        if (query.maximum_results == 0 || query.maximum_results > 4096 ||
            query.timeout <= std::chrono::milliseconds::zero() ||
            query.timeout > std::chrono::seconds(30)) {
            return finish(IndexedSearchState::failed, "invalid_search_budget");
        }
        fs::path executable;
        for (const fs::path candidate : {fs::path("/usr/bin/plocate"),
                                         fs::path("/usr/bin/locate")}) {
            if (fs::is_regular_file(candidate) && !fs::is_symlink(candidate)) {
                executable = candidate;
                break;
            }
        }
        if (executable.empty()) {
            return finish(IndexedSearchState::unavailable,
                          "locate_database_unavailable");
        }

        const auto deadline = started + query.timeout;
        const std::size_t per_kind = std::max<std::size_t>(
            1, query.maximum_results / 3);
        const struct {
            IndexedAssetKind kind;
            std::string_view expression;
        } queries[]{
            {IndexedAssetKind::syzygy, R"(\.(rtbw|rtbz)$)"},
            {IndexedAssetKind::timing_profile,
             R"((uci_time_profile_v[^/]*\.json|[^/]*(timing[^/]*profile|time[^/]*profile)[^/]*\.json)$)"},
            {IndexedAssetKind::opening_book, R"(\.(book|bin)$)"},
        };
        try {
            for (const auto& item : queries) {
                if (!execute_locate_query(
                        executable, item.expression, item.kind, per_kind,
                        deadline, result.candidates, result.truncated)) {
                    return finish(IndexedSearchState::timed_out,
                                  "search_budget_exhausted");
                }
            }
        } catch (const std::exception&) {
            return finish(IndexedSearchState::unavailable,
                          "locate_process_unavailable");
        }
        std::ranges::sort(result.candidates, {}, [](const IndexedAssetCandidate& candidate) {
            return candidate.path.native();
        });
        result.candidates.erase(
            std::unique(result.candidates.begin(), result.candidates.end(),
                        [](const IndexedAssetCandidate& left,
                           const IndexedAssetCandidate& right) {
                            return left.kind == right.kind && left.path == right.path;
                        }),
            result.candidates.end());
        return finish(IndexedSearchState::complete,
                      result.truncated ? "result_limit_reached" : "complete");
    }
};

} // namespace

std::unique_ptr<AssetSearchIndex> make_platform_asset_search_index() {
    return std::make_unique<LocateAssetSearchIndex>();
}

} // namespace arena::ports
