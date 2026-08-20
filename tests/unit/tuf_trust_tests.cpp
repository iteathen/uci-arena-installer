#include "arena/application/installer/tuf_canonical_json.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace {

using arena::application::installer::encode_tuf_olpc_canonical_json;
using nlohmann::json;

} // namespace

TEST_CASE("TUF canonical JSON matches OLPC reference vectors") {
    REQUIRE(encode_tuf_olpc_canonical_json("") == "\"\"");
    REQUIRE(encode_tuf_olpc_canonical_json(json::array({1, 2, 3})) ==
            "[1,2,3]");
    REQUIRE(encode_tuf_olpc_canonical_json(json::array()) == "[]");
    REQUIRE(encode_tuf_olpc_canonical_json(json::object()) == "{}");
    REQUIRE(encode_tuf_olpc_canonical_json(json{{"A", json::array({99})}}) ==
            "{\"A\":[99]}");
    REQUIRE(encode_tuf_olpc_canonical_json(json{{"A", true}}) ==
            "{\"A\":true}");
    REQUIRE(encode_tuf_olpc_canonical_json(json{{"B", false}}) ==
            "{\"B\":false}");
    REQUIRE(encode_tuf_olpc_canonical_json(json{{"x", 3}, {"y", 2}}) ==
            "{\"x\":3,\"y\":2}");
    REQUIRE(encode_tuf_olpc_canonical_json(json{{"x", 3}, {"y", nullptr}}) ==
            "{\"x\":3,\"y\":null}");
}

TEST_CASE("TUF canonical JSON escapes only quote and backslash") {
    REQUIRE(encode_tuf_olpc_canonical_json("\"") == "\"\\\"\"");
    REQUIRE(encode_tuf_olpc_canonical_json("\\") == "\"\\\\\"");
    REQUIRE(encode_tuf_olpc_canonical_json("\\\"") ==
            "\"\\\\\\\"\"");
    REQUIRE(encode_tuf_olpc_canonical_json(
                std::string{"caf\xc3\xa9 \xe9\x9b\xaa"}) ==
            std::string{"\"caf\xc3\xa9 \xe9\x9b\xaa\""});
}

TEST_CASE("TUF canonical JSON emits the complete integer domain") {
    REQUIRE(encode_tuf_olpc_canonical_json(-1) == "-1");
    REQUIRE(encode_tuf_olpc_canonical_json(0) == "0");
    REQUIRE(encode_tuf_olpc_canonical_json(
                std::numeric_limits<std::int64_t>::min()) ==
            "-9223372036854775808");
    REQUIRE(encode_tuf_olpc_canonical_json(
                std::numeric_limits<std::int64_t>::max()) ==
            "9223372036854775807");
    REQUIRE(encode_tuf_olpc_canonical_json(
                std::numeric_limits<std::uint64_t>::max()) ==
            "18446744073709551615");
}

TEST_CASE("TUF canonical JSON sorts ASCII and non-ASCII keys by UTF-8 bytes") {
    json value = json::object();
    value["雪"] = 4;
    value["é"] = 3;
    value["z"] = 2;
    value["A"] = 1;

    REQUIRE(encode_tuf_olpc_canonical_json(value) ==
            std::string{"{\"A\":1,\"z\":2,\"\xc3\xa9\":3,"
                        "\"\xe9\x9b\xaa\":4}"});

    const json nested{{"outer", json{{"b", json::array({2, 1})},
                                      {"a", json{{"y", false}, {"x", true}}}}}};
    REQUIRE(encode_tuf_olpc_canonical_json(nested) ==
            "{\"outer\":{\"a\":{\"x\":true,\"y\":false},"
            "\"b\":[2,1]}}");
}

TEST_CASE("TUF canonical JSON rejects every unsupported numeric type") {
    REQUIRE_THROWS_AS(encode_tuf_olpc_canonical_json(0.0),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(encode_tuf_olpc_canonical_json(-1.5),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(encode_tuf_olpc_canonical_json(
                          std::numeric_limits<double>::infinity()),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(encode_tuf_olpc_canonical_json(
                          std::numeric_limits<double>::quiet_NaN()),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(encode_tuf_olpc_canonical_json(json::binary({1, 2, 3})),
                      std::invalid_argument);

    const json discarded = json::parse("{", nullptr, false);
    REQUIRE(discarded.is_discarded());
    REQUIRE_THROWS_AS(encode_tuf_olpc_canonical_json(discarded),
                      std::invalid_argument);
}

TEST_CASE("TUF canonical JSON rejects invalid UTF-8 in values and keys") {
    const std::string invalid_overlong{"\xc0\xaf", 2};
    const std::string invalid_surrogate{"\xed\xa0\x80", 3};
    REQUIRE_THROWS_AS(encode_tuf_olpc_canonical_json(invalid_overlong),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(encode_tuf_olpc_canonical_json(invalid_surrogate),
                      std::invalid_argument);

    json invalid_key = json::object();
    invalid_key[invalid_overlong] = true;
    REQUIRE_THROWS_AS(encode_tuf_olpc_canonical_json(invalid_key),
                      std::invalid_argument);
}

TEST_CASE("TUF canonical JSON rejects the complete control-character range") {
    for (unsigned int control = 0; control <= 0x1fU; ++control) {
        const std::string value(1, static_cast<char>(control));
        REQUIRE_THROWS_AS(encode_tuf_olpc_canonical_json(value),
                          std::invalid_argument);

        json keyed = json::object();
        keyed[value] = nullptr;
        REQUIRE_THROWS_AS(encode_tuf_olpc_canonical_json(keyed),
                          std::invalid_argument);
    }
}

TEST_CASE("TUF canonical JSON enforces depth and output capacity") {
    json at_limit = nullptr;
    for (std::size_t depth = 0; depth < 64; ++depth) {
        at_limit = json::array({std::move(at_limit)});
    }
    REQUIRE_NOTHROW(encode_tuf_olpc_canonical_json(at_limit));

    json over_limit = json::array({std::move(at_limit)});
    REQUIRE_THROWS_AS(encode_tuf_olpc_canonical_json(over_limit),
                      std::invalid_argument);

    const std::string oversized(64U * 1024U * 1024U, 'x');
    REQUIRE_THROWS_AS(encode_tuf_olpc_canonical_json(oversized),
                      std::invalid_argument);
}

TEST_CASE("TUF canonical JSON failures do not echo input contents") {
    const std::string secret = "fixture-secret-value";
    try {
        (void)encode_tuf_olpc_canonical_json(secret + '\n');
        FAIL("control-bearing secret unexpectedly encoded");
    } catch (const std::invalid_argument& error) {
        REQUIRE(std::string_view{error.what()}.size() <= 512);
        REQUIRE(std::string_view{error.what()}.find(secret) ==
                std::string_view::npos);
    }
}
