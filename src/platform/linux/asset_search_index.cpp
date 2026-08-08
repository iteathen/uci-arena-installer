#include "arena/ports/asset_search_index.hpp"

#include "arena/ports/native_process.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <exception>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace arena::ports {
namespace {

using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

std::string regex_escape(const std::string_view value, const bool glob) {
    std::string result;
    result.reserve(value.size() * 2);
    for (const char character : value) {
        if (glob && character == '*') {
            result.append("[^/]*");
        } else if (glob && character == '?') {
            result.append("[^/]");
        } else {
            if (std::string_view(".^$|()[]{}+?*\\").find(character) !=
                std::string_view::npos) {
                result.push_back('\\');
            }
            result.push_back(character);
        }
    }
    return result;
}

std::string locate_expression(const IndexedAssetRule& rule) {
    std::vector<std::string> alternatives;
    alternatives.reserve(rule.file_extensions.size() + rule.filename_globs.size());
    for (const auto& extension : rule.file_extensions) {
        alternatives.push_back(regex_escape(extension, false) + "$");
    }
    for (const auto& glob : rule.filename_globs) {
        alternatives.push_back("/" + regex_escape(glob, true) + "$");
    }
    std::ostringstream expression;
    expression << '(';
    for (std::size_t index = 0; index < alternatives.size(); ++index) {
        if (index != 0) {
            expression << '|';
        }
        expression << alternatives[index];
    }
    expression << ')';
    return expression.str();
}

bool execute_locate_query(
    const fs::path& executable,
    const std::string_view expression,
    const IndexedAssetRule& rule,
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
            if (indexed_asset_rule_matches(rule, path)) {
                candidates.push_back({rule.kind, path});
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
            if (indexed_asset_rule_matches(rule, path)) {
                candidates.push_back({rule.kind, path});
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
        if (!valid_indexed_asset_query(query)) {
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
            1, query.maximum_results / query.rules.size());
        try {
            for (const auto& rule : query.rules) {
                if (!execute_locate_query(
                        executable, locate_expression(rule), rule, per_kind,
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
