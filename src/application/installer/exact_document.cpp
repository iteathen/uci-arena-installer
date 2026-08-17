#include "arena/application/installer/exact_document.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <sodium.h>

namespace arena::application::installer {
namespace {

constexpr std::uint64_t kMaximumExactDocumentBytes = UINT64_C(67108864);
constexpr std::size_t kReadChunkBytes = 64U * 1024U;
constexpr std::size_t kMaximumDiagnosticBytes = 512U;

[[noreturn]] void throw_document_error(std::string message) {
    if (message.size() > kMaximumDiagnosticBytes) {
        message.resize(kMaximumDiagnosticBytes);
    }
    throw std::runtime_error(std::move(message));
}

void validate_document_limits(const ExactDocumentLimits& limits) {
    if (limits.maximum_bytes == 0 ||
        limits.maximum_bytes > kMaximumExactDocumentBytes) {
        throw_document_error("exact document byte limit is invalid");
    }
}

[[nodiscard]] std::string sha256_hex(const std::span<const std::byte> bytes) {
    if (sodium_init() < 0) {
        throw_document_error("exact document hashing is unavailable");
    }

    crypto_hash_sha256_state state{};
    if (crypto_hash_sha256_init(&state) != 0) {
        throw_document_error("exact document hashing initialization failed");
    }
    if (!bytes.empty() &&
        crypto_hash_sha256_update(
            &state,
            reinterpret_cast<const unsigned char*>(bytes.data()),
            static_cast<unsigned long long>(bytes.size())) != 0) {
        throw_document_error("exact document hashing failed");
    }

    std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
    if (crypto_hash_sha256_final(&state, digest.data()) != 0) {
        throw_document_error("exact document hashing finalization failed");
    }

    constexpr std::string_view hex = "0123456789abcdef";
    std::string result;
    result.resize(digest.size() * 2U);
    for (std::size_t index = 0; index < digest.size(); ++index) {
        const auto value = digest[index];
        result[index * 2U] = hex[(value >> 4U) & 0x0fU];
        result[index * 2U + 1U] = hex[value & 0x0fU];
    }
    return result;
}

[[nodiscard]] bool is_continuation(const unsigned char value) noexcept {
    return (value & 0xc0U) == 0x80U;
}

[[nodiscard]] bool valid_utf8(const std::span<const std::byte> bytes) noexcept {
    std::size_t index = 0;
    while (index < bytes.size()) {
        const auto first = std::to_integer<unsigned char>(bytes[index]);
        if (first <= 0x7fU) {
            ++index;
            continue;
        }

        if (first >= 0xc2U && first <= 0xdfU) {
            if (index + 1U >= bytes.size()) {
                return false;
            }
            const auto second =
                std::to_integer<unsigned char>(bytes[index + 1U]);
            if (!is_continuation(second)) {
                return false;
            }
            index += 2U;
            continue;
        }

        if (first >= 0xe0U && first <= 0xefU) {
            if (index + 2U >= bytes.size()) {
                return false;
            }
            const auto second =
                std::to_integer<unsigned char>(bytes[index + 1U]);
            const auto third =
                std::to_integer<unsigned char>(bytes[index + 2U]);
            if (!is_continuation(third)) {
                return false;
            }
            if (first == 0xe0U) {
                if (second < 0xa0U || second > 0xbfU) {
                    return false;
                }
            } else if (first == 0xedU) {
                if (second < 0x80U || second > 0x9fU) {
                    return false;
                }
            } else if (!is_continuation(second)) {
                return false;
            }
            index += 3U;
            continue;
        }

        if (first >= 0xf0U && first <= 0xf4U) {
            if (index + 3U >= bytes.size()) {
                return false;
            }
            const auto second =
                std::to_integer<unsigned char>(bytes[index + 1U]);
            const auto third =
                std::to_integer<unsigned char>(bytes[index + 2U]);
            const auto fourth =
                std::to_integer<unsigned char>(bytes[index + 3U]);
            if (!is_continuation(third) || !is_continuation(fourth)) {
                return false;
            }
            if (first == 0xf0U) {
                if (second < 0x90U || second > 0xbfU) {
                    return false;
                }
            } else if (first == 0xf4U) {
                if (second < 0x80U || second > 0x8fU) {
                    return false;
                }
            } else if (!is_continuation(second)) {
                return false;
            }
            index += 4U;
            continue;
        }

        return false;
    }
    return true;
}

class BoundedJsonSax final : public nlohmann::json_sax<nlohmann::json> {
  public:
    using json = nlohmann::json;
    using number_integer_t = json::number_integer_t;
    using number_unsigned_t = json::number_unsigned_t;
    using number_float_t = json::number_float_t;
    using string_t = json::string_t;
    using binary_t = json::binary_t;

    explicit BoundedJsonSax(const BoundedJsonLimits& limits)
        : limits_(limits) {}

    bool null() override {
        return append_scalar(nullptr);
    }

