#include "arena/application/installer/artifact_acquirer.hpp"

#include <curl/curl.h>
#include <sodium.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace arena::application::installer {
namespace {

constexpr std::size_t kMaximumSources = 16U;
constexpr std::size_t kMaximumSourceUtf8Bytes = 4096U;
constexpr std::size_t kMaximumPathUtf8Bytes = 131072U;
constexpr std::size_t kMaximumLeafUtf8Bytes = 4096U;
constexpr std::size_t kMaximumDiagnosticBytes = 1024U;
constexpr std::size_t kTransferChunkBytes = 65536U;
constexpr auto kMinimumCacheLockTimeout = std::chrono::milliseconds{1};
constexpr auto kMaximumCacheLockTimeout = std::chrono::seconds{120};
constexpr auto kMaximumConnectTimeout = std::chrono::seconds{120};
constexpr auto kMinimumOverallTimeout = std::chrono::seconds{1};
constexpr auto kMaximumOverallTimeout = std::chrono::hours{168};
constexpr std::string_view kHttpsProtocols = "https";
constexpr long kHttpsFollowLocation = 1L;
constexpr long kHttpsFailOnError = 1L;
constexpr long kHttpsVerifyPeer = 1L;
constexpr long kHttpsVerifyHost = 2L;
constexpr long kHttpsRedirectLimit = 5L;
constexpr long kLowSpeedBytesPerSecond = 1024L;
constexpr long kLowSpeedWindowSeconds = 120L;

class SourceFailure final : public std::runtime_error {
  public:
    explicit SourceFailure(std::string category)
        : std::runtime_error("artifact source failed"),
          category_(std::move(category)) {}

    [[nodiscard]] const std::string& category() const noexcept {
        return category_;
    }

