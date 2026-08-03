#include "arena/ports/asset_search_index.hpp"

#include <algorithm>
#include <tuple>

namespace arena::ports {

void finalize_indexed_asset_search_result(IndexedAssetSearchResult& result) {
    std::ranges::sort(result.candidates, {}, [](const IndexedAssetCandidate& candidate) {
        return std::tuple{
            static_cast<int>(candidate.kind),
            candidate.path.native(),
        };
    });
    result.candidates.erase(
        std::unique(result.candidates.begin(), result.candidates.end(),
                    [](const IndexedAssetCandidate& left,
                       const IndexedAssetCandidate& right) {
                        return left.kind == right.kind && left.path == right.path;
                    }),
        result.candidates.end());
    if (result.candidates.size() > result.maximum_results) {
        result.candidates.resize(result.maximum_results);
        result.truncated = true;
    }
}

std::string_view indexed_asset_kind_name(const IndexedAssetKind kind) noexcept {
    switch (kind) {
    case IndexedAssetKind::opening_book:
        return "opening_book";
    case IndexedAssetKind::syzygy:
        return "syzygy";
    case IndexedAssetKind::timing_profile:
        return "timing_profile";
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

} // namespace arena::ports
