#include "arena/ports/asset_search_index.hpp"

#include <algorithm>
#include <cctype>
#include <set>

namespace arena::ports {

std::string_view indexed_asset_kind_name(const IndexedAssetKind kind) noexcept {
    switch (kind) {
    case IndexedAssetKind::opening_book:
        return "opening_book";
    case IndexedAssetKind::syzygy:
        return "syzygy";
    case IndexedAssetKind::timing_profile:
        return "timing_profile";
    case IndexedAssetKind::lichess_single_game_profile:
        return "lichess_single_game_profile";
    case IndexedAssetKind::lichess_capacity_profile:
        return "lichess_capacity_profile";
    case IndexedAssetKind::lichess_transport_profile:
        return "lichess_transport_profile";
    }
    return "unknown";
}

std::string_view indexed_search_state_name(const IndexedSearchState state) noexcept {
    switch (state) {
    case IndexedSearchState::complete:
        return "complete";
    case IndexedSearchState::unavailable:
        return "unavailable";
    case IndexedSearchState::timed_out:
        return "timed_out";
    case IndexedSearchState::failed:
        return "failed";
    }
    return "failed";
}

namespace {

bool valid_extension(const std::string_view value) noexcept {
    return value.size() >= 2 && value.size() <= 16 && value.front() == '.' &&
           std::all_of(value.begin() + 1, value.end(), [](const unsigned char character) {
               return std::isalnum(character) != 0;
           });
}

bool valid_filename_glob(const std::string_view value) noexcept {
    if (value.empty() || value.size() > 128 ||
        value.find('/') != std::string_view::npos ||
        value.find('\\') != std::string_view::npos) {
        return false;
    }
    bool has_literal = false;
    for (const unsigned char character : value) {
        if (std::isalnum(character) != 0 || character == '.' || character == '-' ||
            character == '_') {
            has_literal = true;
            continue;
        }
        if (character != '*' && character != '?') {
            return false;
        }
    }
    return has_literal;
}

std::string lowercase(std::string value) {
    std::ranges::transform(value, value.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool glob_matches(const std::string_view pattern, const std::string_view value) noexcept {
    std::size_t pattern_index = 0;
    std::size_t value_index = 0;
    std::size_t star = std::string_view::npos;
    std::size_t retry = 0;
    while (value_index < value.size()) {
        if (pattern_index < pattern.size() &&
            (pattern[pattern_index] == '?' || pattern[pattern_index] == value[value_index])) {
            ++pattern_index;
            ++value_index;
        } else if (pattern_index < pattern.size() && pattern[pattern_index] == '*') {
            star = pattern_index++;
            retry = value_index;
        } else if (star != std::string_view::npos) {
            pattern_index = star + 1;
            value_index = ++retry;
        } else {
            return false;
        }
    }
    while (pattern_index < pattern.size() && pattern[pattern_index] == '*') {
        ++pattern_index;
    }
    return pattern_index == pattern.size();
}

} // namespace

bool valid_indexed_asset_query(const IndexedAssetQuery& query) noexcept {
    if (query.maximum_results == 0 || query.maximum_results > 4096 ||
        query.timeout <= std::chrono::milliseconds::zero() ||
        query.timeout > std::chrono::seconds(30) || query.rules.empty() ||
        query.maximum_results < query.rules.size() ||
        query.rules.size() > 16) {
        return false;
    }
    std::set<IndexedAssetKind> kinds;
    for (const auto& rule : query.rules) {
        if (!kinds.insert(rule.kind).second ||
            rule.file_extensions.size() + rule.filename_globs.size() == 0 ||
            rule.file_extensions.size() + rule.filename_globs.size() > 16 ||
            !std::ranges::all_of(rule.file_extensions, valid_extension) ||
            !std::ranges::all_of(rule.filename_globs, valid_filename_glob)) {
            return false;
        }
    }
    return true;
}

bool indexed_asset_rule_matches(const IndexedAssetRule& rule,
                                const std::filesystem::path& path) {
    const std::string extension = lowercase(path.extension().string());
    if (std::ranges::any_of(rule.file_extensions, [&](const std::string& candidate) {
            return lowercase(candidate) == extension;
        })) {
        return true;
    }
    const std::string filename = lowercase(path.filename().string());
    return std::ranges::any_of(rule.filename_globs, [&](const std::string& pattern) {
        return glob_matches(lowercase(pattern), filename);
    });
}

} // namespace arena::ports