  private:
    std::string category_;
};

class CancellationFailure final : public std::runtime_error {
  public:
    CancellationFailure()
        : std::runtime_error("artifact acquisition was cancelled") {}
};

class DeadlineFailure final : public std::runtime_error {
  public:
    DeadlineFailure()
        : std::runtime_error("artifact acquisition deadline expired") {}
};

class CacheFailure final : public std::runtime_error {
  public:
    explicit CacheFailure(const char* message) : std::runtime_error(message) {}
};

enum class SourceKind {
    https,
    catalog_relative,
};

struct ParsedSource final {
    SourceKind kind{};
    std::string value;
    std::vector<std::string> components;
};

struct VerificationResult final {
    bool exact{};
    ports::FilesystemObservation initial{};
};

[[nodiscard]] bool has_ascii_control(const std::string_view text) noexcept {
    return std::ranges::any_of(text, [](const unsigned char value) {
        return value < 0x20U || value == 0x7fU;
    });
}

[[nodiscard]] bool valid_utf8(const std::string_view text) noexcept {
    const auto* data = reinterpret_cast<const unsigned char*>(text.data());
    std::size_t index = 0;
    while (index < text.size()) {
        const auto first = data[index];
        if (first <= 0x7fU) {
            ++index;
            continue;
        }
        if (first >= 0xc2U && first <= 0xdfU) {
            if (index + 1U >= text.size() ||
                (data[index + 1U] & 0xc0U) != 0x80U) {
                return false;
            }
            index += 2U;
            continue;
        }
        if (first >= 0xe0U && first <= 0xefU) {
            if (index + 2U >= text.size()) {
                return false;
            }
            const auto second = data[index + 1U];
            const auto third = data[index + 2U];
            if ((third & 0xc0U) != 0x80U) {
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
            } else if ((second & 0xc0U) != 0x80U) {
                return false;
            }
            index += 3U;
            continue;
        }
        if (first >= 0xf0U && first <= 0xf4U) {
            if (index + 3U >= text.size()) {
                return false;
            }
            const auto second = data[index + 1U];
            if ((data[index + 2U] & 0xc0U) != 0x80U ||
                (data[index + 3U] & 0xc0U) != 0x80U) {
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
            } else if ((second & 0xc0U) != 0x80U) {
                return false;
            }
            index += 4U;
            continue;
        }
        return false;
    }
    return true;
}

[[nodiscard]] bool is_lower_hex_digest(
    const std::string_view digest) noexcept {
    if (digest.size() != 64U) {
        return false;
    }
    return std::ranges::all_of(digest, [](const char value) {
        return (value >= '0' && value <= '9') ||
               (value >= 'a' && value <= 'f');
    });
}

[[nodiscard]] std::string path_as_utf8(
    const std::filesystem::path& path) {
    const auto encoded = path.generic_u8string();
    return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

void validate_absolute_path(
    const std::filesystem::path& path,
    const bool allow_root) {
    if (path.empty() || !path.is_absolute() ||
        path.lexically_normal() != path ||
        (!allow_root && path == path.root_path())) {
        throw std::runtime_error("artifact acquisition path is invalid");
    }

    std::string encoded;
    try {
        encoded = path_as_utf8(path);
    } catch (const std::bad_alloc&) {
        throw;
    } catch (...) {
        throw std::runtime_error("artifact acquisition path is invalid");
    }
    if (encoded.empty() || encoded.size() > kMaximumPathUtf8Bytes ||
        has_ascii_control(encoded) || !valid_utf8(encoded)) {
        throw std::runtime_error("artifact acquisition path is invalid");
    }
}

[[nodiscard]] bool ascii_equal_ci(
    const char left,
    const char right) noexcept {
    const auto lower = [](const unsigned char value) {
        return value >= 'A' && value <= 'Z'
                   ? static_cast<unsigned char>(value + ('a' - 'A'))
                   : value;
    };
    return lower(static_cast<unsigned char>(left)) ==
           lower(static_cast<unsigned char>(right));
}

[[nodiscard]] bool starts_with_https(
    const std::string_view value) noexcept {
    constexpr std::string_view prefix = "https://";
    if (value.size() < prefix.size()) {
        return false;
    }
    for (std::size_t index = 0; index < prefix.size(); ++index) {
        if (!ascii_equal_ci(value[index], prefix[index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool valid_port(const std::string_view value) noexcept {
    if (value.empty() || value.size() > 5U) {
        return false;
    }
    unsigned int port = 0;
    const auto result = std::from_chars(
        value.data(), value.data() + value.size(), port);
    return result.ec == std::errc{} &&
           result.ptr == value.data() + value.size() &&
           port <= 65535U;
}

void validate_https_source(const std::string_view value) {
    constexpr std::size_t prefix_size = 8U;
    if (!starts_with_https(value) || value.size() <= prefix_size ||
        value.find('#') != std::string_view::npos ||
        value.find('\\') != std::string_view::npos) {
        throw std::runtime_error("artifact HTTPS source is invalid");
    }
    if (std::ranges::any_of(value, [](const unsigned char byte) {
            return byte <= 0x20U || byte == 0x7fU;
        })) {
        throw std::runtime_error("artifact HTTPS source is invalid");
    }

    const auto authority_end = value.find_first_of("/?", prefix_size);
    const auto authority = value.substr(
        prefix_size,
        authority_end == std::string_view::npos
            ? std::string_view::npos
            : authority_end - prefix_size);
    if (authority.empty() || authority.find('@') != std::string_view::npos) {
        throw std::runtime_error("artifact HTTPS source authority is invalid");
    }

    if (authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string_view::npos || close == 1U) {
            throw std::runtime_error("artifact HTTPS source host is invalid");
        }
        const auto suffix = authority.substr(close + 1U);
        if (!suffix.empty() &&
            (suffix.front() != ':' || !valid_port(suffix.substr(1U)))) {
            throw std::runtime_error("artifact HTTPS source port is invalid");
        }
        return;
    }

    if (authority.find('[') != std::string_view::npos ||
        authority.find(']') != std::string_view::npos) {
        throw std::runtime_error("artifact HTTPS source host is invalid");
    }
    const auto colon = authority.rfind(':');
    const auto host = colon == std::string_view::npos
                          ? authority
                          : authority.substr(0, colon);
    if (host.empty() || host.find(':') != std::string_view::npos) {
        throw std::runtime_error("artifact HTTPS source host is invalid");
    }
    if (colon != std::string_view::npos &&
        !valid_port(authority.substr(colon + 1U))) {
        throw std::runtime_error("artifact HTTPS source port is invalid");
    }
}

[[nodiscard]] std::vector<std::string> parse_catalog_source(
    const std::string_view value) {
    if (value.empty() || value.front() == '/' || value.back() == '/' ||
        value.find('\\') != std::string_view::npos ||
        value.find(':') != std::string_view::npos ||
        value.find("//") != std::string_view::npos ||
        has_ascii_control(value)) {
        throw std::runtime_error("artifact catalog source path is invalid");
    }

    std::vector<std::string> components;
    std::size_t begin = 0;
    while (begin < value.size()) {
        const auto slash = value.find('/', begin);
        const auto end = slash == std::string_view::npos
                             ? value.size()
                             : slash;
        const auto component = value.substr(begin, end - begin);
        if (component.empty() || component == "." || component == ".." ||
            component.size() > kMaximumLeafUtf8Bytes) {
            throw std::runtime_error("artifact catalog source path is invalid");
        }
        components.emplace_back(component);
        if (slash == std::string_view::npos) {
            break;
        }
        begin = slash + 1U;
    }
    if (components.empty()) {
        throw std::runtime_error("artifact catalog source path is invalid");
    }
    return components;
}

[[nodiscard]] ParsedSource parse_source(const std::string& source) {
    if (source.empty() || source.size() > kMaximumSourceUtf8Bytes ||
        !valid_utf8(source)) {
        throw std::runtime_error("artifact source is invalid");
    }
    if (starts_with_https(source)) {
        validate_https_source(source);
        return ParsedSource{SourceKind::https, source, {}};
    }
    return ParsedSource{
        SourceKind::catalog_relative,
        source,
        parse_catalog_source(source)};
}

[[nodiscard]] std::vector<ParsedSource> validate_request(
    const ArtifactAcquisitionRequest& request) {
    if (request.sources.empty() || request.sources.size() > kMaximumSources) {
        throw std::runtime_error("artifact source count is invalid");
    }
    if (!is_lower_hex_digest(request.expected_sha256) ||
        request.expected_size_bytes == 0) {
        throw std::runtime_error("artifact expected identity is invalid");
    }
    validate_absolute_path(request.catalog_directory, true);
    validate_absolute_path(request.cache_root, false);

    if (request.policy.cache_lock_timeout < kMinimumCacheLockTimeout ||
        request.policy.cache_lock_timeout > kMaximumCacheLockTimeout) {
        throw std::runtime_error("artifact cache lock timeout is invalid");
    }
    if (request.policy.connect_timeout <= std::chrono::milliseconds::zero() ||
        request.policy.connect_timeout > kMaximumConnectTimeout) {
        throw std::runtime_error("artifact connect timeout is invalid");
    }
    if (request.policy.overall_timeout < kMinimumOverallTimeout ||
        request.policy.overall_timeout > kMaximumOverallTimeout) {
        throw std::runtime_error("artifact overall timeout is invalid");
    }

    std::vector<ParsedSource> parsed;
    parsed.reserve(request.sources.size());
    for (const auto& source : request.sources) {
        parsed.push_back(parse_source(source));
    }
    return parsed;
}

void check_terminal(
    const std::stop_token stop_token,
    const std::chrono::steady_clock::time_point deadline) {
    if (stop_token.stop_requested()) {
        throw CancellationFailure{};
    }
    if (std::chrono::steady_clock::now() >= deadline) {
        throw DeadlineFailure{};
    }
}

[[nodiscard]] std::chrono::milliseconds remaining_time(
    const std::stop_token stop_token,
    const std::chrono::steady_clock::time_point deadline) {
    check_terminal(stop_token, deadline);
    const auto remaining = deadline - std::chrono::steady_clock::now();
    auto milliseconds = std::chrono::ceil<std::chrono::milliseconds>(remaining);
    if (milliseconds <= std::chrono::milliseconds::zero()) {
        milliseconds = std::chrono::milliseconds{1};
    }
    return milliseconds;
}

void ensure_crypto_available() {
    if (sodium_init() < 0) {
        throw CacheFailure("artifact cryptography is unavailable");
    }
}

class Sha256Accumulator final {
  public:
    Sha256Accumulator() {
        if (crypto_hash_sha256_init(&state_) != 0) {
            throw CacheFailure("artifact hashing initialization failed");
        }
    }

    void update(const std::span<const std::byte> bytes) {
        if (!bytes.empty() &&
            crypto_hash_sha256_update(
                &state_,
                reinterpret_cast<const unsigned char*>(bytes.data()),
                static_cast<unsigned long long>(bytes.size())) != 0) {
            throw CacheFailure("artifact hashing failed");
        }
    }

    [[nodiscard]] std::string finish() {
        std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
        if (crypto_hash_sha256_final(&state_, digest.data()) != 0) {
            throw CacheFailure("artifact hashing failed");
        }
        constexpr std::string_view hex = "0123456789abcdef";
        std::string result(digest.size() * 2U, '0');
        for (std::size_t index = 0; index < digest.size(); ++index) {
            const auto value = digest[index];
            result[index * 2U] = hex[(value >> 4U) & 0x0fU];
            result[index * 2U + 1U] = hex[value & 0x0fU];
        }
        return result;
    }

  private:
    crypto_hash_sha256_state state_{};
};

[[nodiscard]] std::string random_id() {
    std::array<unsigned char, 16> bytes{};
    randombytes_buf(bytes.data(), bytes.size());
    constexpr std::string_view hex = "0123456789abcdef";
    std::string result(32U, '0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        result[index * 2U] = hex[(bytes[index] >> 4U) & 0x0fU];
        result[index * 2U + 1U] = hex[bytes[index] & 0x0fU];
    }
    return result;
}

[[nodiscard]] VerificationResult verify_admitted_file(
    ports::AdmittedRegularFile& file,
    const std::uint64_t expected_size,
    const std::string_view expected_sha256,
    const std::stop_token stop_token,
    const std::chrono::steady_clock::time_point deadline) {
    const auto initial = file.initial_observation();
    if (initial.kind != ports::FilesystemObjectKind::regular_file) {
        throw CacheFailure("artifact cache capability has the wrong kind");
    }

    bool content_mismatch = initial.size_bytes != expected_size;
    Sha256Accumulator hash;
    std::array<std::byte, kTransferChunkBytes> buffer{};
    std::uint64_t offset = 0;

    if (!content_mismatch) {
        while (offset < expected_size) {
            check_terminal(stop_token, deadline);
            const auto remaining = expected_size - offset;
            const auto request = static_cast<std::size_t>(
                std::min<std::uint64_t>(remaining, buffer.size()));
            const auto read = file.read_at(
                offset, std::span<std::byte>{buffer.data(), request});
            if (read > request) {
                throw CacheFailure("artifact cache read returned an invalid count");
            }
            if (read == 0) {
                throw CacheFailure("artifact cache read ended before stable size");
            }
            hash.update(std::span<const std::byte>{buffer.data(), read});
            offset += static_cast<std::uint64_t>(read);
            check_terminal(stop_token, deadline);
        }

        std::array<std::byte, 1> probe{};
        if (file.read_at(offset, probe) != 0) {
            content_mismatch = true;
        }
    }

    const auto final = file.observe_open_object();
    if (final != initial ||
        file.observe_path_binding() != ports::PathBindingState::same_object) {
        throw CacheFailure("artifact cache object changed during verification");
    }

    if (!content_mismatch) {
        content_mismatch = hash.finish() != expected_sha256;
    }
    return VerificationResult{!content_mismatch, initial};
}

void write_candidate(
    ports::ArtifactCacheCandidate& candidate,
    const std::span<const std::byte> bytes) {
    try {
        candidate.write(bytes);
    } catch (const std::bad_alloc&) {
        throw;
    } catch (...) {
        throw CacheFailure("artifact cache candidate write failed");
    }
}

void cleanup_candidate(ports::ArtifactCacheCandidate& candidate) {
    const auto diagnostic = candidate.cleanup();
    if (diagnostic.has_value()) {
        throw CacheFailure("artifact cache candidate cleanup failed");
    }
}

void cleanup_candidate_preserve_primary(
    ports::ArtifactCacheCandidate& candidate) noexcept {
    (void)candidate.cleanup();
}

void finish_candidate(ports::ArtifactCacheCandidate& candidate) {
    try {
        candidate.finish();
    } catch (const std::bad_alloc&) {
        throw;
    } catch (...) {
        throw CacheFailure("artifact cache candidate finish failed");
    }
}

void transfer_catalog_source(
    ports::FilesystemIdentity& filesystem_identity,
    const ArtifactAcquisitionRequest& request,
    const ParsedSource& source,
    ports::ArtifactCacheCandidate& candidate,
    const std::chrono::steady_clock::time_point deadline) {
    try {
        check_terminal(request.stop_token, deadline);
        auto directory =
            filesystem_identity.admit_directory(request.catalog_directory);
        for (std::size_t index = 0; index + 1U < source.components.size(); ++index) {
            directory = directory->open_directory(source.components[index]);
        }
        auto file = directory->open_regular_file(source.components.back());
        const auto initial = file->initial_observation();
        if (initial.size_bytes != request.expected_size_bytes) {
            throw SourceFailure("local_size");
        }

        Sha256Accumulator hash;
        std::array<std::byte, kTransferChunkBytes> buffer{};
        std::uint64_t offset = 0;
        while (offset < request.expected_size_bytes) {
            check_terminal(request.stop_token, deadline);
            const auto remaining = request.expected_size_bytes - offset;
            const auto request_size = static_cast<std::size_t>(
                std::min<std::uint64_t>(remaining, buffer.size()));
            const auto read = file->read_at(
                offset,
                std::span<std::byte>{buffer.data(), request_size});
            if (read > request_size) {
                throw SourceFailure("local_read");
            }
            if (read == 0) {
                throw SourceFailure("local_size");
            }
            const auto bytes =
                std::span<const std::byte>{buffer.data(), read};
            write_candidate(candidate, bytes);
            hash.update(bytes);
            offset += static_cast<std::uint64_t>(read);
            check_terminal(request.stop_token, deadline);
        }

        std::array<std::byte, 1> probe{};
        if (file->read_at(offset, probe) != 0) {
            throw SourceFailure("local_size");
        }

        const auto final = file->observe_open_object();
        if (final != initial ||
            file->observe_path_binding() != ports::PathBindingState::same_object) {
            throw SourceFailure("local_changed");
        }
        if (hash.finish() != request.expected_sha256) {
            throw SourceFailure("local_digest");
        }
    } catch (const CancellationFailure&) {
        throw;
    } catch (const DeadlineFailure&) {
        throw;
    } catch (const CacheFailure&) {
        throw;
    } catch (const SourceFailure&) {
        throw;
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const ports::FilesystemIdentityException&) {
        throw SourceFailure("local_unavailable");
    } catch (...) {
        throw SourceFailure("local_unavailable");
    }
}

[[nodiscard]] bool hard_byte_cap_allows_value(
    const std::uint64_t total,
    const std::size_t incoming,
    const std::uint64_t expected) noexcept {
    return total <= expected && incoming <= expected - total;
}

struct CurlTransferContext final {
    ports::ArtifactCacheCandidate* candidate{};
    Sha256Accumulator* hash{};
    std::stop_token stop_token;
    std::chrono::steady_clock::time_point deadline{};
    std::uint64_t expected_size{};
    std::uint64_t total{};
    bool cancelled{};
    bool deadline_expired{};
    bool too_large{};
    std::exception_ptr callback_failure{};
};

[[nodiscard]] bool callback_should_abort(CurlTransferContext& context) noexcept {
    if (context.stop_token.stop_requested()) {
        context.cancelled = true;
        return true;
    }
    if (std::chrono::steady_clock::now() >= context.deadline) {
        context.deadline_expired = true;
        return true;
    }
    return false;
}

std::size_t curl_write_callback(
    char* data,
    const std::size_t size,
    const std::size_t count,
    void* userdata) noexcept {
    auto& context = *static_cast<CurlTransferContext*>(userdata);
    if (callback_should_abort(context)) {
        return 0;
    }
    if (size != 0U && count > std::numeric_limits<std::size_t>::max() / size) {
        context.too_large = true;
        return 0;
    }
    const auto bytes = size * count;
    if (!hard_byte_cap_allows_value(
            context.total, bytes, context.expected_size)) {
        context.too_large = true;
        return 0;
    }

    try {
        const auto span = std::span<const std::byte>{
            reinterpret_cast<const std::byte*>(data), bytes};
        write_candidate(*context.candidate, span);
        context.hash->update(span);
        context.total += static_cast<std::uint64_t>(bytes);
    } catch (...) {
        context.callback_failure = std::current_exception();
        return 0;
    }

    if (callback_should_abort(context)) {
        return 0;
    }
    return bytes;
}

int curl_progress_callback(
    void* userdata,
    curl_off_t,
    curl_off_t,
    curl_off_t,
    curl_off_t) noexcept {
    auto& context = *static_cast<CurlTransferContext*>(userdata);
    return callback_should_abort(context) ? 1 : 0;
}

class CurlEasy final {
  public:
    CurlEasy() : handle_(curl_easy_init()) {
        if (handle_ == nullptr) {
            throw CacheFailure("HTTPS transport initialization failed");
        }
    }

    ~CurlEasy() {
        curl_easy_cleanup(handle_);
    }

    CurlEasy(const CurlEasy&) = delete;
    CurlEasy& operator=(const CurlEasy&) = delete;

    [[nodiscard]] CURL* get() const noexcept {
        return handle_;
    }

  private:
    CURL* handle_{};
};

void ensure_curl_global() {
    static std::once_flag flag;
    static CURLcode result = CURLE_OK;
    std::call_once(flag, [] {
        result = curl_global_init(CURL_GLOBAL_DEFAULT);
    });
    if (result != CURLE_OK) {
        throw CacheFailure("HTTPS transport global initialization failed");
    }
}

void require_setopt(const CURLcode result) {
    if (result != CURLE_OK) {
        throw CacheFailure("HTTPS transport configuration failed");
    }
}

[[nodiscard]] std::chrono::milliseconds clipped_connect_timeout(
    const std::chrono::milliseconds requested,
    const std::chrono::milliseconds remaining) noexcept {
    return (std::min)(requested, remaining);
}

void transfer_https_source(
    const ArtifactAcquisitionRequest& request,
    const ParsedSource& source,
    ports::ArtifactCacheCandidate& candidate,
    const std::chrono::steady_clock::time_point deadline) {
    check_terminal(request.stop_token, deadline);
    ensure_curl_global();
    CurlEasy easy;
    Sha256Accumulator hash;
    CurlTransferContext context{
        &candidate,
        &hash,
        request.stop_token,
        deadline,
        request.expected_size_bytes,
    };

    const auto remaining = remaining_time(request.stop_token, deadline);
    const auto connect_timeout = clipped_connect_timeout(
        request.policy.connect_timeout, remaining);
    const auto timeout_count = remaining.count();
    const auto connect_count = connect_timeout.count();
    if (timeout_count <= 0 || connect_count <= 0 ||
        timeout_count > std::numeric_limits<long>::max() ||
        connect_count > std::numeric_limits<long>::max()) {
        throw DeadlineFailure{};
    }

    auto* curl = easy.get();
    require_setopt(curl_easy_setopt(curl, CURLOPT_URL, source.value.c_str()));
    require_setopt(curl_easy_setopt(
        curl, CURLOPT_PROTOCOLS_STR, kHttpsProtocols.data()));
    require_setopt(curl_easy_setopt(
        curl, CURLOPT_REDIR_PROTOCOLS_STR, kHttpsProtocols.data()));
    require_setopt(curl_easy_setopt(
        curl, CURLOPT_FOLLOWLOCATION, kHttpsFollowLocation));
    require_setopt(curl_easy_setopt(curl, CURLOPT_MAXREDIRS, kHttpsRedirectLimit));
    require_setopt(curl_easy_setopt(curl, CURLOPT_FAILONERROR, kHttpsFailOnError));
    require_setopt(curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, kHttpsVerifyPeer));
    require_setopt(curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, kHttpsVerifyHost));
    require_setopt(curl_easy_setopt(
        curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(connect_count)));
    require_setopt(curl_easy_setopt(
        curl, CURLOPT_LOW_SPEED_LIMIT, kLowSpeedBytesPerSecond));
    require_setopt(curl_easy_setopt(
        curl, CURLOPT_LOW_SPEED_TIME, kLowSpeedWindowSeconds));
    require_setopt(curl_easy_setopt(
        curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_count)));
    if (request.expected_size_bytes <=
        static_cast<std::uint64_t>(std::numeric_limits<curl_off_t>::max())) {
        require_setopt(curl_easy_setopt(
            curl,
            CURLOPT_MAXFILESIZE_LARGE,
            static_cast<curl_off_t>(request.expected_size_bytes)));
    }
    require_setopt(curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L));
    require_setopt(curl_easy_setopt(
        curl, CURLOPT_XFERINFOFUNCTION, curl_progress_callback));
    require_setopt(curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &context));
    require_setopt(curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_callback));
    require_setopt(curl_easy_setopt(curl, CURLOPT_WRITEDATA, &context));

