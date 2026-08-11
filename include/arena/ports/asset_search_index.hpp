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
    lichess_single_game_profile,
    lichess_capacity_profile,
    lichess_transport_profile,
};

enum class IndexedSearchState {
    complete,
    unavailable,
    timed_out,
    failed,
};

struct IndexedAssetRule {
    IndexedAssetKind kind{};
    std::vector<std::string> file_extensions;
    std::vector<std::string> filename_globs;
};

struct IndexedAssetQuery {
    std::size_t maximum_results{256};
    std::chrono::milliseconds timeout{2000};
    std::vector<IndexedAssetRule> rules;
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

[[nodiscard]] std::string_view indexed_asset_kind_name(IndexedAssetKind kind) noexcept;
[[nodiscard]] std::string_view indexed_search_state_name(IndexedSearchState state) noexcept;
[[nodiscard]] bool valid_indexed_asset_query(const IndexedAssetQuery& query) noexcept;
[[nodiscard]] bool indexed_asset_rule_matches(const IndexedAssetRule& rule,
                                              const std::filesystem::path& path);
[[nodiscard]] std::unique_ptr<AssetSearchIndex> make_platform_asset_search_index();

} // namespace arena::ports
