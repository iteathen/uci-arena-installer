#include "arena/application/installer/tuf_canonical_json.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace arena::application::installer {
namespace {

constexpr std::size_t kMaximumOutputBytes = 64U * 1024U * 1024U;
constexpr std::size_t kMaximumDepth = 64U;

[[noreturn]] void reject(const char* message) {
    throw std::invalid_argument(message);
}

void checked_append(std::string& output, const std::string_view bytes) {
    if (bytes.size() > kMaximumOutputBytes - output.size()) {
        reject("TUF canonical JSON output exceeds capacity");
    }
    output.append(bytes);
}

void checked_append(std::string& output, const char byte) {
    if (output.size() == kMaximumOutputBytes) {
        reject("TUF canonical JSON output exceeds capacity");
    }
    output.push_back(byte);
}

[[nodiscard]] bool is_continuation(const unsigned char byte) noexcept {
    return (byte & 0xc0U) == 0x80U;
}

void validate_utf8_and_controls(const std::string_view text) {
    std::size_t offset = 0;
    while (offset < text.size()) {
        const auto first = static_cast<unsigned char>(text[offset]);
        if (first <= 0x7fU) {
            if (first <= 0x1fU) {
                reject("TUF canonical JSON string contains a control character");
            }
            ++offset;
            continue;
        }

        std::size_t length = 0;
        std::uint32_t scalar = 0;
        if (first >= 0xc2U && first <= 0xdfU) {
            length = 2;
            scalar = first & 0x1fU;
        } else if (first >= 0xe0U && first <= 0xefU) {
            length = 3;
            scalar = first & 0x0fU;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            length = 4;
            scalar = first & 0x07U;
        } else {
            reject("TUF canonical JSON string is not valid UTF-8");
        }

        if (length > text.size() - offset) {
            reject("TUF canonical JSON string is not valid UTF-8");
        }
        for (std::size_t index = 1; index < length; ++index) {
            const auto byte = static_cast<unsigned char>(text[offset + index]);
            if (!is_continuation(byte)) {
                reject("TUF canonical JSON string is not valid UTF-8");
            }
            scalar = (scalar << 6U) | (byte & 0x3fU);
        }

        const bool overlong =
            (length == 2 && scalar < 0x80U) ||
            (length == 3 && scalar < 0x800U) ||
            (length == 4 && scalar < 0x10000U);
        if (overlong || scalar > 0x10ffffU ||
            (scalar >= 0xd800U && scalar <= 0xdfffU)) {
            reject("TUF canonical JSON string is not valid UTF-8");
        }
        offset += length;
    }
}

void append_string(std::string& output, const std::string_view text) {
    validate_utf8_and_controls(text);
    checked_append(output, '"');
    std::size_t segment_start = 0;
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] != '\\' && text[index] != '"') {
            continue;
        }
        checked_append(output, text.substr(segment_start, index - segment_start));
        checked_append(output, '\\');
        checked_append(output, text[index]);
        segment_start = index + 1;
    }
    checked_append(output, text.substr(segment_start));
    checked_append(output, '"');
}

template <typename Integer>
void append_integer(std::string& output, const Integer value) {
    std::array<char, 32> buffer{};
    const auto [end, error] =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (error != std::errc{}) {
        throw std::runtime_error("TUF canonical JSON integer encoding failed");
    }
    checked_append(output,
                   std::string_view(buffer.data(),
                                    static_cast<std::size_t>(end - buffer.data())));
}

void encode_value(const nlohmann::json& value,
                  std::string& output,
                  const std::size_t depth) {
    using value_t = nlohmann::json::value_t;
    switch (value.type()) {
    case value_t::null:
        checked_append(output, "null");
        return;
    case value_t::boolean:
        checked_append(output, value.get<bool>() ? "true" : "false");
        return;
    case value_t::number_integer:
        append_integer(output, value.get<nlohmann::json::number_integer_t>());
        return;
    case value_t::number_unsigned:
        append_integer(output, value.get<nlohmann::json::number_unsigned_t>());
        return;
    case value_t::string:
        append_string(output, value.get_ref<const std::string&>());
        return;
    case value_t::array: {
        if (depth >= kMaximumDepth) {
            reject("TUF canonical JSON nesting exceeds depth limit");
        }
        checked_append(output, '[');
        bool first = true;
        for (const auto& element : value) {
            if (!first) {
                checked_append(output, ',');
            }
            first = false;
            encode_value(element, output, depth + 1);
        }
        checked_append(output, ']');
        return;
    }
    case value_t::object: {
        if (depth >= kMaximumDepth) {
            reject("TUF canonical JSON nesting exceeds depth limit");
        }
        std::vector<nlohmann::json::const_iterator> members;
        members.reserve(value.size());
        for (auto member = value.cbegin(); member != value.cend(); ++member) {
            validate_utf8_and_controls(member.key());
            members.push_back(member);
        }
        std::sort(members.begin(), members.end(), [](const auto& left,
                                                     const auto& right) {
            return left.key() < right.key();
        });

        checked_append(output, '{');
        bool first = true;
        for (const auto& member : members) {
            if (!first) {
                checked_append(output, ',');
            }
            first = false;
            append_string(output, member.key());
            checked_append(output, ':');
            encode_value(member.value(), output, depth + 1);
        }
        checked_append(output, '}');
        return;
    }
    case value_t::number_float:
    case value_t::binary:
    case value_t::discarded:
        reject("TUF canonical JSON value type is unsupported");
    }
    reject("TUF canonical JSON value type is unsupported");
}

} // namespace

std::string encode_tuf_olpc_canonical_json(const nlohmann::json& value) {
    std::string output;
    encode_value(value, output, 0);
    return output;
}

} // namespace arena::application::installer