    const auto result = curl_easy_perform(curl);
    if (context.callback_failure) {
        std::rethrow_exception(context.callback_failure);
    }
    if (context.cancelled || request.stop_token.stop_requested()) {
        throw CancellationFailure{};
    }
    if (context.deadline_expired ||
        std::chrono::steady_clock::now() >= deadline) {
        throw DeadlineFailure{};
    }
    if (context.too_large) {
        throw SourceFailure("https_size");
    }
    if (result != CURLE_OK) {
        throw SourceFailure("https_transfer");
    }
    if (context.total != request.expected_size_bytes) {
        throw SourceFailure("https_size");
    }
    if (hash.finish() != request.expected_sha256) {
        throw SourceFailure("https_digest");
    }
    check_terminal(request.stop_token, deadline);
}

[[nodiscard]] std::unique_ptr<ports::AdmittedRegularFile> admit_target_if_present(
    ports::FilesystemIdentity& filesystem_identity,
    const std::filesystem::path& target_path) {
    try {
        return filesystem_identity.admit_regular_file(target_path);
    } catch (const ports::FilesystemIdentityException& error) {
        if (error.code() == ports::FilesystemIdentityErrorCode::missing) {
            return nullptr;
        }
        throw CacheFailure("artifact cache target could not be admitted safely");
    } catch (const std::bad_alloc&) {
        throw;
    } catch (...) {
        throw CacheFailure("artifact cache target admission failed");
    }
}

