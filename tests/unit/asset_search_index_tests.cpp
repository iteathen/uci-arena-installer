#include "arena/ports/asset_search_index.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

namespace {

using arena::ports::IndexedAssetCandidate;
using arena::ports::IndexedAssetKind;
using arena::ports::IndexedAssetSearchResult;
using arena::ports::finalize_indexed_asset_search_result;

TEST_CASE("indexed asset finalization enforces the global result bound") {
    IndexedAssetSearchResult result;
    result.maximum_results = 1;
    result.candidates = {
        {IndexedAssetKind::syzygy, std::filesystem::path("c.rtbw")},
        {IndexedAssetKind::timing_profile, std::filesystem::path("b.json")},
        {IndexedAssetKind::opening_book, std::filesystem::path("a.bin")},
    };

    finalize_indexed_asset_search_result(result);

    REQUIRE(result.candidates.size() == 1);
    REQUIRE(result.truncated);
}

TEST_CASE("indexed asset finalization removes exact kind and path duplicates") {
    IndexedAssetSearchResult result;
    result.maximum_results = 8;
    result.candidates = {
        {IndexedAssetKind::timing_profile, std::filesystem::path("profile.json")},
        {IndexedAssetKind::opening_book, std::filesystem::path("book.bin")},
        {IndexedAssetKind::timing_profile, std::filesystem::path("profile.json")},
        {IndexedAssetKind::opening_book, std::filesystem::path("book.bin")},
        {IndexedAssetKind::syzygy, std::filesystem::path("table.rtbw")},
    };

    finalize_indexed_asset_search_result(result);

    REQUIRE(result.candidates.size() == 3);
    REQUIRE_FALSE(result.truncated);
}

TEST_CASE("indexed asset finalization preserves an existing truncation signal") {
    IndexedAssetSearchResult result;
    result.maximum_results = 4;
    result.truncated = true;
    result.candidates = {
        {IndexedAssetKind::opening_book, std::filesystem::path("book.bin")},
    };

    finalize_indexed_asset_search_result(result);

    REQUIRE(result.candidates.size() == 1);
    REQUIRE(result.truncated);
}

} // namespace
