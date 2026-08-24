#include "arena/application/installer/exact_document.hpp"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace arena::application::installer {
namespace {

namespace ports = arena::ports;

constexpr std::uint64_t kMaximumDocumentBytes = UINT64_C(64) * 1024 * 1024;
constexpr std::size_t kReadChunkBytes = 64 * 1024;

class DocumentFailure final : public std::runtime_error {
  public:
    explicit DocumentFailure(const std::string_view message)
        : std::runtime_error(std::string{message}) {}
};

[[noreturn]] void fail(const std::string_view message) {
    throw DocumentFailure(message);
}

[[nodiscard]] std::string sha256_hex(
    const std::span<const std::byte> bytes) {
    if (sodium_init() < 0) {
        fail("exact document hashing is unavailable");
    }
    std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
    constexpr unsigned char empty_input{};
    const auto* input = bytes.empty()
                            ? &empty_input
                            : reinterpret_cast<const unsigned char*>(
                                  bytes.data());
    if (crypto_hash_sha256(
            digest.data(), input,
            static_cast<unsigned long long>(bytes.size())) != 0) {
        fail("exact document hashing failed");
    }

    constexpr std::array<char, 16> digits{
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
    };
    std::string encoded;
    encoded.resize(digest.size() * 2);
    for (std::size_t index = 0; index < digest.size(); ++index) {
        encoded[index * 2] = digits[digest[index] >> 4U];
        encoded[index * 2 + 1] = digits[digest[index] & 0x0fU];
    }
    return encoded;
}

[[nodiscard]] bool valid_utf8(const std::span<const std::byte> bytes) noexcept {
    std::size_t index{};
    const auto octet = [&](const std::size_t at) {
        return std::to_integer<unsigned int>(bytes[at]);
    };
    const auto continuation = [&](const std::size_t at) {
        return at < bytes.size() && (octet(at) & 0xc0U) == 0x80U;
    };

    while (index < bytes.size()) {
        const auto first = octet(index);
        if (first <= 0x7fU) {
            ++index;
            continue;
        }
        if (first >= 0xc2U && first <= 0xdfU) {
            if (!continuation(index + 1)) return false;
            index += 2;
            continue;
        }
        if (first >= 0xe0U && first <= 0xefU) {
            if (index + 2 >= bytes.size() || !continuation(index + 1) ||
                !continuation(index + 2)) {
                return false;
            }
            const auto second = octet(index + 1);
            if ((first == 0xe0U && second < 0xa0U) ||
                (first == 0xedU && second > 0x9fU)) {
                return false;
            }
            index += 3;
            continue;
        }
        if (first >= 0xf0U && first <= 0xf4U) {
            if (index + 3 >= bytes.size() || !continuation(index + 1) ||
                !continuation(index + 2) || !continuation(index + 3)) {
                return false;
            }
            const auto second = octet(index + 1);
            if ((first == 0xf0U && second < 0x90U) ||
                (first == 0xf4U && second > 0x8fU)) {
                return false;
            }
            index += 4;
            continue;
        }
        return false;
    }
    return true;
}

class BoundedJsonBuilder final : public nlohmann::json_sax<nlohmann::json> {
  public:
    explicit BoundedJsonBuilder(const BoundedJsonLimits& limits)
        : limits_(limits) {}

    bool null() override { return scalar(nullptr); }
    bool boolean(const bool value) override { return scalar(value); }
    bool number_integer(const number_integer_t value) override {
        return scalar(value);
    }
    bool number_unsigned(const number_unsigned_t value) override {
        return scalar(value);
    }
    bool number_float(const number_float_t value,
                      const string_t&) override {
        if (!limits_.allow_floating_point || !std::isfinite(value)) {
            return reject("bounded JSON floating-point value is not allowed");
        }
        return scalar(value);
    }
    bool string(string_t& value) override {
        if (value.size() > limits_.maximum_string_utf8_bytes) {
            return reject("bounded JSON string limit exceeded");
        }
        return scalar(std::move(value));
    }
    bool binary(binary_t&) override {
        return reject("bounded JSON value type is unsupported");
    }

    bool start_object(const std::size_t) override {
        return start_container(true);
    }