[[nodiscard]] std::unique_ptr<ports::AdmittedRegularFile> require_exact_target(
    ports::FilesystemIdentity& filesystem_identity,
    const std::filesystem::path& target_path,
    const ArtifactAcquisitionRequest& request,
    const std::chrono::steady_clock::time_point deadline) {
    auto file = admit_target_if_present(filesystem_identity, target_path);
    if (!file) {
        throw CacheFailure("artifact cache target is missing after activation");
    }
    const auto verification = verify_admitted_file(
        *file,
        request.expected_size_bytes,
        request.expected_sha256,
        request.stop_token,
        deadline);
    if (!verification.exact) {
        throw CacheFailure("artifact cache target does not match expected bytes");
    }
    return file;
}

[[nodiscard]] std::string all_sources_message(
    const std::vector<std::string>& categories,
    const bool offline,
    const bool any_attempted) {
    std::string message = !any_attempted && offline
                              ? "artifact acquisition offline_unavailable"
                              : "artifact acquisition all_sources_failed";
    for (std::size_t index = 0; index < categories.size(); ++index) {
        if (categories[index].empty()) {
            continue;
        }
        const auto entry = " source[" + std::to_string(index) + "]=" +
                           categories[index];
        if (message.size() + entry.size() > kMaximumDiagnosticBytes) {
            break;
        }
        message += entry;
    }
    return message;
}

} // namespace

