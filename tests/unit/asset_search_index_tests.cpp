#include "arena/ports/asset_search_index.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

namespace {

using arena::ports::IndexedAssetKind;
using arena::ports::IndexedAssetSearchResult;
using arena::ports::finalize_indexed_asset_search_result;
using arena::ports::indexed_asset_remaining_budget;

TEST_CASE("indexed asset search carries unused global budget forward") {
    for (std::size_t maximum = 0; maximum <= 4096; ++maximum) {
        for (std::size_t materialized = 0; materialized <= maximum + 1; ++materialized) {
            const std::size_t expected = materialized >= maximum
                ? 0
                : maximum - materialized;
            REQUIRE(indexed_asset_remaining_budget(maximum, materialized) == expected);
        }
    }

    REQUIRE(indexed_asset_remaining_budget(1, 0) == 1);
    REQUIRE(indexed_asset_remaining_budget(1, 1) == 0);
    REQUIRE(indexed_asset_remaining_budget(4, 0) == 4);
    REQUIRE(indexed_asset_remaining_budget(4, 1) == 3);
    REQUIRE(indexed_asset_remaining_budget(4, 3) == 1);
    REQUIRE(indexed_asset_remaining_budget(4, 4) == 0);
}

TEST_CASE("empty earlier asset kinds leave the complete budget for later kinds") {
    constexpr std::size_t maximum = 3;
    std::size_t materialized = 0;

    const std::size_t opening_budget =
        indexed_asset_remaining_budget(maximum, materialized);
    REQUIRE(opening_budget == 3);
    // Opening-book query returns no rows.
    REQUIRE(indexed_asset_remaining_budget(maximum, materialized) == 3);

    // Syzygy query fills two rows, leaving one slot for timing evidence.
    materialized += 2;
    REQUIRE(indexed_asset_remaining_budget(maximum, materialized) == 1);
    materialized += 1;
    REQUIRE(indexed_asset_remaining_budget(maximum, materialized) == 0);
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