    bool key(string_t& value) override {
        if (frames_.empty() || !frames_.back().object ||
            frames_.back().pending_key.has_value()) {
            return reject("bounded JSON object structure is invalid");
        }
        if (value.size() > limits_.maximum_key_utf8_bytes) {
            return reject("bounded JSON key limit exceeded");
        }
        auto& frame = frames_.back();
        if (frame.item_count >= limits_.maximum_object_members) {
            return reject("bounded JSON object member limit exceeded");
        }
        if (frame.value.contains(value)) {
            return reject("bounded JSON duplicate key rejected");
        }
        ++frame.item_count;
        frame.pending_key = std::move(value);
        return true;
    }

    bool end_object() override { return end_container(true); }

    bool start_array(const std::size_t) override {
        return start_container(false);
    }

    bool end_array() override { return end_container(false); }

    bool parse_error(const std::size_t,
                     const std::string&,
                     const nlohmann::detail::exception&) override {
        return reject("bounded JSON syntax is invalid");
    }

    [[nodiscard]] std::string_view failure() const noexcept {
        return failure_;
    }

    [[nodiscard]] nlohmann::json take_result() {
        if (!failure_.empty() || !frames_.empty() || !root_complete_) {
            fail("bounded JSON result is incomplete");
        }
        return std::move(result_);
    }

  private:
    enum class DestinationKind {
        root,
        array,
        object,
    };

    struct Destination {
        DestinationKind kind{};
        std::string key;
    };

    struct Frame {
        bool object{};
        nlohmann::json value;
        Destination destination;
        std::size_t item_count{};
        std::optional<std::string> pending_key;
    };

    bool reject(const std::string_view reason) {
        if (failure_.empty()) failure_ = reason;
        return false;
    }

    [[nodiscard]] std::optional<Destination> prepare_value() {
        if (value_count_ >= limits_.maximum_values) {
            reject("bounded JSON value limit exceeded");
            return std::nullopt;
        }
        ++value_count_;

        if (frames_.empty()) {
            if (root_complete_) {
                reject("bounded JSON contains multiple root values");
                return std::nullopt;
            }
            return Destination{DestinationKind::root, {}};
        }

        auto& parent = frames_.back();
        if (parent.object) {
            if (!parent.pending_key.has_value()) {
                reject("bounded JSON object value has no key");
                return std::nullopt;
            }
            Destination destination{DestinationKind::object,
                                    std::move(*parent.pending_key)};
            parent.pending_key.reset();
            return destination;
        }

        if (parent.item_count >= limits_.maximum_array_items) {
            reject("bounded JSON array item limit exceeded");
            return std::nullopt;
        }
        ++parent.item_count;
        return Destination{DestinationKind::array, {}};
    }

    bool attach(nlohmann::json value, Destination destination) {
        switch (destination.kind) {
        case DestinationKind::root:
            if (root_complete_) {
                return reject("bounded JSON contains multiple root values");
            }
            result_ = std::move(value);
            root_complete_ = true;
            return true;
        case DestinationKind::array:
            if (frames_.empty() || frames_.back().object) {
                return reject("bounded JSON array structure is invalid");
            }
            frames_.back().value.push_back(std::move(value));
            return true;
        case DestinationKind::object:
            if (frames_.empty() || !frames_.back().object) {
                return reject("bounded JSON object structure is invalid");
            }
            frames_.back().value.emplace(std::move(destination.key),
                                         std::move(value));
            return true;
        }
        return reject("bounded JSON destination is invalid");
    }

    template <typename Value>
    bool scalar(Value&& value) {
        auto destination = prepare_value();
        if (!destination.has_value()) return false;
        return attach(nlohmann::json(std::forward<Value>(value)),
                      std::move(*destination));
    }

    bool start_container(const bool object) {
        if (frames_.size() >= limits_.maximum_depth) {
            return reject("bounded JSON depth limit exceeded");
        }
        auto destination = prepare_value();
        if (!destination.has_value()) return false;
        Frame frame;
        frame.object = object;
        frame.value = object ? nlohmann::json::object()
                             : nlohmann::json::array();
        frame.destination = std::move(*destination);
        frames_.push_back(std::move(frame));
        return true;
    }

    bool end_container(const bool object) {
        if (frames_.empty() || frames_.back().object != object ||
            (object && frames_.back().pending_key.has_value())) {
            return reject("bounded JSON container structure is invalid");
        }
        auto frame = std::move(frames_.back());
        frames_.pop_back();
        return attach(std::move(frame.value), std::move(frame.destination));
    }