ArtifactAcquirer::ArtifactAcquirer(
    arena::ports::FilesystemIdentity& filesystem_identity,
    arena::ports::ArtifactCachePlatform& cache_platform)
    : filesystem_identity_(filesystem_identity),
      cache_platform_(cache_platform) {}

AcquiredArtifact ArtifactAcquirer::acquire(
    const ArtifactAcquisitionRequest& request) const {
    const auto sources = validate_request(request);
    ensure_crypto_available();
    const auto deadline =
        std::chrono::steady_clock::now() + request.policy.overall_timeout;
    check_terminal(request.stop_token, deadline);

    const auto lease_timeout = (std::min)(
        request.policy.cache_lock_timeout,
        remaining_time(request.stop_token, deadline));
    auto lease = cache_platform_.acquire_digest_lease(
        request.cache_root, request.expected_sha256, lease_timeout);
    check_terminal(request.stop_token, deadline);

    const auto target_path = lease->target_path();
    if (auto target = admit_target_if_present(filesystem_identity_, target_path)) {
        const auto verification = verify_admitted_file(
            *target,
            request.expected_size_bytes,
            request.expected_sha256,
            request.stop_token,
            deadline);
        if (verification.exact) {
            return AcquiredArtifact{
                target_path,
                request.expected_size_bytes,
                request.expected_sha256,
                std::move(target)};
        }
        try {
            lease->discard_exact_target(
                verification.initial.object_identity, random_id());
        } catch (const std::bad_alloc&) {
            throw;
        } catch (...) {
            throw CacheFailure("artifact cache invalid-target disposal failed");
        }
    }

    std::vector<std::string> categories(sources.size());
    bool any_attempted = false;
    for (std::size_t index = 0; index < sources.size(); ++index) {
        const auto& source = sources[index];
        check_terminal(request.stop_token, deadline);
        if (request.offline && source.kind == SourceKind::https) {
            categories[index] = "offline_skipped";
            continue;
        }
        any_attempted = true;

        std::unique_ptr<ports::ArtifactCacheCandidate> candidate;
        try {
            candidate = lease->create_candidate(
                random_id(), request.expected_size_bytes);
        } catch (const std::bad_alloc&) {
            throw;
        } catch (...) {
            throw CacheFailure("artifact cache candidate creation failed");
        }
        if (!candidate) {
            throw CacheFailure("artifact cache platform returned no candidate");
        }

        try {
            if (source.kind == SourceKind::https) {
                transfer_https_source(request, source, *candidate, deadline);
            } else {
                transfer_catalog_source(
                    filesystem_identity_, request, source, *candidate, deadline);
            }
            finish_candidate(*candidate);

            auto candidate_file = filesystem_identity_.admit_regular_file(
                candidate->path());
            const auto candidate_verification = verify_admitted_file(
                *candidate_file,
                request.expected_size_bytes,
                request.expected_sha256,
                request.stop_token,
                deadline);
            if (!candidate_verification.exact) {
                throw CacheFailure(
                    "artifact cache candidate failed independent verification");
            }

            ports::ArtifactCacheActivationResult activation;
            try {
                activation = lease->activate_candidate(
                    *candidate,
                    candidate_verification.initial.object_identity);
            } catch (const std::bad_alloc&) {
                throw;
            } catch (...) {
                throw CacheFailure("artifact cache activation failed");
            }

            switch (activation.outcome) {
            case ports::ArtifactCacheActivationOutcome::activated: {
                auto target = require_exact_target(
                    filesystem_identity_, target_path, request, deadline);
                return AcquiredArtifact{
                    target_path,
                    request.expected_size_bytes,
                    request.expected_sha256,
                    std::move(target)};
            }
            case ports::ArtifactCacheActivationOutcome::target_occupied: {
                auto winner = admit_target_if_present(
                    filesystem_identity_, target_path);
                if (winner) {
                    const auto winner_verification = verify_admitted_file(
                        *winner,
                        request.expected_size_bytes,
                        request.expected_sha256,
                        request.stop_token,
                        deadline);
                    if (winner_verification.exact) {
                        cleanup_candidate(*candidate);
                        return AcquiredArtifact{
                            target_path,
                            request.expected_size_bytes,
                            request.expected_sha256,
                            std::move(winner)};
                    }
                }
                cleanup_candidate(*candidate);
                throw CacheFailure("artifact cache collision is invalid");
            }
            case ports::ArtifactCacheActivationOutcome::activated_durability_unproven:
                throw CacheFailure(
                    "artifact cache activation durability is unproven");
            case ports::ArtifactCacheActivationOutcome::indeterminate:
                throw CacheFailure("artifact cache activation is indeterminate");
            }
            throw CacheFailure("artifact cache activation outcome is invalid");
        } catch (const SourceFailure& failure) {
            cleanup_candidate(*candidate);
            categories[index] = failure.category();
            continue;
        } catch (const CancellationFailure&) {
            cleanup_candidate_preserve_primary(*candidate);
            throw;
        } catch (const DeadlineFailure&) {
            cleanup_candidate_preserve_primary(*candidate);
            throw;
        } catch (const std::bad_alloc&) {
            cleanup_candidate_preserve_primary(*candidate);
            throw;
        } catch (const CacheFailure&) {
            throw;
        } catch (const ports::FilesystemIdentityException&) {
            cleanup_candidate(*candidate);
            throw CacheFailure("artifact cache candidate identity verification failed");
        } catch (...) {
            cleanup_candidate(*candidate);
            throw CacheFailure("artifact cache candidate processing failed");
        }
    }

    throw std::runtime_error(
        all_sources_message(categories, request.offline, any_attempted));
}

namespace artifact_acquirer_test_seam {

std::string_view https_protocols() noexcept {
    return kHttpsProtocols;
}

long https_follow_location() noexcept {
    return kHttpsFollowLocation;
}

long https_fail_on_error() noexcept {
    return kHttpsFailOnError;
}

long https_verify_peer() noexcept {
    return kHttpsVerifyPeer;
}

long https_verify_host() noexcept {
    return kHttpsVerifyHost;
}

long https_redirect_limit() noexcept {
    return kHttpsRedirectLimit;
}

long https_low_speed_limit() noexcept {
    return kLowSpeedBytesPerSecond;
}

long https_low_speed_window() noexcept {
    return kLowSpeedWindowSeconds;
}

std::chrono::milliseconds https_connect_timeout(
    const std::chrono::milliseconds requested,
    const std::chrono::milliseconds remaining) noexcept {
    return clipped_connect_timeout(requested, remaining);
}

bool hard_byte_cap_allows(
    const std::uint64_t total,
    const std::size_t incoming,
    const std::uint64_t expected) noexcept {
    return hard_byte_cap_allows_value(total, incoming, expected);
}

} // namespace artifact_acquirer_test_seam

} // namespace arena::application::installer