    bool boolean(const bool value) override {
        return append_scalar(value);
    }

    bool number_integer(const number_integer_t value) override {
        return append_scalar(value);
    }

    bool number_unsigned(const number_unsigned_t value) override {
        return append_scalar(value);
    }

    bool number_float(
        const number_float_t value,
        const string_t&) override {
        if (!limits_.allow_floating_point || !std::isfinite(value)) {
            return fail("bounded JSON floating-point value is not permitted");
        }
        return append_scalar(value);
    }

    bool string(string_t& value) override {
        if (value.size() > limits_.maximum_string_utf8_bytes) {
            return fail("bounded JSON string exceeds configured limit");
        }
        return append_scalar(value);
    }

    bool binary(binary_t&) override {
        return fail("bounded JSON binary values are not supported");
    }

    bool start_object(const std::size_t elements) override {
        if (elements != unknown_size() &&
            elements > limits_.maximum_object_members) {
            return fail("bounded JSON object exceeds configured member limit");
        }
        if (!begin_value()) {
            return false;
        }
        stack_.push_back(
            Frame{FrameKind::object, nlohmann::json::object()});
        return true;
    }

    bool key(string_t& value) override {
        if (stack_.empty() ||
            stack_.back().kind != FrameKind::object ||
            stack_.back().pending_key.has_value()) {
            return fail("bounded JSON object structure is invalid");
        }
        if (value.size() > limits_.maximum_key_utf8_bytes) {
            return fail("bounded JSON key exceeds configured limit");
        }

        auto& frame = stack_.back();
        if (frame.item_count >= limits_.maximum_object_members) {
            return fail("bounded JSON object exceeds configured member limit");
        }
        if (!frame.keys.emplace(value).second) {
            return fail("bounded JSON object contains a duplicate key");
        }

        frame.pending_key = value;
        ++frame.item_count;
        return true;
    }

    bool end_object() override {
        if (stack_.empty() ||
            stack_.back().kind != FrameKind::object ||
            stack_.back().pending_key.has_value()) {
            return fail("bounded JSON object structure is invalid");
        }
        auto value = std::move(stack_.back().value);
        stack_.pop_back();
        return attach_precounted(std::move(value));
    }

    bool start_array(const std::size_t elements) override {
        if (elements != unknown_size() &&
            elements > limits_.maximum_array_items) {
            return fail("bounded JSON array exceeds configured item limit");
        }
        if (!begin_value()) {
            return false;
        }
        stack_.push_back(
            Frame{FrameKind::array, nlohmann::json::array()});
        return true;
    }

    bool end_array() override {
        if (stack_.empty() ||
            stack_.back().kind != FrameKind::array) {
            return fail("bounded JSON array structure is invalid");
        }
        auto value = std::move(stack_.back().value);
        stack_.pop_back();
        return attach_precounted(std::move(value));
    }

    bool parse_error(
        const std::size_t,
        const std::string&,
        const nlohmann::detail::exception&) override {
        return fail("bounded JSON syntax is invalid");
    }

    [[nodiscard]] const std::string& error_message() const noexcept {
        return error_;
    }

    [[nodiscard]] nlohmann::json take_result() {
        if (!root_set_ || !stack_.empty()) {
            throw_document_error("bounded JSON did not produce one complete value");
        }
        return std::move(root_);
    }

  private:
    enum class FrameKind {
        object,
        array,
    };

    struct Frame {
        FrameKind kind;
        nlohmann::json value;
        std::unordered_set<std::string> keys{};
        std::optional<std::string> pending_key{};
        std::size_t item_count{};
    };

    [[nodiscard]] static constexpr std::size_t unknown_size() noexcept {
        return static_cast<std::size_t>(-1);
    }

    bool fail(std::string message) {
        if (error_.empty()) {
            if (message.size() > kMaximumDiagnosticBytes) {
                message.resize(kMaximumDiagnosticBytes);
            }
            error_ = std::move(message);
        }
        return false;
    }

    bool ensure_depth() {
        const auto depth = stack_.size() + 1U;
        if (depth > limits_.maximum_depth) {
            return fail("bounded JSON exceeds configured depth");
        }
        return true;
    }

    bool ensure_parent_slot() {
        if (stack_.empty()) {
            if (root_set_) {
                return fail("bounded JSON contains more than one root value");
            }
            return true;
        }

        auto& parent = stack_.back();
        if (parent.kind == FrameKind::array) {
            if (parent.item_count >= limits_.maximum_array_items) {
                return fail("bounded JSON array exceeds configured item limit");
            }
            return true;
        }

        if (!parent.pending_key.has_value()) {
            return fail("bounded JSON object value has no key");
        }
        return true;
    }

    bool begin_value() {
        if (!ensure_depth() || !ensure_parent_slot()) {
            return false;
        }
        if (value_count_ >= limits_.maximum_values) {
            return fail("bounded JSON exceeds configured value limit");
        }
        ++value_count_;
        return true;
    }