    const BoundedJsonLimits& limits_;
    std::vector<Frame> frames_;
    nlohmann::json result_;
    std::size_t value_count_{};
    bool root_complete_{};
    std::string_view failure_;
};

void validate_json_limits(const BoundedJsonLimits& limits) {
    if (limits.maximum_depth == 0 || limits.maximum_values == 0 ||
        limits.maximum_object_members == 0 ||
        limits.maximum_array_items == 0 ||
        limits.maximum_key_utf8_bytes == 0 ||
        limits.maximum_string_utf8_bytes == 0) {
        fail("bounded JSON limits are invalid");
    }
}

ExactDocument read_child_impl(
    const ports::AdmittedDirectory& directory,
    const std::string_view leaf,
    const ExactDocumentLimits& limits) {
    if (limits.maximum_bytes == 0 ||
        limits.maximum_bytes > kMaximumDocumentBytes) {
        fail("exact document byte limit is invalid");
    }
    if (directory.observe_path_binding() !=
        ports::PathBindingState::same_object) {
        fail("exact document parent binding is not current");
    }

    auto file = directory.open_regular_file(leaf);
    const auto initial = file->initial_observation();
    if (initial.kind != ports::FilesystemObjectKind::regular_file) {
        fail("exact document child kind is invalid");
    }
    if (initial.size_bytes > limits.maximum_bytes) {
        fail("exact document initial size exceeds the limit");
    }

    std::vector<std::byte> bytes;
    bytes.reserve(static_cast<std::size_t>(initial.size_bytes));
    std::array<std::byte, kReadChunkBytes> chunk{};
    std::uint64_t offset{};
    while (true) {
        const auto remaining = limits.maximum_bytes - offset;
        const auto request = remaining == 0
                                 ? std::size_t{1}
                                 : static_cast<std::size_t>((std::min)(
                                       static_cast<std::uint64_t>(chunk.size()),
                                       remaining));
        const auto count = file->read_at(
            offset, std::span<std::byte>{chunk}.first(request));
        if (count > request) {
            fail("exact document reader returned an invalid byte count");
        }
        if (count == 0) break;
        if (count > remaining) {
            fail("exact document size exceeds the limit");
        }
        bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + count);
        offset += count;
    }

    if (limits.require_nonempty && bytes.empty()) {
        fail("exact document must not be empty");
    }

    const auto final = file->observe_open_object();
    if (final.kind != ports::FilesystemObjectKind::regular_file ||
        final.kind != initial.kind ||
        final.object_identity != initial.object_identity ||
        final.metadata_identity != initial.metadata_identity ||
        final.size_bytes != initial.size_bytes) {
        fail("exact document open object changed during the read");
    }
    if (final.size_bytes != bytes.size()) {
        fail("exact document byte count is not stable");
    }
    if (file->observe_path_binding() !=
        ports::PathBindingState::same_object) {
        fail("exact document child binding changed during the read");
    }
    if (directory.observe_path_binding() !=
        ports::PathBindingState::same_object) {
        fail("exact document parent binding changed during the read");
    }

    auto digest = sha256_hex(bytes);
    return {std::move(bytes), std::move(digest), initial, final};
}

} // namespace

ExactDocument ExactDocumentReader::read_child(
    const ports::AdmittedDirectory& directory,
    const std::string_view leaf,
    const ExactDocumentLimits& limits) const {
    try {
        return read_child_impl(directory, leaf, limits);
    } catch (const DocumentFailure&) {
        throw;
    } catch (const ports::FilesystemIdentityException&) {
        throw;
    } catch (const std::bad_alloc&) {
        throw;
    } catch (...) {
        fail("exact document operation failed");
    }
}

nlohmann::json parse_bounded_json(
    const ExactDocument& document,
    const BoundedJsonLimits& limits) {
    try {
        validate_json_limits(limits);
        if (!valid_utf8(document.bytes)) {
            fail("bounded JSON UTF-8 is invalid");
        }
        BoundedJsonBuilder builder(limits);
        constexpr char empty_input{};
        const auto* begin = document.bytes.empty()
                                ? &empty_input
                                : reinterpret_cast<const char*>(
                                      document.bytes.data());
        const bool accepted = nlohmann::json::sax_parse(
            begin, begin + document.bytes.size(), &builder,
            nlohmann::json::input_format_t::json, true, false);
        if (!accepted) {
            fail(builder.failure().empty()
                     ? "bounded JSON input is invalid"
                     : builder.failure());
        }
        return builder.take_result();
    } catch (const DocumentFailure&) {
        throw;
    } catch (const std::bad_alloc&) {
        throw;
    } catch (...) {
        fail("bounded JSON operation failed");
    }
}

} // namespace arena::application::installer
