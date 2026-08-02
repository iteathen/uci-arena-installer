#include "arena/ports/asset_search_index.hpp"

namespace arena::ports {

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