    template <typename Value>
    bool append_scalar(Value&& value) {
        if (!begin_value()) {
            return false;
        }
        return attach_precounted(
            nlohmann::json(std::forward<Value>(value)));
    }

    bool attach_precounted(nlohmann::json value) {
        if (stack_.empty()) {
            if (root_set_) {
                return fail("bounded JSON contains more than one root value");
            }
            root_ = std::move(value);
            root_set_ = true;
            return true;
        }

        auto& parent = stack_.back();
        if (parent.kind == FrameKind::array) {
            if (parent.item_count >= limits_.maximum_array_items) {
                return fail("bounded JSON array exceeds configured item limit");
            }
            parent.value.push_back(std::move(value));
            ++parent.item_count;
            return true;
        }

        if (!parent.pending_key.has_value()) {
            return fail("bounded JSON object value has no key");
        }
        parent.value[*parent.pending_key] = std::move(value);
        parent.pending_key.reset();
        return true;
    }

    const BoundedJsonLimits& limits_;
    std::vector<Frame> stack_;
    nlohmann::json root_;
    bool root_set_{false};
    std::size_t value_count_{};
    std::string error_;
};

} // namespace

ExactDocument ExactDocumentReader::read_child(
    const arena::ports::AdmittedDirectory& directory,
    const std::string_view leaf,
    const ExactDocumentLimits& limits) const {
    validate_document_limits(limits);

    if (directory.observe_path_binding() !=
        arena::ports::PathBindingState::same_object) {
        throw_document_error(
            "exact document parent binding is not stable before read");
    }

    auto file = directory.open_regular_file(leaf);
    const auto initial = file->initial_observation();
    if (initial.kind != arena::ports::FilesystemObjectKind::regular_file) {
        throw_document_error("exact document child is not a regular file");
    }
    if (initial.size_bytes > limits.maximum_bytes) {
        throw_document_error("exact document exceeds configured byte limit");
    }

    std::vector<std::byte> bytes;
    bytes.reserve(static_cast<std::size_t>(initial.size_bytes));
    std::array<std::byte, kReadChunkBytes> chunk{};

    std::uint64_t offset = 0;
    while (offset < limits.maximum_bytes) {
        const auto remaining = limits.maximum_bytes - offset;
        const auto request = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, chunk.size()));
        const auto read = file->read_at(
            offset, std::span<std::byte>{chunk.data(), request});
        if (read > request) {
            throw_document_error(
                "exact document reader returned more bytes than requested");
        }
        if (read == 0) {
            break;
        }
        bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + read);
        offset += static_cast<std::uint64_t>(read);
    }

    if (offset == limits.maximum_bytes) {
        std::array<std::byte, 1> overflow_probe{};
        const auto extra = file->read_at(offset, overflow_probe);
        if (extra > overflow_probe.size()) {
            throw_document_error(
                "exact document overflow probe returned an invalid count");
        }
        if (extra != 0) {
            throw_document_error("exact document exceeds configured byte limit");
        }
    }

    if (limits.require_nonempty && bytes.empty()) {
        throw_document_error("exact document must not be empty");
    }

    const auto final = file->observe_open_object();
    if (final.kind != initial.kind ||
        final.size_bytes != initial.size_bytes ||
        final.object_identity != initial.object_identity ||
        final.metadata_identity != initial.metadata_identity) {
        throw_document_error("exact document changed during read");
    }
    if (bytes.size() != final.size_bytes) {
        throw_document_error(
            "exact document byte count does not match stable observation");
    }

    if (file->observe_path_binding() !=
        arena::ports::PathBindingState::same_object) {
        throw_document_error("exact document child binding changed during read");
    }
    if (directory.observe_path_binding() !=
        arena::ports::PathBindingState::same_object) {
        throw_document_error("exact document parent binding changed during read");
    }

    auto digest = sha256_hex(bytes);
    return ExactDocument{
        std::move(bytes),
        std::move(digest),
        initial,
        final,
    };
}

nlohmann::json parse_bounded_json(
    const ExactDocument& document,
    const BoundedJsonLimits& limits) {
    if (!valid_utf8(document.bytes)) {
        throw_document_error("bounded JSON input is not valid UTF-8");
    }

    BoundedJsonSax sax{limits};
    const char* begin = document.bytes.empty()
                            ? ""
                            : reinterpret_cast<const char*>(
                                  document.bytes.data());
    const char* end = begin + document.bytes.size();

    const bool parsed = nlohmann::json::sax_parse(
        begin,
        end,
        &sax,
        nlohmann::json::input_format_t::json,
        true,
        false);
    if (!parsed) {
        if (sax.error_message().empty()) {
            throw_document_error("bounded JSON parsing failed");
        }
        throw_document_error(sax.error_message());
    }
    return sax.take_result();
}

} // namespace arena::application::installer
