#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace arena::ports {

enum class IndexedAssetKind {
    opening_book,
    syzygy,
    timing_profile,
};

enum class IndexedSearchState {
    complete,
    unavailable,
    timed_out,
    failed,
};

struct IndexedAssetQuery {
    std::size_t maximum_results{256};
    std::chrono::milliseconds timeout{2000};
};

struct IndexedAssetCandidate {
    IndexedAssetKind kind{};
    std::filesystem::path path;
};

struct IndexedAssetSearchResult {
    std::string adapter;
    IndexedSearchState state{IndexedSearchState::unavailable};
    std::vector<IndexedAssetCandidate> candidates;
    std::size_t maximum_results{};
    std::chrono::milliseconds elapsed{};
    bool truncated{};
    std::string detail_code;
};

class AssetSearchIndex {
  public:
    virtual ~AssetSearchIndex() = default;

    [[nodiscard]] virtual IndexedAssetSearchResult
    search(const IndexedAssetQuery& query) = 0;
};

inline constexpr std::size_t indexed_asset_kind_count = 3;

[[nodiscard]] constexpr std::size_t indexed_asset_kind_ordinal(
    const IndexedAssetKind kind) noexcept {
    switch (kind) {
    case IndexedAssetKind::opening_book:
        return 0;
    case IndexedAssetKind::syzygy:
        return 1;
    case IndexedAssetKind::timing_profile:
        return 2;
    }
    return indexed_asset_kind_count;
}

[[nodiscard]] constexpr std::size_t indexed_asset_kind_budget(
    const std::size_t maximum_results,
    const IndexedAssetKind kind) noexcept {
    const std::size_t ordinal = indexed_asset_kind_ordinal(kind);
    if (ordinal >= indexed_asset_kind_count) {
        return 0;
    }
    return maximum_results / indexed_asset_kind_count +
        (ordinal < maximum_results % indexed_asset_kind_count ? 1 : 0);
}

void finalize_indexed_asset_search_result(IndexedAssetSearchResult& result);

[[nodiscard]] std::string_view indexed_asset_kind_name(IndexedAssetKind kind) noexcept;
[[nodiscard]] std::string_view indexed_search_state_name(IndexedSearchState state) noexcept;
[[nodiscard]] std::unique_ptr<AssetSearchIndex> make_platform_asset_search_index();

} // namespace arena::ports
