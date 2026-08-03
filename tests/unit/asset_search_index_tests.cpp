#include "arena/ports/asset_search_index.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

namespace {

using arena::ports::IndexedAssetKind;
using arena::ports::IndexedAssetSearchResult;
using arena::ports::finalize_indexed_asset_search_result;
using arena::ports::indexed_asset_kind_budget;

TEST_CASE("indexed asset category budgets sum to the global maximum") {
    for (std::size_t maximum = 0; maximum <= 64; ++maximum) {
        const std::size_t opening = indexed_asset_kind_budget(
            maximum, IndexedAssetKind::opening_book);
        const std::size_t syzygy = indexed_asset_kind_budget(
            maximum, IndexedAssetKind::syzygy);
        const std::size_t timing = indexed_asset_kind_budget(
            maximum, IndexedAssetKind::timing_profile);
        REQUIRE(opening + syzygy + timing == maximum);
        REQUIRE(opening >= syzygy);
        REQUIRE(syzygy >= timing);
        REQUIRE(opening - timing <= 1);
    }

    REQUIRE(indexed_asset_kind_budget(1, IndexedAssetKind::opening_book) == 1);
    REQUIRE(indexed_asset_kind_budget(1, IndexedAssetKind::syzygy) == 0);
    REQUIRE(indexed_asset_kind_budget(2, IndexedAssetKind::opening_book) == 1);
    REQUIRE(indexed_asset_kind_budget(2, IndexedAssetKind::syzygy) == 1);
    REQUIRE(indexed_asset_kind_budget(4, IndexedAssetKind::opening_book) == 2);
    REQUIRE(indexed_asset_kind_budget(4, IndexedAssetKind::syzygy) == 1);
    REQUIRE(indexed_asset_kind_budget(4, IndexedAssetKind::timing_profile) == 1);
}

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
    REQUIRE(result.candidates.front().kind == IndexedAssetKind::opening_book);
    REQUIRE(result.candidates.front().path == std::filesystem::path("a.bin"));
    REQUIRE(result.truncated);
}

TEST_CASE("indexed asset finalization orders and deduplicates without changing kind") {
    IndexedAssetSearchResult result;
    result.maximum_results = 8;
    result.candidates = {
        {IndexedAssetKind::timing_profile, std::filesystem::path("z-profile.json")},
        {IndexedAssetKind::opening_book, std::filesystem::path("z-book.bin")},
        {IndexedAssetKind::timing_profile, std::filesystem::path("z-profile.json")},
        {IndexedAssetKind::opening_book, std::filesystem::path("a-book.bin")},
        {IndexedAssetKind::syzygy, std::filesystem::path("table.rtbw")},
    };

    finalize_indexed_asset_search_result(result);

    REQUIRE(result.candidates.size() == 4);
    REQUIRE(result.candidates.at(0).kind == IndexedAssetKind::opening_book);
    REQUIRE(result.candidates.at(0).path == std::filesystem::path("a-book.bin"));
    REQUIRE(result.candidates.at(1).kind == IndexedAssetKind::opening_book);
    REQUIRE(result.candidates.at(1).path == std::filesystem::path("z-book.bin"));
    REQUIRE(result.candidates.at(2).kind == IndexedAssetKind::syzygy);
    REQUIRE(result.candidates.at(3).kind == IndexedAssetKind::timing_profile);
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

TEST_CASE("indexed asset finalization clears candidates under a zero result bound") {
    IndexedAssetSearchResult result;
    result.maximum_results = 0;
    result.candidates = {
        {IndexedAssetKind::opening_book, std::filesystem::path("book.bin")},
    };

    finalize_indexed_asset_search_result(result);

    REQUIRE(result.candidates.empty());
    REQUIRE(result.truncated);
}

} // namespace
