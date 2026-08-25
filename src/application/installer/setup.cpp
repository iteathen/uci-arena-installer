#include "arena/application/installer/setup.hpp"

#include <archive.h>
#include <archive_entry.h>
#include <curl/curl.h>
#include <sodium.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <vector>

#include "arena/adapters/provider_lifecycle.hpp"
#include "arena/adapters/uci_process.hpp"
#include "arena/application/installer/provider_lifecycle_contract.hpp"
#include "arena/application/installer/removal_lifecycle.hpp"
#include "arena/application/process_supervisor.hpp"
#include "arena/ports/asset_search_index.hpp"
#include "arena/ports/provider_lifecycle.hpp"
#include "arena/platform/filesystem_path.hpp"
#include "arena/platform/service_control.hpp"
#include "arena/platform/workspace_permissions.hpp"

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace arena::application::installer {
namespace {

using nlohmann::json;
namespace fs = std::filesystem;
using arena::ports::ProviderLifecycleDisposition;
using arena::ports::ProviderLifecycleOperation;
using arena::ports::ProviderLifecycleResult;
using arena::ports::provider_lifecycle_result_to_json;

constexpr std::uint64_t gib = 1024ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t managed_book_max_bytes = 256ULL * 1024ULL * 1024ULL;
constexpr std::size_t managed_book_max_files = 64;
constexpr std::uint64_t managed_profile_max_bytes = 64ULL * 1024ULL * 1024ULL;
constexpr std::size_t managed_profile_max_files = 16;
constexpr const char* compiled_tuf_root_sha256 = ARENA_TUF_ROOT_SHA256;

json read_json(const fs::path& path) {
    std::ifstream input(platform::filesystem_api_path(path), std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot read JSON file: " + path.string());
    }
    try {
        return json::parse(input);
    } catch (const json::exception& error) {
        throw std::runtime_error("invalid JSON in " + path.string() + ": " + error.what());
    }
}

void write_json_atomic(const fs::path& path, const json& value) {
    fs::create_directories(platform::filesystem_api_path(path.parent_path()));
    const fs::path temporary = path.string() + ".tmp";
    {
        std::ofstream output(platform::filesystem_api_path(temporary),
                             std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("cannot write " + temporary.string());
        }
        output << value.dump(2) << '\n';
        output.flush();
        if (!output) {
            throw std::runtime_error("cannot flush " + temporary.string());
        }
    }
    std::error_code error;
    fs::rename(platform::filesystem_api_path(temporary),
               platform::filesystem_api_path(path), error);
    if (error) {
        fs::remove(platform::filesystem_api_path(path), error);
        error.clear();
        fs::rename(platform::filesystem_api_path(temporary),
                   platform::filesystem_api_path(path), error);
    }
    if (error) {
        throw std::runtime_error("cannot atomically activate " + path.string());
    }
}

std::string sha256_file(const fs::path& path) {
    if (sodium_init() < 0) {
        throw std::runtime_error("libsodium initialization failed");
    }
    std::ifstream input(platform::filesystem_api_path(path), std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot hash " + path.string());
    }
    crypto_hash_sha256_state state{};
    crypto_hash_sha256_init(&state);
    std::array<unsigned char, 64 * 1024> buffer{};
    while (input) {
        input.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
        const auto count = input.gcount();
        if (count > 0) {
            crypto_hash_sha256_update(&state, buffer.data(),
                                      static_cast<unsigned long long>(count));
        }
    }
    std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
    crypto_hash_sha256_final(&state, digest.data());
    std::array<char, crypto_hash_sha256_BYTES * 2 + 1> encoded{};
    sodium_bin2hex(encoded.data(), encoded.size(), digest.data(), digest.size());
    return encoded.data();
}

std::string sha256_text(const std::string& value) {
    if (sodium_init() < 0) {
        throw std::runtime_error("libsodium initialization failed");
    }
    std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
    crypto_hash_sha256(digest.data(), reinterpret_cast<const unsigned char*>(value.data()),
                       static_cast<unsigned long long>(value.size()));
    std::array<char, crypto_hash_sha256_BYTES * 2 + 1> encoded{};
    sodium_bin2hex(encoded.data(), encoded.size(), digest.data(), digest.size());
    return encoded.data();
}

bool valid_identifier(const std::string& value) {
    return !value.empty() &&
           std::all_of(value.begin(), value.end(), [](const unsigned char character) {
               return std::isalnum(character) != 0 || character == '.' || character == '_' ||
                      character == '-';
           });
}

bool valid_bundle_owner(const std::string& value) {
    return !value.empty() && value.size() <= 128 &&
           std::all_of(value.begin(), value.end(), [](const unsigned char character) {
               return std::isalnum(character) != 0 || character == '.' || character == '_' ||
                      character == '-' || character == '{' || character == '}';
           });
}

fs::path normalized_absolute(const fs::path& value) {
    return fs::absolute(value).lexically_normal();
}

bool contains_symbolic_link(const fs::path& value) {
    const fs::path absolute = normalized_absolute(value);
    fs::path current = absolute.root_path();
    std::error_code error;
    for (const auto& part : absolute.relative_path()) {
        current /= part;
        const fs::file_status status =
            fs::symlink_status(platform::filesystem_api_path(current), error);
        if (error) {
            error.clear();
            continue;
        }
        if (fs::is_symlink(status)) {
            return true;
        }
    }
    return false;
}

void require_no_symbolic_link(const fs::path& value) {
    if (contains_symbolic_link(value)) {
        throw std::runtime_error("symbolic-link traversal is forbidden: " + value.string());
    }
}

fs::path nearest_existing_ancestor(fs::path path) {
    path = normalized_absolute(path);
    while (!path.empty() && !fs::exists(path)) {
        const fs::path parent = path.parent_path();
        if (parent == path) {
            break;
        }
        path = parent;
    }
    if (path.empty() || !fs::exists(path)) {
        throw std::runtime_error("cannot resolve a filesystem volume");
    }
    return path;
}

std::string volume_key(const fs::path& path) {
    const fs::path existing = nearest_existing_ancestor(path);
#ifdef _WIN32
    std::string key = (existing.root_name() / existing.root_directory()).string();
    std::ranges::transform(key, key.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return key;
#else
    struct stat status{};
    if (::stat(existing.c_str(), &status) != 0) {
        throw std::runtime_error("cannot identify filesystem volume for " + existing.string());
    }
    return std::to_string(static_cast<std::uintmax_t>(status.st_dev));
#endif
}

bool within(const fs::path& root, const fs::path& candidate) {
    const fs::path normalized_root = normalized_absolute(root);
    const fs::path normalized_candidate = normalized_absolute(candidate);
    auto root_it = normalized_root.begin();
    auto candidate_it = normalized_candidate.begin();
    for (; root_it != normalized_root.end(); ++root_it, ++candidate_it) {
        if (candidate_it == normalized_candidate.end() || *root_it != *candidate_it) {
            return false;
        }
    }
    return normalized_candidate != normalized_root;
}

bool at_or_within(const fs::path& root, const fs::path& candidate) {
    return normalized_absolute(root) == normalized_absolute(candidate) ||
           within(root, candidate);
}

void require_external_token_path(
    const fs::path& manager_data_root,
    const fs::path& token_path) {
    const fs::path normalized_data_root = normalized_absolute(manager_data_root);
    const fs::path normalized_token_path = normalized_absolute(token_path);
    if (normalized_token_path == normalized_data_root ||
        within(normalized_data_root, normalized_token_path)) {
        throw std::runtime_error(
            "Lichess token file must be operator-owned and outside Manager data");
    }
}

bool safe_relative_path(const fs::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_name()) {
        return false;
    }
    for (const auto& part : path) {
        if (part == ".." || part == ".") {
            return false;
        }
    }
    return true;
}

std::uint64_t integer_field(const json& value, const char* key) {
    if (!value.contains(key)) {
        return 0;
    }
    if (!value.at(key).is_number_unsigned() && !value.at(key).is_number_integer()) {
        throw std::runtime_error(std::string("catalog field must be an integer: ") + key);
    }
    const auto parsed = value.at(key).get<std::int64_t>();
    if (parsed < 0) {
        throw std::runtime_error(std::string("catalog field must not be negative: ") + key);
    }
    return static_cast<std::uint64_t>(parsed);
}

std::string current_utc_iso() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t timestamp = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &timestamp);
#else
    gmtime_r(&timestamp, &utc);
#endif
    std::array<char, 32> buffer{};
    std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buffer.data();
}

std::vector<unsigned char> decode_hex(const std::string& encoded, const std::size_t expected_size,
                                      const std::string& label) {
    std::vector<unsigned char> decoded(expected_size);
    std::size_t actual = 0;
    if (sodium_hex2bin(decoded.data(), decoded.size(), encoded.c_str(), encoded.size(), nullptr,
                       &actual, nullptr) != 0 ||
        actual != expected_size) {
        throw std::runtime_error("invalid hexadecimal " + label);
    }
    return decoded;
}

void verify_role(const json& metadata, const json& root_signed, const std::string& role_name) {
    if (!metadata.contains("signed") || !metadata.contains("signatures") ||
        !metadata.at("signatures").is_array()) {
        throw std::runtime_error("TUF " + role_name + " metadata is incomplete");
    }
    const auto& role = root_signed.at("roles").at(role_name);
    const std::set<std::string> allowed(role.at("keyids").begin(), role.at("keyids").end());
    const std::size_t threshold = role.at("threshold").get<std::size_t>();
    const std::string canonical = metadata.at("signed").dump();
    std::set<std::string> accepted;
    for (const auto& signature : metadata.at("signatures")) {
        const std::string key_id = signature.value("keyid", "");
        if (!allowed.contains(key_id) || accepted.contains(key_id)) {
            continue;
        }
        const auto& key = root_signed.at("keys").at(key_id);
        if (key.value("keytype", "") != "ed25519" || key.value("scheme", "") != "ed25519") {
            continue;
        }
        const auto public_key = decode_hex(key.at("keyval").at("public").get<std::string>(),
                                           crypto_sign_PUBLICKEYBYTES, "TUF public key");
        const auto signature_bytes =
            decode_hex(signature.at("sig").get<std::string>(), crypto_sign_BYTES, "TUF signature");
        if (crypto_sign_verify_detached(signature_bytes.data(),
                                        reinterpret_cast<const unsigned char*>(canonical.data()),
                                        canonical.size(), public_key.data()) == 0) {
            accepted.insert(key_id);
        }
    }
    if (accepted.size() < threshold) {
        throw std::runtime_error("TUF " + role_name + " signature threshold was not met");
    }
}

void verify_not_expired(const json& metadata, const std::string& role) {
    const std::string expires = metadata.at("signed").value("expires", "");
    if (expires.size() != 20 || expires <= current_utc_iso()) {
        throw std::runtime_error("TUF " + role + " metadata is expired or malformed");
    }
}

void verify_described_file(const fs::path& path, const json& descriptor, const std::string& label) {
    if (!fs::is_regular_file(path)) {
        throw std::runtime_error(label + " is missing: " + path.string());
    }
    const auto expected_size = integer_field(descriptor, "length");
    if (expected_size != 0 && fs::file_size(path) != expected_size) {
        throw std::runtime_error(label + " size does not match trusted metadata");
    }
    const std::string expected_hash =
        descriptor.value("hashes", json::object()).value("sha256", "");
    if (expected_hash.size() != 64 || sha256_file(path) != expected_hash) {
        throw std::runtime_error(label + " SHA-256 does not match trusted metadata");
    }
}

std::uint64_t metadata_version(const json& metadata, const std::string& role) {
    if (!metadata.at("signed").contains("version") ||
        (!metadata.at("signed").at("version").is_number_unsigned() &&
         !metadata.at("signed").at("version").is_number_integer())) {
        throw std::runtime_error("TUF " + role + " version is missing");
    }
    const auto version = metadata.at("signed").at("version").get<std::int64_t>();
    if (version < 1) {
        throw std::runtime_error("TUF " + role + " version is invalid");
    }
    return static_cast<std::uint64_t>(version);
}

void verify_tuf_chain(const fs::path& catalog_path, const fs::path& metadata_directory,
                      const fs::path& rollback_state_path) {
    if (sodium_init() < 0) {
        throw std::runtime_error("libsodium initialization failed");
    }
    const fs::path root_path = metadata_directory / "root.json";
    const std::string pinned_root = compiled_tuf_root_sha256;
    if (pinned_root.size() != 64 || sha256_file(root_path) != pinned_root) {
        throw std::runtime_error(
            "TUF root metadata does not match the root compiled into arena-setup");
    }
    const json root = read_json(root_path);
    if (root.at("signed").value("_type", "") != "root") {
        throw std::runtime_error("trusted root metadata has the wrong type");
    }
    verify_role(root, root.at("signed"), "root");
    verify_not_expired(root, "root");

    const json timestamp = read_json(metadata_directory / "timestamp.json");
    verify_role(timestamp, root.at("signed"), "timestamp");
    verify_not_expired(timestamp, "timestamp");
    verify_described_file(metadata_directory / "snapshot.json",
                          timestamp.at("signed").at("meta").at("snapshot.json"), "TUF snapshot");

    const json snapshot = read_json(metadata_directory / "snapshot.json");
    verify_role(snapshot, root.at("signed"), "snapshot");
    verify_not_expired(snapshot, "snapshot");
    verify_described_file(metadata_directory / "targets.json",
                          snapshot.at("signed").at("meta").at("targets.json"), "TUF targets");

    const json targets = read_json(metadata_directory / "targets.json");
    verify_role(targets, root.at("signed"), "targets");
    verify_not_expired(targets, "targets");
    const std::string target_name = catalog_path.filename().generic_string();
    verify_described_file(catalog_path, targets.at("signed").at("targets").at(target_name),
                          "installation catalog");

    const json versions{
        {"root", metadata_version(root, "root")},
        {"timestamp", metadata_version(timestamp, "timestamp")},
        {"snapshot", metadata_version(snapshot, "snapshot")},
        {"targets", metadata_version(targets, "targets")},
    };
    if (timestamp.at("signed").at("meta").at("snapshot.json").value("version", 0ULL) !=
            versions.at("snapshot").get<std::uint64_t>() ||
        snapshot.at("signed").at("meta").at("targets.json").value("version", 0ULL) !=
            versions.at("targets").get<std::uint64_t>()) {
        throw std::runtime_error("TUF delegated metadata versions disagree");
    }
    if (!rollback_state_path.empty() && fs::is_regular_file(rollback_state_path)) {
        const json previous = read_json(rollback_state_path);
        if (previous.value("schema", "") != "arena_tuf_rollback_state_v1") {
            throw std::runtime_error("TUF rollback state is malformed");
        }
        for (const auto& [role, version] : versions.items()) {
            if (version.get<std::uint64_t>() < previous.value(role, 0ULL)) {
                throw std::runtime_error("TUF rollback detected for " + role);
            }
        }
    }
    if (!rollback_state_path.empty()) {
        write_json_atomic(rollback_state_path, {
                                                   {"schema", "arena_tuf_rollback_state_v1"},
                                                   {"root_sha256", pinned_root},
                                                   {"root", versions.at("root")},
                                                   {"timestamp", versions.at("timestamp")},
                                                   {"snapshot", versions.at("snapshot")},
                                                   {"targets", versions.at("targets")},
                                                   {"verified_at", current_utc_iso()},
                                               });
    }
}

const json& component_by_id(const json& catalog, const std::string& id) {
    for (const auto& component : catalog.at("components")) {
        if (component.value("id", "") == id) {
            return component;
        }
    }
    throw std::runtime_error("catalog does not define component " + id);
}

const json& platform_artifact(const json& component) {
    const auto& artifacts = component.at("artifacts");
    const std::string key = SetupEngine::platform_key();
    if (artifacts.contains(key)) {
        return artifacts.at(key);
    }
    if (artifacts.contains("any")) {
        return artifacts.at("any");
    }
    throw std::runtime_error("component " + component.at("id").get<std::string>() +
                             " has no artifact for " + key);
}

std::size_t curl_write(char* data, std::size_t size, std::size_t count, void* output) {
    static_cast<std::ofstream*>(output)->write(data, static_cast<std::streamsize>(size * count));
    return size * count;
}

fs::path acquire_artifact(const json& artifact, const fs::path& catalog_directory,
                          const SetupPaths& paths, const bool offline) {
    fs::create_directories(paths.cache_root);
    const std::string expected_hash = artifact.at("sha256").get<std::string>();
    if (expected_hash.size() != 64) {
        throw std::runtime_error("artifact SHA-256 is malformed");
    }
    const fs::path cached = paths.cache_root / expected_hash;
    if (fs::is_regular_file(cached)) {
        if (sha256_file(cached) == expected_hash) {
            return cached;
        }
        std::error_code remove_error;
        fs::remove(cached, remove_error);
        if (remove_error) {
            throw std::runtime_error("cannot replace corrupt cached artifact");
        }
    }

    const std::string source = artifact.at("source").get<std::string>();
    const fs::path temporary = cached.string() + ".partial";
    std::error_code ignored;
    if (source.starts_with("https://")) {
        if (offline) {
            throw std::runtime_error("offline mode cannot download " + source);
        }
        const std::uintmax_t partial_size =
            fs::is_regular_file(temporary) ? fs::file_size(temporary) : 0;
        const auto expected_size = integer_field(artifact, "size_bytes");
        if (expected_size != 0 && partial_size > expected_size) {
            fs::remove(temporary, ignored);
        }
        const std::uintmax_t resume_from =
            fs::is_regular_file(temporary) ? fs::file_size(temporary) : 0;
        if (expected_size != 0 && resume_from == expected_size &&
            sha256_file(temporary) == expected_hash) {
            fs::rename(temporary, cached);
            return cached;
        }
        std::ofstream output(temporary, std::ios::binary |
                                            (resume_from > 0 ? std::ios::app : std::ios::trunc));
        CURL* handle = curl_easy_init();
        if (handle == nullptr) {
            throw std::runtime_error("cannot initialize artifact download");
        }
        curl_easy_setopt(handle, CURLOPT_URL, source.c_str());
        curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(handle, CURLOPT_PROTOCOLS_STR, "https");
        curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
        curl_easy_setopt(handle, CURLOPT_TIMEOUT, 0L);
        curl_easy_setopt(handle, CURLOPT_FAILONERROR, 1L);
        if (resume_from > 0) {
            curl_easy_setopt(handle, CURLOPT_RESUME_FROM_LARGE,
                             static_cast<curl_off_t>(resume_from));
        }
        curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, curl_write);
        curl_easy_setopt(handle, CURLOPT_WRITEDATA, &output);
        const CURLcode result = curl_easy_perform(handle);
        curl_easy_cleanup(handle);
        output.close();
        if (result != CURLE_OK) {
            throw std::runtime_error("artifact download failed: " +
                                     std::string(curl_easy_strerror(result)));
        }
    } else {
        fs::remove(temporary, ignored);
        fs::path source_path = fs::path(source);
        if (source_path.is_relative()) {
            source_path = catalog_directory / source_path;
        }
        if (!fs::is_regular_file(source_path)) {
            throw std::runtime_error("artifact source is unavailable: " + source_path.string());
        }
        fs::copy_file(source_path, temporary, fs::copy_options::overwrite_existing);
    }
    if (sha256_file(temporary) != expected_hash) {
        fs::remove(temporary, ignored);
        throw std::runtime_error("downloaded artifact SHA-256 mismatch");
    }
    const auto expected_size = integer_field(artifact, "size_bytes");
    if (expected_size != 0 && fs::file_size(temporary) != expected_size) {
        fs::remove(temporary, ignored);
        throw std::runtime_error("downloaded artifact size mismatch");
    }
    fs::rename(temporary, cached);
    return cached;
}

json stage_file_set(const fs::path& manifest_path, const json& catalog_artifact,
                    const fs::path& catalog_directory, const SetupPaths& paths, const bool offline,
                    const fs::path& stage, const std::string& component_id,
                    const std::string& component_version) {
    const json file_set = read_json(manifest_path);
    const bool provider_syzygy =
        file_set.value("schema", "") == "arena_syzygy_file_set_manifest_v1";
    const bool generic_file_set = file_set.value("schema", "") == "arena_install_file_set_v1";
    if ((!provider_syzygy && !generic_file_set) || file_set.value("schema_version", 0) != 1 ||
        file_set.value("component_id", "") != component_id ||
        file_set.value("component_version", "") != component_version ||
        !file_set.contains("files") || !file_set.at("files").is_array()) {
        throw std::runtime_error("file-set manifest identity is invalid");
    }
    const auto expected_count = integer_field(catalog_artifact, "file_count");
    const auto expected_total = integer_field(catalog_artifact, "total_size_bytes");
    if (expected_count == 0 || file_set.at("files").size() != expected_count) {
        throw std::runtime_error("file-set manifest count differs from signed catalog");
    }

    std::uint64_t total = 0;
    std::set<fs::path> paths_seen;
    json installed_files = json::array();
    fs::create_directories(stage);
    for (const auto& file : file_set.at("files")) {
        const fs::path declared = fs::path(file.at("path").get<std::string>()).lexically_normal();
        const fs::path relative = provider_syzygy ? fs::path("tablebases") / declared : declared;
        if (!safe_relative_path(relative) || !paths_seen.insert(relative).second ||
            !within(stage, stage / relative)) {
            throw std::runtime_error("file-set manifest contains an unsafe or duplicate path");
        }
        if (provider_syzygy) {
            const auto generic = relative.generic_string();
            const auto extension = relative.extension().string();
            if (!generic.starts_with("tablebases/") ||
                (extension != ".rtbw" && extension != ".rtbz")) {
                throw std::runtime_error(
                    "Syzygy file-set entries must be tablebases/*.rtbw or *.rtbz");
            }
        }
        const auto size = integer_field(file, "size_bytes");
        if (size == 0 || total > (std::numeric_limits<std::uint64_t>::max)() - size) {
            throw std::runtime_error("file-set manifest has invalid total size");
        }
        total += size;
        fs::path cached;
        if (provider_syzygy) {
            if (!file.contains("sources") || !file.at("sources").is_array() ||
                file.at("sources").empty()) {
                throw std::runtime_error("Syzygy file-set entry has no acquisition source");
            }
            std::string last_error;
            for (const auto& source : file.at("sources")) {
                try {
                    cached = acquire_artifact(
                        {{"source", source}, {"sha256", file.at("sha256")}, {"size_bytes", size}},
                        catalog_directory, paths, offline);
                    break;
                } catch (const std::exception& error) {
                    last_error = error.what();
                }
            }
            if (cached.empty()) {
                throw std::runtime_error("all Syzygy acquisition sources failed for " +
                                         declared.generic_string() + ": " + last_error);
            }
        } else {
            cached = acquire_artifact(file, catalog_directory, paths, offline);
        }
        fs::create_directories((stage / relative).parent_path());
        fs::copy_file(cached, stage / relative, fs::copy_options::overwrite_existing);
        installed_files.push_back({{"path", relative.generic_string()},
                                   {"sha256", file.at("sha256")},
                                   {"size_bytes", size}});
    }
    if (total != expected_total ||
        file_set.value("total_size_bytes", std::uint64_t{0}) != expected_total) {
        throw std::runtime_error("file-set manifest bytes differ from signed catalog");
    }

    json component_manifest{
        {"schema", "arena_install_component_v1"},
        {"schema_version", 1},
        {"component_id", component_id},
        {"component_version", component_version},
        {"target_triple", "any"},
        {"entrypoints", json::object()},
        {"capabilities",
         provider_syzygy ? json::array({"syzygy"})
                          : file_set.value("capabilities", json::array())},
        {"startup", json::object()},
        {"data_paths", json::array()},
        {"files", installed_files},
    };
    if (provider_syzygy) {
        component_manifest["discovery"] = json::array(
            {{{"kind", "syzygy"},
              {"locator", {{"source", "component_path"}, {"path", "tablebases"}}}}});
        component_manifest["activation"] = json::array(
            {{{"kind", "syzygy"},
              {"source_path", "tablebases"},
              {"destination_root", "knowledge_root"},
              {"destination_path", "tablebases"}}});
    } else if (file_set.contains("discovery")) {
        component_manifest["discovery"] = file_set.at("discovery");
    }
    if (!provider_syzygy && file_set.contains("activation")) {
        component_manifest["activation"] = file_set.at("activation");
    }
    write_json_atomic(stage / "arena-component.json", component_manifest);
    return component_manifest;
}

void extract_archive(const fs::path& archive_path, const fs::path& destination) {
    fs::create_directories(destination);
    archive* reader = archive_read_new();
    archive_read_support_filter_all(reader);
    archive_read_support_format_all(reader);
    if (archive_read_open_filename(reader, archive_path.string().c_str(), 10240) != ARCHIVE_OK) {
        const std::string error = archive_error_string(reader);
        archive_read_free(reader);
        throw std::runtime_error("cannot open component archive: " + error);
    }
    archive_entry* entry = nullptr;
    try {
        while (archive_read_next_header(reader, &entry) == ARCHIVE_OK) {
            const char* raw_path = archive_entry_pathname(entry);
            const fs::path relative =
                raw_path == nullptr ? fs::path{} : fs::path(raw_path).lexically_normal();
            if (relative == fs::path(".") && archive_entry_filetype(entry) == AE_IFDIR) {
                archive_read_data_skip(reader);
                continue;
            }
            if (!safe_relative_path(relative)) {
                throw std::runtime_error("component archive contains an unsafe path");
            }
            const fs::path output_path = destination / relative;
            if (!within(destination, output_path)) {
                throw std::runtime_error("component archive escapes its staging root");
            }
            archive_entry_set_pathname(entry, output_path.string().c_str());
            const int result = archive_read_extract(reader, entry,
                                                    ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM |
                                                        ARCHIVE_EXTRACT_SECURE_NODOTDOT |
                                                        ARCHIVE_EXTRACT_SECURE_SYMLINKS);
            if (result != ARCHIVE_OK) {
                throw std::runtime_error("component extraction failed: " +
                                         std::string(archive_error_string(reader)));
            }
        }
        archive_read_close(reader);
        archive_read_free(reader);
    } catch (...) {
        archive_read_close(reader);
        archive_read_free(reader);
        throw;
    }
}

std::set<std::string> identifier_set_field(const json& document, const char* field) {
    if (!document.contains(field) || !document.at(field).is_array()) {
        throw std::runtime_error(std::string("standalone service contract field is not an array: ") +
                                 field);
    }
    std::set<std::string> values;
    for (const auto& value : document.at(field)) {
        if (!value.is_string() || !valid_identifier(value.get<std::string>()) ||
            !values.insert(value.get<std::string>()).second) {
            throw std::runtime_error(
                std::string("standalone service contract has an invalid or duplicate ") + field +
                " value");
        }
    }
    if (values.empty()) {
        throw std::runtime_error(std::string("standalone service contract field is empty: ") +
                                 field);
    }
    return values;
}

std::string identifier_field(const json& document, const char* field) {
    if (!document.contains(field) || !document.at(field).is_string()) {
        throw std::runtime_error(std::string("standalone service contract field is not a string: ") +
                                 field);
    }
    const std::string value = document.at(field).get<std::string>();
    if (!valid_identifier(value)) {
        throw std::runtime_error(std::string("standalone service contract identifier is invalid: ") +
                                 field);
    }
    return value;
}

bool compatible_manager_api_version(const std::string_view version) {
    return version.size() > 2 && version.starts_with("1.") &&
           std::ranges::all_of(
               version.substr(2),
               [](const unsigned char character) {
                   return std::isdigit(character) != 0;
               });
}

json validate_manager_service_api(const json& declaration,
                                  const json& component_manifest) {
    if (!declaration.is_object() ||
        declaration.value("schema", "") !=
            "uci_arena_manager_service_api_v1" ||
        !declaration.contains("version") ||
        !declaration.at("version").is_string() ||
        !compatible_manager_api_version(
            declaration.at("version").get_ref<const std::string&>()) ||
        declaration.value("transport", "") != "cli_json" ||
        declaration.value("entrypoint", "") != "cli" ||
        declaration.value("output_schema", "") !=
            "uci_arena_service_output_v1") {
        throw std::runtime_error(
            "standalone service Manager API declaration is incompatible");
    }
    if (!component_manifest.contains("entrypoints") ||
        !component_manifest.at("entrypoints").is_object() ||
        !component_manifest.at("entrypoints").contains("cli") ||
        !component_manifest.at("entrypoints").at("cli").is_string()) {
        throw std::runtime_error(
            "standalone service Manager API entrypoint is unavailable");
    }
    if (!declaration.contains("arguments") ||
        declaration.at("arguments") != json::array({"api"})) {
        throw std::runtime_error(
            "standalone service Manager API arguments are incompatible");
    }
    const std::set<std::string> required_operations{
        "config_validate",
        "describe",
        "outputs_inspect",
        "outputs_list",
        "status",
    };
    const std::set<std::string> operations =
        identifier_set_field(declaration, "operations");
    if (!std::ranges::includes(operations, required_operations)) {
        throw std::runtime_error(
            "standalone service Manager API operations are incomplete");
    }
    return declaration;
}

fs::path validated_contract_file(const fs::path& root,
                                 const std::set<fs::path>& listed_files,
                                 const json& contract, const char* field) {
    if (!contract.contains(field) || !contract.at(field).is_string()) {
        throw std::runtime_error(std::string("provider service contract path is missing: ") +
                                 field);
    }
    const fs::path relative =
        fs::path(contract.at(field).get<std::string>()).lexically_normal();
    if (!safe_relative_path(relative) || !listed_files.contains(relative)) {
        throw std::runtime_error(std::string("provider service contract path is unsafe or ") +
                                 "unlisted: " + field);
    }
    const fs::path absolute = root / relative;
    if (!fs::is_regular_file(absolute) || fs::is_symlink(absolute)) {
        throw std::runtime_error(std::string("provider service contract file is unavailable: ") +
                                 field);
    }
    return relative;
}

void validate_closed_json_schema(const fs::path& path, const char* label) {
    const json schema = read_json(path);
    if (!schema.is_object() || schema.value("$schema", "") !=
                                   "https://json-schema.org/draft/2020-12/schema" ||
        schema.value("type", "") != "object" ||
        !schema.contains("additionalProperties") ||
        !schema.at("additionalProperties").is_boolean() ||
        schema.at("additionalProperties").get<bool>()) {
        throw std::runtime_error(std::string("provider ") + label +
                                 " must be a closed JSON Schema object");
    }
}

void validate_manager_operation_catalog(const fs::path& path,
                                        const fs::path& expected_entrypoint,
                                        const std::string& expected_contract_version) {
    const json catalog = read_json(path);
    const std::string catalog_schema = catalog.value("schema", "");
    if (!catalog.is_object() ||
        !valid_identifier(catalog_schema) || !catalog_schema.ends_with("_manager_api_v1") ||
        !catalog.contains("version") || !catalog.at("version").is_string() ||
        catalog.at("version").get_ref<const std::string&>() != expected_contract_version ||
        !compatible_manager_api_version(
            catalog.at("version").get_ref<const std::string&>()) ||
        !catalog.contains("transport") || !catalog.at("transport").is_object() ||
        !catalog.contains("operations") || !catalog.at("operations").is_object() ||
        catalog.at("operations").empty()) {
        throw std::runtime_error("provider Manager operation catalog is incompatible");
    }
    const json& transport = catalog.at("transport");
    if (transport.value("kind", "") != "cli_json" ||
        transport.value("entrypoint", "") != expected_entrypoint.generic_string() ||
        transport.value("stdout", "") != "one_bounded_json_document" ||
        !transport.contains("shell") || !transport.at("shell").is_boolean() ||
        transport.at("shell").get<bool>()) {
        throw std::runtime_error("provider Manager transport is incompatible");
    }
    for (const auto& [name, operation] : catalog.at("operations").items()) {
        if (!valid_identifier(name) || !operation.is_object() ||
            !operation.contains("arguments") || !operation.at("arguments").is_array() ||
            operation.at("arguments").empty() ||
            !operation.contains("credential") || !operation.at("credential").is_string()) {
            throw std::runtime_error("provider Manager operation is invalid: " + name);
        }
        const std::string credential = operation.at("credential").get<std::string>();
        if (credential != "forbidden" && credential != "external_file_path_only") {
            throw std::runtime_error("provider Manager operation credential boundary is invalid: " +
                                     name);
        }
        for (const auto& argument : operation.at("arguments")) {
            if (!argument.is_string() || !valid_identifier(argument.get<std::string>())) {
                throw std::runtime_error("provider Manager operation argument is invalid: " +
                                         name);
            }
        }
    }
    if (!catalog.at("operations").contains("provider_status")) {
        throw std::runtime_error(
            "provider Manager operation catalog has no provider status operation");
    }
    const json& provider_status = catalog.at("operations").at("provider_status");
    if (!provider_status.contains("mutates") ||
        !provider_status.at("mutates").is_boolean() ||
        provider_status.at("mutates").get<bool>() ||
        provider_status.value("credential", "") != "forbidden" ||
        !provider_status.contains("response") ||
        !provider_status.at("response").is_string() ||
        !valid_identifier(provider_status.at("response").get<std::string>())) {
        throw std::runtime_error(
            "provider Manager status operation contract is incompatible");
    }
}

json validate_provider_service_contract(const fs::path& root, const json& manifest,
                                        const std::set<fs::path>& listed_files,
                                        const fs::path& contract_relative,
                                        const json& contract) {
    const std::string component_id = manifest.at("component_id").get<std::string>();
    const std::string component_version = manifest.at("component_version").get<std::string>();
    if (!contract.contains("service") || !contract.at("service").is_object()) {
        throw std::runtime_error("provider service identity is missing");
    }
    const json& service = contract.at("service");
    if (service.value("component_id", "") != component_id ||
        service.value("component_version", "") != component_version ||
        !service.contains("contract_version") ||
        !service.at("contract_version").is_string() ||
        !compatible_manager_api_version(
            service.at("contract_version").get_ref<const std::string&>()) ||
        service.value("transport", "") != "cli_json" ||
        !service.contains("entrypoint") || !service.at("entrypoint").is_string() ||
        !manifest.contains("entrypoints") || !manifest.at("entrypoints").is_object() ||
        !manifest.at("entrypoints").contains("cli") ||
        !manifest.at("entrypoints").at("cli").is_string()) {
        throw std::runtime_error("provider service identity or transport is incompatible");
    }
    const fs::path service_entrypoint =
        fs::path(service.at("entrypoint").get<std::string>()).lexically_normal();
    const fs::path component_entrypoint = fs::path(
        manifest.at("entrypoints").at("cli").get<std::string>()).lexically_normal();
    if (!safe_relative_path(service_entrypoint) ||
        service_entrypoint != component_entrypoint ||
        !listed_files.contains(service_entrypoint)) {
        throw std::runtime_error("provider service CLI entrypoint does not match the component");
    }
    static_cast<void>(identifier_set_field(contract, "provided_capabilities"));
    static_cast<void>(identifier_set_field(contract, "required_capabilities"));
    if (!contract.contains("invocation") ||
        !contract.at("invocation").is_object()) {
        throw std::runtime_error("provider invocation contract is missing");
    }
    const json& invocation = contract.at("invocation");
    const std::string invocation_kind = invocation.value("kind", "");
    if (invocation_kind == "direct") {
        if (invocation.size() != 1) {
            throw std::runtime_error("direct provider invocation has unexpected fields");
        }
    } else if (invocation_kind == "dependency_runtime") {
        if (invocation.size() != 3 ||
            !invocation.contains("runtime_component_id") ||
            !invocation.at("runtime_component_id").is_string() ||
            !invocation.contains("runtime_entrypoint") ||
            !invocation.at("runtime_entrypoint").is_string()) {
            throw std::runtime_error("provider dependency-runtime invocation is malformed");
        }
        const std::string runtime_component =
            invocation.at("runtime_component_id").get<std::string>();
        const std::string runtime_entrypoint =
            invocation.at("runtime_entrypoint").get<std::string>();
        if (!valid_identifier(runtime_component) ||
            !valid_identifier(runtime_entrypoint) ||
            !manifest.contains("dependencies") ||
            !manifest.at("dependencies").is_array() ||
            std::ranges::none_of(
                manifest.at("dependencies"),
                [&runtime_component](const json& dependency) {
                    return dependency.is_string() &&
                           dependency.get_ref<const std::string&>() == runtime_component;
                })) {
            throw std::runtime_error(
                "provider invocation runtime is not a declared component dependency");
        }
    } else {
        throw std::runtime_error("provider invocation kind is incompatible");
    }
    if (!contract.contains("credential_contract") ||
        !contract.at("credential_contract").is_object() ||
        contract.at("credential_contract").value("input", "") !=
            "external_absolute_file_path" ||
        contract.at("credential_contract").value("contents_visible_to", "") !=
            "provider_transport_only" ||
        contract.at("credential_contract").value("manager_projection", "") !=
            "path_and_redacted_status_only" ||
        contract.at("credential_contract").value("installer_behavior", "") !=
            "never_package_copy_or_inspect") {
        throw std::runtime_error("provider credential contract is incompatible");
    }
    if (!contract.contains("startup") || !contract.at("startup").is_object() ||
        !manifest.contains("startup") || contract.at("startup") != manifest.at("startup") ||
        !contract.at("startup").value("credential_gated", false)) {
        throw std::runtime_error("provider startup ownership does not match the component");
    }
    if (!contract.contains("persistence") || !contract.at("persistence").is_object() ||
        contract.at("persistence").value("upgrade", "") != "preserve_and_migrate" ||
        contract.at("persistence").value("repair", "") != "preserve" ||
        contract.at("persistence").value("uninstall_default", "") != "preserve") {
        throw std::runtime_error("provider persistence contract is incompatible");
    }
    if (!contract.contains("error_contract") || !contract.at("error_contract").is_object() ||
        contract.at("error_contract").value("format", "") != "bounded_typed_json" ||
        contract.at("error_contract").value("secrets", "") != "redacted" ||
        contract.at("error_contract").value("unknown_operation", "") != "fail_closed" ||
        contract.at("error_contract").value("unknown_major_version", "") != "incompatible") {
        throw std::runtime_error("provider error contract is incompatible");
    }

    const fs::path operations_relative =
        validated_contract_file(root, listed_files, contract, "manager_operations");
    const fs::path configuration_relative =
        validated_contract_file(root, listed_files, contract, "configuration_schema");
    const fs::path configuration_update_relative = validated_contract_file(
        root, listed_files, contract, "configuration_update_schema");
    const fs::path status_relative =
        validated_contract_file(root, listed_files, contract, "status_schema");
    validate_manager_operation_catalog(
        root / operations_relative, service_entrypoint,
        service.at("contract_version").get_ref<const std::string&>());
    validate_closed_json_schema(root / configuration_relative, "configuration schema");
    validate_closed_json_schema(root / configuration_update_relative,
                                "configuration-update schema");
    validate_closed_json_schema(root / status_relative, "status schema");

    return {
        {"schema", "uci_arena_service_contract_v1"},
        {"path", contract_relative.generic_string()},
        {"sha256", sha256_file(root / contract_relative)},
        {"configuration_schema_path", configuration_relative.generic_string()},
        {"configuration_schema_sha256", sha256_file(root / configuration_relative)},
        {"configuration_update_schema_path", configuration_update_relative.generic_string()},
        {"configuration_update_schema_sha256",
         sha256_file(root / configuration_update_relative)},
        {"status_schema_path", status_relative.generic_string()},
        {"status_schema_sha256", sha256_file(root / status_relative)},
        {"manager_operations_path", operations_relative.generic_string()},
        {"manager_operations_sha256", sha256_file(root / operations_relative)},
        {"credential_gated_startup", true},
    };
}

std::optional<json> validate_standalone_service_contract(
    const fs::path& root, const json& manifest, const std::set<fs::path>& listed_files) {
    const std::string component_id = manifest.at("component_id").get<std::string>();
    const std::string component_version = manifest.at("component_version").get<std::string>();
    if (!manifest.contains("contract_manifest")) {
        if (manifest.contains("manager_integration_api")) {
            throw std::runtime_error(
                "integrated provider component is missing its contract manifest");
        }
        return std::nullopt;
    }
    if (!manifest.at("contract_manifest").is_string()) {
        throw std::runtime_error("component contract manifest path is not a string");
    }
    const fs::path contract_relative =
        fs::path(manifest.at("contract_manifest").get<std::string>()).lexically_normal();
    if (!safe_relative_path(contract_relative) || !listed_files.contains(contract_relative)) {
        throw std::runtime_error(
            "component contract manifest is unsafe or absent from the payload manifest");
    }
    const fs::path contract_path = root / contract_relative;
    if (!fs::is_regular_file(contract_path) || fs::is_symlink(contract_path)) {
        throw std::runtime_error("component contract manifest is not a regular payload file");
    }
    const json contract = read_json(contract_path);
    if (contract.is_object() &&
        contract.value("schema", "") == "uci_arena_service_contract_v1") {
        return validate_provider_service_contract(root, manifest, listed_files,
                                                  contract_relative, contract);
    }
    if (!contract.is_object() ||
        contract.value("schema", "") != "uci_arena_standalone_service_contract_v1" ||
        contract.value("component_id", "") != component_id ||
        contract.value("component_version", "") != component_version) {
        throw std::runtime_error(
            "standalone service contract identity does not match the component");
    }
    if (!contract.contains("cli_entrypoint") || !contract.at("cli_entrypoint").is_string() ||
        !manifest.contains("entrypoints") || !manifest.at("entrypoints").is_object() ||
        !manifest.at("entrypoints").contains("cli") ||
        !manifest.at("entrypoints").at("cli").is_string()) {
        throw std::runtime_error(
            "standalone service contract or component is missing its CLI entrypoint");
    }
    const fs::path contract_cli =
        fs::path(contract.at("cli_entrypoint").get<std::string>()).lexically_normal();
    const fs::path component_cli =
        fs::path(manifest.at("entrypoints").at("cli").get<std::string>()).lexically_normal();
    if (!safe_relative_path(contract_cli) || contract_cli != component_cli ||
        !listed_files.contains(contract_cli)) {
        throw std::runtime_error(
            "standalone service contract CLI entrypoint does not match the component");
    }
    const auto contract_capabilities = identifier_set_field(contract, "capabilities");
    const auto component_capabilities = identifier_set_field(manifest, "capabilities");
    if (contract_capabilities != component_capabilities ||
        !contract_capabilities.contains("status")) {
        throw std::runtime_error(
            "standalone service contract capabilities do not match the component");
    }
    if (identifier_set_field(contract, "data_paths") !=
        identifier_set_field(manifest, "data_paths")) {
        throw std::runtime_error(
            "standalone service contract data paths do not match the component");
    }
    const std::string configuration_schema = identifier_field(contract, "configuration_schema");
    const std::string status_schema = identifier_field(contract, "status_schema");
    if (!contract.contains("startup") || !contract.at("startup").is_object() ||
        !manifest.contains("startup") || !manifest.at("startup").is_object() ||
        contract.at("startup") != manifest.at("startup")) {
        throw std::runtime_error(
            "standalone service contract startup ownership does not match the component");
    }
    if (!contract.contains("health_smoke") ||
        !contract.at("health_smoke").is_object() ||
        contract.at("health_smoke").value("command", "") != "status" ||
        contract.at("health_smoke").value("success", "") !=
            "exit_zero_and_status_schema") {
        throw std::runtime_error(
            "standalone service contract health smoke is invalid");
    }
    if (!contract.contains("outputs") || !contract.at("outputs").is_object() ||
        !contract.contains("persistence") || !contract.at("persistence").is_object()) {
        throw std::runtime_error(
            "standalone service contract outputs or persistence declaration is invalid");
    }
    const json& persistence = contract.at("persistence");
    if (persistence.value("upgrade", "") != "preserve_and_migrate" ||
        persistence.value("repair", "") != "preserve" ||
        persistence.value("uninstall_default", "") != "preserve") {
        throw std::runtime_error(
            "standalone service contract does not preserve persistent service data");
    }
    std::optional<json> manager_integration_api;
    if (manifest.contains("manager_integration_api") ||
        contract.contains("manager_integration_api")) {
        if (!manifest.contains("manager_integration_api") ||
            !contract.contains("manager_integration_api") ||
            manifest.at("manager_integration_api") !=
                contract.at("manager_integration_api")) {
            throw std::runtime_error(
                "standalone service Manager API declarations do not match");
        }
        manager_integration_api = validate_manager_service_api(
            manifest.at("manager_integration_api"), manifest);
    }
    json projection{
        {"schema", "uci_arena_standalone_service_contract_v1"},
        {"path", contract_relative.generic_string()},
        {"sha256", sha256_file(contract_path)},
        {"configuration_schema", configuration_schema},
        {"status_schema", status_schema},
    };
    if (manager_integration_api.has_value()) {
        projection["manager_integration_api"] =
            *manager_integration_api;
    }
    return projection;
}

struct ValidatedComponentPayload {
    json manifest;
    std::optional<json> service_contract;
};

bool valid_discovery_kind(std::string_view kind);
json validate_book_locator(const fs::path& path);
json validate_syzygy_locator(const fs::path& path);
json inspect_timing_evidence_candidate(const fs::path& path);
json inspect_lichess_profile_candidate(const fs::path& path,
                                       std::string_view schema_prefix,
                                       std::string_view current_schema);

bool valid_knowledge_activation_kind(const std::string_view kind) {
    return kind == "opening_book" || kind == "syzygy";
}

bool valid_provider_locator_source(const std::string_view source) {
    return source == "saved_locator" || source == "install_receipt";
}

void validate_provider_installer_integration(const json& manifest,
                                             const std::set<fs::path>& listed_files) {
    if (!manifest.contains("installer_integration")) {
        return;
    }
    const json& integration = manifest.at("installer_integration");
    if (!integration.is_object() || integration.size() < 9 || integration.size() > 11 ||
        integration.value("schema", "") != "arena_provider_installer_integration_v1" ||
        integration.value("schema_version", 0) != 1 ||
        !integration.contains("entrypoint") ||
        !integration.at("entrypoint").is_string() ||
        !valid_identifier(integration.at("entrypoint").get<std::string>()) ||
        integration.value("arguments", json::array()) != json::array({"--context"}) ||
        !integration.contains("workspace_name") ||
        !integration.at("workspace_name").is_string() ||
        !valid_identifier(integration.at("workspace_name").get<std::string>()) ||
        integration.value("workspace_name", "") != manifest.value("workspace_name", "") ||
        (integration.value("workspace_placement", "workspace_root") != "workspace_root" &&
         integration.value("workspace_placement", "workspace_root") != "product_data") ||
        !integration.contains("configure_when_disabled") ||
        !integration.at("configure_when_disabled").is_boolean() ||
        !integration.contains("dependency_bindings") ||
        !integration.at("dependency_bindings").is_array() ||
        integration.at("dependency_bindings").size() > 32 ||
        !integration.contains("locator_bindings") ||
        !integration.at("locator_bindings").is_array() ||
        integration.at("locator_bindings").size() > 32) {
        throw std::runtime_error(
            "provider installer-integration contract is invalid: " +
            manifest.value("component_id", "unknown"));
    }
    const std::string integration_entrypoint =
        integration.at("entrypoint").get<std::string>();
    if (!manifest.contains("entrypoints") ||
        !manifest.at("entrypoints").contains(integration_entrypoint) ||
        !manifest.at("entrypoints").at(integration_entrypoint).is_string()) {
        throw std::runtime_error("provider installer-integration entrypoint is unavailable");
    }
    const fs::path integration_relative = fs::path(
        manifest.at("entrypoints").at(integration_entrypoint).get<std::string>())
                                               .lexically_normal();
    if (!safe_relative_path(integration_relative) ||
        !listed_files.contains(integration_relative)) {
        throw std::runtime_error("provider installer-integration entrypoint is unlisted");
    }
    if (!manifest.contains("dependencies") || !manifest.at("dependencies").is_array()) {
        throw std::runtime_error("provider installer-integration dependencies are unavailable");
    }
    std::set<std::string> dependencies;
    for (const auto& dependency : manifest.at("dependencies")) {
        if (!dependency.is_string() ||
            !valid_identifier(dependency.get_ref<const std::string&>()) ||
            !dependencies.insert(dependency.get<std::string>()).second) {
            throw std::runtime_error("component dependency declaration is invalid");
        }
    }
    const json& invocation = integration.at("invocation");
    if (!invocation.is_object() || invocation.size() != 3 ||
        invocation.value("kind", "") != "dependency_runtime" ||
        !invocation.contains("runtime_component_id") ||
        !invocation.at("runtime_component_id").is_string() ||
        !invocation.contains("runtime_entrypoint") ||
        !invocation.at("runtime_entrypoint").is_string() ||
        !valid_identifier(invocation.at("runtime_component_id").get<std::string>()) ||
        !valid_identifier(invocation.at("runtime_entrypoint").get<std::string>()) ||
        !dependencies.contains(invocation.at("runtime_component_id").get<std::string>())) {
        throw std::runtime_error("provider installer-integration invocation is invalid");
    }
    std::set<std::string> binding_names;
    for (const auto& binding : integration.at("dependency_bindings")) {
        if (!binding.is_object() || !binding.contains("name") ||
            !binding.at("name").is_string() ||
            !valid_identifier(binding.at("name").get<std::string>()) ||
            !binding_names.insert(binding.at("name").get<std::string>()).second ||
            !binding.contains("component_id") ||
            !binding.at("component_id").is_string() ||
            !valid_identifier(binding.at("component_id").get<std::string>()) ||
            !binding.contains("required") || !binding.at("required").is_boolean()) {
            throw std::runtime_error("provider installer dependency binding is invalid");
        }
        const std::string source = binding.value("source", "");
        if (source == "entrypoint") {
            if (binding.size() != 5 || !binding.contains("entrypoint") ||
                !binding.at("entrypoint").is_string() ||
                !valid_identifier(binding.at("entrypoint").get<std::string>())) {
                throw std::runtime_error(
                    "provider installer entrypoint binding is invalid");
            }
        } else if (source == "component_path") {
            if (binding.size() != 5 || !binding.contains("path") ||
                !binding.at("path").is_string() ||
                !safe_relative_path(binding.at("path").get<std::string>())) {
                throw std::runtime_error("provider installer path binding is invalid");
            }
        } else if (source != "active_root" || binding.size() != 4) {
            throw std::runtime_error("provider installer dependency source is invalid");
        }
        if (binding.at("required").get<bool>() &&
            !dependencies.contains(binding.at("component_id").get<std::string>())) {
            throw std::runtime_error(
                "required provider installer binding is not a component dependency");
        }
    }
    std::set<std::string> locator_names;
    std::set<std::string> locator_kinds;
    for (const auto& binding : integration.at("locator_bindings")) {
        if (!binding.is_object() || (binding.size() != 3 && binding.size() != 4) ||
            (binding.size() == 4 && !binding.contains("accepted_sources")) ||
            !binding.contains("name") || !binding.at("name").is_string() ||
            !valid_identifier(binding.at("name").get<std::string>()) ||
            !locator_names.insert(binding.at("name").get<std::string>()).second ||
            !binding.contains("kind") || !binding.at("kind").is_string() ||
            !valid_discovery_kind(binding.at("kind").get<std::string>()) ||
            !locator_kinds.insert(binding.at("kind").get<std::string>()).second ||
            !binding.contains("required") || !binding.at("required").is_boolean()) {
            throw std::runtime_error("provider installer locator binding is invalid");
        }
        if (binding.contains("accepted_sources")) {
            const json& accepted_sources = binding.at("accepted_sources");
            std::set<std::string> unique_sources;
            if (!accepted_sources.is_array() || accepted_sources.empty() ||
                accepted_sources.size() > 2) {
                throw std::runtime_error(
                    "provider installer locator sources are invalid");
            }
            for (const auto& source : accepted_sources) {
                if (!source.is_string() ||
                    !valid_provider_locator_source(source.get<std::string>()) ||
                    !unique_sources.insert(source.get<std::string>()).second) {
                    throw std::runtime_error(
                        "provider installer locator sources are invalid");
                }
            }
        }
    }
    bool declares_startup = false;
    const json capabilities = manifest.value("capabilities", json::array());
    if (!capabilities.is_array()) {
        throw std::runtime_error("component capabilities are not an array");
    }
    for (const auto& capability : capabilities) {
        if (!capability.is_string() ||
            !valid_identifier(capability.get_ref<const std::string&>())) {
            throw std::runtime_error("component capability declaration is invalid");
        }
        declares_startup = declares_startup ||
                           capability == "startup_install" ||
                           capability == "startup_uninstall";
    }
    if (declares_startup != integration.contains("startup_control")) {
        throw std::runtime_error(
            "provider startup capability and installer control contract disagree");
    }
    if (!declares_startup) {
        return;
    }
    const ProviderLifecycleControl lifecycle_control = [&]() {
        try {
            return parse_provider_lifecycle_control(
                integration.at("startup_control"));
        } catch (const std::invalid_argument&) {
            throw std::runtime_error(
                "provider lifecycle control contract is invalid");
        }
    }();
    if (!manifest.at("entrypoints").contains(lifecycle_control.entrypoint) ||
        !manifest.at("entrypoints").at(lifecycle_control.entrypoint).is_string()) {
        throw std::runtime_error("provider lifecycle entrypoint is unavailable");
    }
}

fs::path provider_workspace(const SetupPaths& paths, const json& integration) {
    const std::string workspace_name =
        integration.at("workspace_name").get<std::string>();
    const std::string placement =
        integration.value("workspace_placement", "workspace_root");
    const fs::path root = placement == "product_data"
                              ? paths.data_root / "provider-data"
                              : paths.workspace_root;
    const fs::path workspace = normalized_absolute(root / workspace_name);
    if (!within(root, workspace)) {
        throw std::runtime_error("provider workspace escapes its declared placement");
    }
    return workspace;
}

void validate_component_discovery(const json& manifest,
                                  const std::set<fs::path>& listed_files) {
    if (!manifest.contains("discovery")) {
        return;
    }
    const json& declarations = manifest.at("discovery");
    if (!declarations.is_array() || declarations.empty() || declarations.size() > 16) {
        throw std::runtime_error("component discovery declaration is invalid");
    }
    std::set<std::string> kinds;
    for (const auto& declaration : declarations) {
        if (!declaration.is_object() || declaration.size() != 2 ||
            !declaration.contains("kind") || !declaration.at("kind").is_string() ||
            !valid_discovery_kind(declaration.at("kind").get<std::string>()) ||
            !kinds.insert(declaration.at("kind").get<std::string>()).second ||
            !declaration.contains("locator") || !declaration.at("locator").is_object()) {
            throw std::runtime_error("component discovery entry is invalid");
        }
        const json& locator = declaration.at("locator");
        const std::string source = locator.value("source", "");
        if (source == "active_root") {
            if (locator.size() != 1) {
                throw std::runtime_error("active-root discovery locator is invalid");
            }
            continue;
        }
        if (source == "entrypoint") {
            if (locator.size() != 2 || !locator.contains("entrypoint") ||
                !locator.at("entrypoint").is_string() ||
                !valid_identifier(locator.at("entrypoint").get<std::string>()) ||
                !manifest.contains("entrypoints") ||
                !manifest.at("entrypoints").is_object() ||
                !manifest.at("entrypoints").contains(
                    locator.at("entrypoint").get<std::string>())) {
                throw std::runtime_error("entrypoint discovery locator is invalid");
            }
            continue;
        }
        if (source == "component_path") {
            if (locator.size() != 2 || !locator.contains("path") ||
                !locator.at("path").is_string()) {
                throw std::runtime_error("component-path discovery locator is invalid");
            }
            const fs::path relative =
                fs::path(locator.at("path").get<std::string>()).lexically_normal();
            if (!safe_relative_path(relative)) {
                throw std::runtime_error("component-path discovery locator is unsafe");
            }
            const std::string prefix = relative.generic_string() + "/";
            const bool listed = std::ranges::any_of(listed_files, [&](const fs::path& file) {
                const std::string path = file.generic_string();
                return file == relative || path.starts_with(prefix);
            });
            if (!listed) {
                throw std::runtime_error("component-path discovery locator is unlisted");
            }
            continue;
        }
        throw std::runtime_error("component discovery locator source is invalid");
    }
}

void validate_component_activation(const json& manifest,
                                    const std::set<fs::path>& listed_files) {
    if (!manifest.contains("activation")) {
        return;
    }
    const json& declarations = manifest.at("activation");
    if (!declarations.is_array() || declarations.empty() || declarations.size() > 4) {
        throw std::runtime_error("component activation declaration is invalid");
    }
    const auto overlaps = [](const fs::path& left, const fs::path& right) {
        const fs::path normalized_left = left.lexically_normal();
        const fs::path normalized_right = right.lexically_normal();
        const auto is_prefix = [](const fs::path& prefix, const fs::path& candidate) {
            auto prefix_it = prefix.begin();
            auto candidate_it = candidate.begin();
            for (; prefix_it != prefix.end(); ++prefix_it, ++candidate_it) {
                if (candidate_it == candidate.end() || *prefix_it != *candidate_it) {
                    return false;
                }
            }
            return true;
        };
        return is_prefix(normalized_left, normalized_right) ||
               is_prefix(normalized_right, normalized_left);
    };
    std::set<std::string> kinds;
    std::vector<fs::path> sources;
    std::vector<fs::path> destinations;
    for (const auto& declaration : declarations) {
        if (!declaration.is_object() || declaration.size() != 4 ||
            !declaration.contains("kind") || !declaration.at("kind").is_string() ||
            !valid_knowledge_activation_kind(declaration.at("kind").get<std::string>()) ||
            !kinds.insert(declaration.at("kind").get<std::string>()).second ||
            !declaration.contains("source_path") ||
            !declaration.at("source_path").is_string() ||
            declaration.value("destination_root", "") != "knowledge_root" ||
            !declaration.contains("destination_path") ||
            !declaration.at("destination_path").is_string()) {
            throw std::runtime_error("component activation entry is invalid");
        }
        const fs::path source =
            fs::path(declaration.at("source_path").get<std::string>()).lexically_normal();
        const fs::path destination =
            fs::path(declaration.at("destination_path").get<std::string>()).lexically_normal();
        if (!safe_relative_path(source) || !safe_relative_path(destination)) {
            throw std::runtime_error("component activation path is unsafe");
        }
        const std::string source_prefix = source.generic_string() + "/";
        if (!std::ranges::any_of(listed_files, [&](const fs::path& file) {
                return file.generic_string().starts_with(source_prefix);
            })) {
            throw std::runtime_error("component activation source has no listed payload");
        }
        if (std::ranges::any_of(sources, [&](const fs::path& existing) {
                return overlaps(existing, source);
            }) ||
            std::ranges::any_of(destinations, [&](const fs::path& existing) {
                return overlaps(existing, destination);
            })) {
            throw std::runtime_error("component activation paths overlap");
        }
        const bool matching_discovery =
            manifest.contains("discovery") && manifest.at("discovery").is_array() &&
            std::ranges::any_of(manifest.at("discovery"), [&](const json& discovery) {
                if (!discovery.is_object() ||
                    discovery.value("kind", "") != declaration.at("kind").get<std::string>() ||
                    !discovery.contains("locator") ||
                    !discovery.at("locator").is_object()) {
                    return false;
                }
                const json& locator = discovery.at("locator");
                return locator.value("source", "") == "component_path" &&
                       locator.contains("path") && locator.at("path").is_string() &&
                       fs::path(locator.at("path").get<std::string>()).lexically_normal() == source;
            });
        if (!matching_discovery) {
            throw std::runtime_error(
                "component activation has no matching discovery declaration");
        }
        sources.push_back(source);
        destinations.push_back(destination);
    }
}

ValidatedComponentPayload validate_component_payload(const fs::path& root,
                                                     const std::string& expected_id,
                                                     const std::string& expected_version) {
    json manifest = read_json(root / "arena-component.json");
    if (manifest.value("schema", "") != "arena_install_component_v1" ||
        manifest.value("component_id", "") != expected_id ||
        manifest.value("component_version", "") != expected_version) {
        throw std::runtime_error("component payload identity does not match the catalog");
    }
    std::set<fs::path> listed_files;
    for (const auto& file : manifest.at("files")) {
        const fs::path relative = file.at("path").get<std::string>();
        if (!safe_relative_path(relative)) {
            throw std::runtime_error("component manifest contains an unsafe path");
        }
        if (!listed_files.insert(relative.lexically_normal()).second) {
            throw std::runtime_error("component manifest contains a duplicate path");
        }
        const fs::path path = root / relative;
        const fs::path api_path = platform::filesystem_api_path(path);
        if (!fs::is_regular_file(api_path) ||
            fs::file_size(api_path) != integer_field(file, "size_bytes") ||
            sha256_file(api_path) != file.at("sha256").get<std::string>()) {
            throw std::runtime_error("component payload verification failed for " +
                                     relative.generic_string());
        }
    }
    if (manifest.contains("entrypoints")) {
        if (!manifest.at("entrypoints").is_object()) {
            throw std::runtime_error("component entrypoints are not an object");
        }
        for (const auto& [name, value] : manifest.at("entrypoints").items()) {
            if (!valid_identifier(name) || !value.is_string()) {
                throw std::runtime_error("component entrypoint declaration is invalid");
            }
            const fs::path relative = fs::path(value.get<std::string>()).lexically_normal();
            if (!safe_relative_path(relative) || !listed_files.contains(relative)) {
                throw std::runtime_error("component entrypoint is unsafe or unlisted: " + name);
            }
        }
    }
    if (manifest.contains("configuration_template")) {
        if (!manifest.at("configuration_template").is_string()) {
            throw std::runtime_error("component configuration template path is invalid");
        }
        const fs::path relative = fs::path(
            manifest.at("configuration_template").get<std::string>()).lexically_normal();
        if (!safe_relative_path(relative) || !listed_files.contains(relative)) {
            throw std::runtime_error(
                "component configuration template is unsafe or unlisted");
        }
    }
    validate_provider_installer_integration(manifest, listed_files);
    validate_component_discovery(manifest, listed_files);
    validate_component_activation(manifest, listed_files);
    const fs::path api_root = platform::filesystem_api_path(root);
    for (const auto& entry : fs::recursive_directory_iterator(api_root)) {
        if (entry.is_symlink()) {
            throw std::runtime_error("component payload contains a symbolic link");
        }
        if (!entry.is_regular_file()) {
            continue;
        }
        const fs::path relative = fs::relative(entry.path(), api_root).lexically_normal();
        if (relative != fs::path("arena-component.json") && !listed_files.contains(relative)) {
            throw std::runtime_error("component payload contains an unlisted file: " +
                                     relative.generic_string());
        }
    }
    auto service_contract =
        validate_standalone_service_contract(root, manifest, listed_files);
    return {std::move(manifest), std::move(service_contract)};
}

void copy_directory_contents(const fs::path& source, const fs::path& destination,
                             std::vector<fs::path>& copied_files) {
    if (!fs::is_directory(source)) {
        throw std::runtime_error("knowledge component is missing " + source.filename().string());
    }
    fs::create_directories(destination);
    for (const auto& entry : fs::recursive_directory_iterator(source)) {
        if (entry.is_symlink()) {
            throw std::runtime_error("knowledge payload contains a symbolic link");
        }
        const fs::path relative = fs::relative(entry.path(), source);
        const fs::path target = destination / relative;
        if (!within(destination, target)) {
            throw std::runtime_error("knowledge payload escapes its activation root");
        }
        if (entry.is_directory()) {
            fs::create_directories(target);
        } else if (entry.is_regular_file()) {
            fs::create_directories(target.parent_path());
            fs::copy_file(entry.path(), target, fs::copy_options::overwrite_existing);
            copied_files.push_back(relative);
        } else {
            throw std::runtime_error("knowledge payload contains an unsupported entry");
        }
    }
}

json declared_knowledge_resources(const fs::path& component_root, const json& manifest,
                                  const SetupPaths& paths) {
    json resources = json::array();
    for (const auto& declaration : manifest.value("activation", json::array())) {
        const fs::path source_relative =
            fs::path(declaration.at("source_path").get<std::string>()).lexically_normal();
        const fs::path destination_relative =
            fs::path(declaration.at("destination_path").get<std::string>()).lexically_normal();
        const fs::path source = normalized_absolute(component_root / source_relative);
        const fs::path destination = normalized_absolute(paths.knowledge_root / destination_relative);
        if (!within(component_root, source) || !within(paths.knowledge_root, destination)) {
            throw std::runtime_error("knowledge activation path escapes its declared root");
        }
        json activated_files = json::array();
        const std::string source_prefix = source_relative.generic_string() + "/";
        for (const auto& file : manifest.at("files")) {
            const fs::path relative =
                fs::path(file.at("path").get<std::string>()).lexically_normal();
            if (!relative.generic_string().starts_with(source_prefix)) {
                continue;
            }
            const fs::path resource_relative = relative.lexically_relative(source_relative);
            activated_files.push_back({
                {"path", normalized_absolute(destination / resource_relative).string()},
                {"sha256", file.at("sha256")},
                {"size_bytes", file.at("size_bytes")},
            });
        }
        resources.push_back({
            {"kind", declaration.at("kind")},
            {"source_path", source.string()},
            {"path", destination.string()},
            {"activated_files", std::move(activated_files)},
        });
    }
    return resources;
}

json plan_knowledge_activation(const fs::path& component_root, const json& manifest,
                               const SetupPaths& paths, const std::string& transaction_id,
                               const std::string& component_id) {
    json plan = declared_knowledge_resources(component_root, manifest, paths);
    for (auto& activation : plan) {
        const fs::path source = activation.at("source_path").get<std::string>();
        const fs::path destination = activation.at("path").get<std::string>();
        if (!fs::is_directory(source) || contains_symbolic_link(source)) {
            throw std::runtime_error("knowledge activation source is unavailable or unsafe");
        }
        fs::create_directories(destination.parent_path());
        if (contains_symbolic_link(destination.parent_path()) ||
            (fs::exists(destination) &&
             (!fs::is_directory(destination) || contains_symbolic_link(destination)))) {
            throw std::runtime_error("knowledge activation destination is unsafe");
        }
        const std::string suffix = transaction_id + "-" + component_id + "-" +
                                   activation.at("kind").get<std::string>();
        activation["candidate_path"] =
            (destination.parent_path() /
             (destination.filename().string() + ".staging-" + suffix))
                .string();
        activation["backup_path"] =
            (destination.parent_path() /
             (destination.filename().string() + ".rollback-" + suffix))
                .string();
        activation["had_existing"] = fs::exists(destination);
        activation["state"] = "planned";
    }
    return plan;
}

void apply_knowledge_activation(
    json& plan, const std::function<void(const json&)>& checkpoint) {
    for (auto& activation : plan) {
        const fs::path source = activation.at("source_path").get<std::string>();
        const fs::path destination = activation.at("path").get<std::string>();
        const fs::path candidate = activation.at("candidate_path").get<std::string>();
        const fs::path backup = activation.at("backup_path").get<std::string>();
        const bool had_existing = activation.at("had_existing").get<bool>();
        if (fs::exists(destination) != had_existing) {
            throw std::runtime_error(
                "knowledge activation destination changed after planning");
        }
        std::error_code error;
        fs::remove_all(candidate, error);
        if (error) {
            throw std::runtime_error("cannot clear knowledge activation candidate: " +
                                     error.message());
        }
        fs::remove_all(backup, error);
        if (error) {
            throw std::runtime_error("cannot clear knowledge activation rollback path: " +
                                     error.message());
        }
        std::vector<fs::path> copied_files;
        if (had_existing) {
            copy_directory_contents(destination, candidate, copied_files);
            copied_files.clear();
        }
        copy_directory_contents(source, candidate, copied_files);
        activation["state"] = "prepared";
        checkpoint(plan);
        if (had_existing) {
            fs::rename(destination, backup);
            activation["state"] = "prior_saved";
            checkpoint(plan);
        }
        fs::rename(candidate, destination);
        activation["state"] = "activated";
        checkpoint(plan);
    }
}

json receipt_activated_resources(const json& plan) {
    json resources = json::array();
    for (const auto& activation : plan) {
        resources.push_back({
            {"kind", activation.at("kind")},
            {"path", activation.at("path")},
            {"activated_files", activation.at("activated_files")},
        });
    }
    return resources;
}

json flattened_activated_files(const json& resources) {
    json files = json::array();
    for (const auto& resource : resources) {
        for (const auto& file : resource.at("activated_files")) {
            files.push_back(file);
        }
    }
    return files;
}

json expected_receipt_knowledge_resources(const fs::path& active_root, const json& manifest,
                                          const SetupPaths& paths, const json& receipt) {
    json declared = declared_knowledge_resources(active_root, manifest, paths);
    if (!declared.empty()) {
        return receipt_activated_resources(declared);
    }
    json resources = json::array();
    if (receipt.value("import_mode", "") != "managed_copy") {
        return resources;
    }
    const json declarations = manifest.value("discovery", json::array());
    const auto discovery = std::ranges::find_if(
        declarations, [](const json& declaration) {
            return declaration.is_object() &&
                   valid_knowledge_activation_kind(declaration.value("kind", "")) &&
                   declaration.contains("locator") && declaration.at("locator").is_object() &&
                   declaration.at("locator").value("source", "") == "active_root";
        });
    if (discovery == declarations.end() || !within(paths.knowledge_root, active_root)) {
        return resources;
    }
    json activated_files = json::array();
    for (const auto& file : manifest.at("files")) {
        const fs::path relative =
            fs::path(file.at("path").get<std::string>()).lexically_normal();
        activated_files.push_back({
            {"path", normalized_absolute(active_root / relative).string()},
            {"sha256", file.at("sha256")},
            {"size_bytes", file.at("size_bytes")},
        });
    }
    resources.push_back({
        {"kind", discovery->at("kind")},
        {"path", normalized_absolute(active_root).string()},
        {"activated_files", std::move(activated_files)},
    });
    return resources;
}

std::vector<std::string> selected_ids(const json& catalog, const std::string& preset,
                                      const std::vector<std::string>& selected,
                                      const bool download_missing_knowledge,
                                      const bool download_missing_books,
                                      const bool download_missing_tablebases) {
    std::vector<std::string> result;
    if (preset == "custom") {
        result = selected;
    } else {
        if (!catalog.at("presets").contains(preset)) {
            throw std::runtime_error("unknown installation preset " + preset);
        }
        result = catalog.at("presets").at(preset).get<std::vector<std::string>>();
    }
    const auto select_once = [&result](const std::string& id) {
        if (std::find(result.begin(), result.end(), id) == result.end()) {
            result.push_back(id);
        }
    };
    const auto select_group = [&](const std::string& group) {
        const auto& groups = catalog.value(
            "optional_component_groups", json::object());
        if (!groups.contains(group) || !groups.at(group).is_array()) {
            throw std::runtime_error(
                "catalog does not declare optional component group " + group);
        }
        for (const auto& id : groups.at(group)) {
            select_once(id.get<std::string>());
        }
    };
    if (download_missing_knowledge) select_group("knowledge");
    if (download_missing_books) select_group("books");
    if (download_missing_tablebases) select_group("tablebases");

    std::map<std::string, int> visits;
    std::vector<std::string> ordered;
    std::function<void(const std::string&)> add = [&](const std::string& id) {
        if (!valid_identifier(id)) {
            throw std::runtime_error("invalid component id " + id);
        }
        if (visits[id] == 1) {
            throw std::runtime_error("component dependency cycle at " + id);
        }
        if (visits[id] == 2) {
            return;
        }
        visits[id] = 1;
        const auto& component = component_by_id(catalog, id);
        for (const auto& dependency : component.value("dependencies", json::array())) {
            add(dependency.get<std::string>());
        }
        visits[id] = 2;
        ordered.push_back(id);
    };
    for (const auto& id : result) {
        add(id);
    }
    return ordered;
}

json receipt_rows(const SetupPaths& paths) {
    json rows = json::array();
    const fs::path directory = paths.data_root / "receipts";
    if (!fs::is_directory(directory) || contains_symbolic_link(directory)) {
        return rows;
    }
    std::vector<fs::path> receipt_paths;
    for (const auto& entry : fs::directory_iterator(directory)) {
        if (entry.is_regular_file() && !entry.is_symlink() && entry.path().extension() == ".json") {
            receipt_paths.push_back(entry.path());
        }
    }
    std::ranges::sort(receipt_paths, {}, [](const fs::path& path) {
        return path.filename().generic_string();
    });
    for (const auto& receipt_path : receipt_paths) {
        try {
            rows.push_back(read_json(receipt_path));
        } catch (const std::exception& error) {
            rows.push_back({{"schema", "arena_install_receipt_v1"},
                            {"state", "broken"},
                            {"receipt", receipt_path.string()},
                            {"error", error.what()}});
        }
    }
    return rows;
}

const std::array<std::string_view, 11> discovery_kinds{
    "opening_book",
    "syzygy",
    "uci_engine",
    "book_forge",
    "evidence_service",
    "timing_profile",
    "lichess_single_game_profile",
    "lichess_capacity_profile",
    "lichess_transport_profile",
    "lichess_token_path",
    "restaurant",
};

bool valid_discovery_kind(const std::string_view kind) {
    return std::find(discovery_kinds.begin(), discovery_kinds.end(), kind) != discovery_kinds.end();
}

json browse_contract(const std::string_view kind) {
    json result{
        {"selection", "file"},
        {"filters", json::array()},
        {"hints", json::array()},
        {"managed_copy_allowed", false},
    };
    if (kind == "opening_book") {
        result["selection"] = "file_or_directory";
        result["filters"].push_back(
            {{"label", "Opening books"}, {"patterns", {"*.bin", "*.book"}}});
        result["hints"] = {
            "Select a book file or a flat directory containing its sidecars.",
            "Reference in place is the default; managed copy is limited to 256 "
            "MiB.",
        };
        result["managed_copy_allowed"] = true;
    } else if (kind == "syzygy") {
        result["selection"] = "directory";
        result["filters"].push_back(
            {{"label", "Syzygy tablebases"}, {"patterns", {"*.rtbw", "*.rtbz"}}});
        result["hints"] = {
            "Select the directory containing the tablebase files.",
            "Tablebases are always referenced in place.",
        };
    } else if (kind == "uci_engine") {
#ifdef _WIN32
        result["filters"].push_back({{"label", "UCI engine executables"}, {"patterns", {"*.exe"}}});
#else
        result["filters"].push_back({{"label", "UCI engine executables"}, {"patterns", {"*"}}});
#endif
        result["hints"] = {
            "Any executable is accepted only after a complete bounded UCI probe.",
        };
    } else if (kind == "book_forge") {
        result["selection"] = "file_or_directory";
        result["filters"].push_back(
            {{"label", "Book Forge provider"}, {"patterns", {"arena-component.json", "*.mjs"}}});
        result["hints"] = {
            "Select a provider root containing arena-component.json or its CLI "
            "entrypoint.",
        };
    } else if (kind == "evidence_service") {
        result["selection"] = "file_or_directory";
        result["filters"].push_back({{"label", "Evidence Service provider"},
                                     {"patterns", {"arena-component.json", "*.mjs"}}});
        result["hints"] = {
            "Select a provider root containing arena-component.json or its CLI "
            "entrypoint.",
        };
    } else if (kind == "timing_profile") {
        result["filters"].push_back({{"label", "Qualified UCI timing profile"},
                                     {"patterns", {"uci_time_profile_v3.json", "*.json"}}});
        result["hints"] = {
            "Select the exact qualified uci_time_profile_v3.json published for "
            "this engine, model, "
            "provider, and hardware.",
            "The profile remains an in-place reference and is rejected at runtime "
            "if its identity "
            "is stale.",
        };
    } else if (kind == "lichess_single_game_profile") {
        result["filters"].push_back(
            {{"label", "Qualified Lichess single-game profile"},
             {"patterns", {"uci_arena_single_game_profile_v1.json", "*.json"}}});
        result["hints"] = {
            "Select the qualified single-game profile for the exact installed engine, model, "
            "provider, and hardware.",
            "The profile and its bounded hash-verified evidence closure are copied unchanged "
            "into immutable Manager storage.",
        };
        result["managed_copy_allowed"] = true;
    } else if (kind == "lichess_capacity_profile") {
        result["filters"].push_back(
            {{"label", "Qualified Lichess capacity profile"},
             {"patterns", {"uci_arena_lichess_capacity_profile_v2.json", "*.json"}}});
        result["hints"] = {
            "Select the qualified v2 capacity profile that matches the timing authority, "
            "single-game profile, and configured concurrency.",
            "The profile remains in place and is never rewritten.",
        };
    } else if (kind == "lichess_transport_profile") {
        result["filters"].push_back(
            {{"label", "Qualified Lichess transport profile"},
             {"patterns", {"transport_profile_v2.json", "*.json"}}});
        result["hints"] = {
            "Select the qualified Lichess transport-reserve profile for this host.",
            "The profile remains in place and is never rewritten.",
        };
    } else if (kind == "lichess_token_path") {
        result["filters"].push_back(
            {{"label", "Token reference files"}, {"patterns", {"*.txt", "*.token", "*"}}});
        result["hints"] = {
            "Only the path is saved; token contents are never opened, copied, "
            "hashed, logged, or "
            "returned.",
        };
    } else if (kind == "restaurant") {
        result["selection"] = "directory";
        result["hints"] = {
            "Restaurant is detect-only and cannot be imported, installed, or "
            "controlled.",
        };
    }
    return result;
}

std::set<fs::path> manifest_listed_files(const json& manifest) {
    if (!manifest.contains("files") || !manifest.at("files").is_array()) {
        throw std::runtime_error("component manifest file inventory is unavailable");
    }
    std::set<fs::path> listed_files;
    for (const auto& file : manifest.at("files")) {
        if (!file.is_object() || !file.contains("path") || !file.at("path").is_string()) {
            throw std::runtime_error("component manifest file inventory is invalid");
        }
        const fs::path relative =
            fs::path(file.at("path").get<std::string>()).lexically_normal();
        if (!safe_relative_path(relative) || !listed_files.insert(relative).second) {
            throw std::runtime_error("component manifest file inventory is unsafe");
        }
    }
    return listed_files;
}

fs::path component_discovery_locator(const fs::path& root, const json& manifest,
                                     const json& declaration) {
    const json& locator = declaration.at("locator");
    const std::string source = locator.at("source").get<std::string>();
    fs::path selected = root;
    if (source == "entrypoint") {
        const std::string entrypoint = locator.at("entrypoint").get<std::string>();
        selected /= manifest.at("entrypoints").at(entrypoint).get<std::string>();
    } else if (source == "component_path") {
        selected /= locator.at("path").get<std::string>();
    }
    selected = normalized_absolute(selected);
    const fs::path normalized_root = normalized_absolute(root);
    if (selected != normalized_root && !within(normalized_root, selected)) {
        throw std::runtime_error("component discovery locator escapes its root");
    }
    return selected;
}

void append_discovery_candidate(json& rows, std::map<std::string, std::set<std::string>>& seen,
                                const std::string& kind, const std::string& source,
                                const fs::path& path, const std::string& state,
                                const json& details = json::object()) {
    if (!valid_discovery_kind(kind) || path.empty()) {
        return;
    }
    const std::string normalized = normalized_absolute(path).string();
    const std::string key = source + "\n" + normalized;
    if (!seen[kind].insert(key).second) {
        return;
    }
    json candidate{
        {"source", source},
        {"state", state},
        {"path", normalized},
    };
    if (!details.empty()) {
        candidate["details"] = details;
    }
    rows.at(kind).at("candidates").push_back(candidate);
}

int discovery_source_rank(const std::string_view source) noexcept {
    if (source == "saved_locator") {
        return 0;
    }
    if (source == "install_receipt") {
        return 1;
    }
    if (source == "provider_manifest") {
        return 2;
    }
    if (source == "indexed") {
        return 3;
    }
    if (source == "discovered") {
        return 4;
    }
    return 5;
}

std::uint64_t discovery_quality_score(const json& candidate) noexcept {
    try {
        const json& details = candidate.at("details");
        const json& validation = details.at("validation");
        if (validation.contains("quality_score") &&
            validation.at("quality_score").is_number_unsigned()) {
            return validation.at("quality_score").get<std::uint64_t>();
        }
    } catch (const std::exception&) {
    }
    return 0;
}

void select_discovery_candidates(json& rows) {
    for (const auto kind : discovery_kinds) {
        const json* best = nullptr;
        for (const auto& candidate : rows.at(std::string(kind)).at("candidates")) {
            const std::string state = candidate.value("state", "");
            if (state == "broken" || state == "unresolved") {
                continue;
            }
            if (best == nullptr) {
                best = &candidate;
                continue;
            }
            const int rank = discovery_source_rank(candidate.value("source", ""));
            const int best_rank = discovery_source_rank(best->value("source", ""));
            const std::uint64_t quality = discovery_quality_score(candidate);
            const std::uint64_t best_quality = discovery_quality_score(*best);
            const std::string path = candidate.value("path", "");
            const std::string best_path = best->value("path", "");
            const std::string source = candidate.value("source", "");
            const std::string best_source = best->value("source", "");
            const bool same_managed_resource =
                path == best_path &&
                ((source == "install_receipt" && best_source == "saved_locator") ||
                 (source == "saved_locator" && best_source == "install_receipt"));
            const bool better_by_precedence =
                rank < best_rank ||
                (rank == best_rank &&
                 (quality > best_quality ||
                  (quality == best_quality && path < best_path)));
            if ((same_managed_resource && source == "install_receipt") ||
                (!same_managed_resource && better_by_precedence)) {
                best = &candidate;
            }
        }
        if (best != nullptr) {
            rows.at(std::string(kind))["selection"] = *best;
        } else {
            rows.at(std::string(kind))["selection"] = {
                {"source", "unresolved"},
                {"state", "unresolved"},
                {"path", nullptr},
            };
        }
    }
}

void inspect_provider_root(json& rows, std::map<std::string, std::set<std::string>>& seen,
                           const fs::path& root) {
    if (!fs::is_directory(root) || contains_symbolic_link(root)) {
        return;
    }
    const fs::path manifest_path = root / "arena-component.json";
    std::error_code error;
    if (!fs::is_regular_file(manifest_path) ||
        fs::is_symlink(fs::symlink_status(manifest_path, error)) || error ||
        fs::file_size(manifest_path, error) > 1024ULL * 1024ULL || error) {
        return;
    }
    try {
        const json manifest = read_json(manifest_path);
        if (manifest.value("schema", "") != "arena_install_component_v1") {
            return;
        }
        const auto listed_files = manifest_listed_files(manifest);
        validate_component_discovery(manifest, listed_files);
        const std::string component_id = manifest.value("component_id", "");
        for (const auto& declaration : manifest.value("discovery", json::array())) {
            const std::string kind = declaration.at("kind").get<std::string>();
            const fs::path locator = component_discovery_locator(root, manifest, declaration);
            const bool healthy = fs::exists(locator) && !contains_symbolic_link(locator);
            append_discovery_candidate(
                rows, seen, kind, "provider_manifest", locator,
                healthy ? "detected_unmanaged" : "broken",
                {{"component_id", component_id},
                 {"component_version", manifest.value("component_version", "")}});
        }
    } catch (const std::exception&) {
    }
}

std::vector<fs::path> bounded_root_candidates(const std::vector<fs::path>& roots) {
    std::map<std::string, fs::path> candidates;
    for (const auto& candidate_root : roots) {
        if (candidate_root.empty()) {
            continue;
        }
        const fs::path root = normalized_absolute(candidate_root);
        if (contains_symbolic_link(root)) {
            continue;
        }
        if (fs::is_regular_file(root)) {
            candidates.emplace(root.string(), root);
            continue;
        }
        if (!fs::is_directory(root)) {
            continue;
        }
        candidates.emplace(root.string(), root);
        std::error_code error;
        for (fs::directory_iterator
                 iterator(root, fs::directory_options::skip_permission_denied, error),
             end;
             iterator != end && !error; iterator.increment(error)) {
            if (iterator->is_directory(error) && !error && !iterator->is_symlink(error) && !error &&
                !contains_symbolic_link(iterator->path())) {
                const fs::path child = normalized_absolute(iterator->path());
                candidates.emplace(child.string(), child);
            }
            error.clear();
        }
    }
    std::vector<fs::path> result;
    result.reserve(candidates.size());
    for (auto& [key, path] : candidates) {
        (void)key;
        result.push_back(std::move(path));
    }
    return result;
}

void inspect_provider_roots(json& rows, std::map<std::string, std::set<std::string>>& seen,
                            const std::vector<fs::path>& roots) {
    for (const auto& root : bounded_root_candidates(roots)) {
        if (!fs::is_directory(root)) {
            continue;
        }
        inspect_provider_root(rows, seen, root);
    }
}

bool directory_has_extension(const fs::path& root, const std::set<std::string>& extensions) {
    if (!fs::is_directory(root) || contains_symbolic_link(root)) {
        return false;
    }
    std::error_code error;
    for (fs::directory_iterator
             iterator(root, fs::directory_options::skip_permission_denied, error),
         end;
         iterator != end && !error; iterator.increment(error)) {
        if (!iterator->is_regular_file(error) || error || iterator->is_symlink(error) || error) {
            error.clear();
            continue;
        }
        std::string extension = iterator->path().extension().string();
        std::ranges::transform(extension, extension.begin(), [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        if (extensions.contains(extension)) {
            return true;
        }
    }
    return false;
}

json validate_book_locator(const fs::path& path);
json inspect_indexed_book_candidate(const fs::path& path);
json validate_syzygy_locator(const fs::path& path);
struct InspectedIndexedCandidate {
    fs::path path;
    json validation;
};

InspectedIndexedCandidate inspect_indexed_asset_candidate(
    const ports::IndexedAssetCandidate& candidate) {
    fs::path selected = candidate.path;
    if (candidate.kind == ports::IndexedAssetKind::opening_book) {
        return {selected, inspect_indexed_book_candidate(selected)};
    }
    if (candidate.kind == ports::IndexedAssetKind::syzygy) {
        if (fs::is_regular_file(selected)) {
            selected = selected.parent_path();
        }
        return {selected, validate_syzygy_locator(selected)};
    }
    if (candidate.kind == ports::IndexedAssetKind::timing_profile) {
        return {selected, inspect_timing_evidence_candidate(selected)};
    }
    if (candidate.kind == ports::IndexedAssetKind::lichess_single_game_profile) {
        return {selected,
                inspect_lichess_profile_candidate(
                    selected, "uci_arena_single_game_profile_v",
                    "uci_arena_single_game_profile_v1")};
    }
    if (candidate.kind == ports::IndexedAssetKind::lichess_capacity_profile) {
        return {selected,
                inspect_lichess_profile_candidate(
                    selected, "uci_arena_lichess_capacity_profile_v",
                    "uci_arena_lichess_capacity_profile_v2")};
    }
    return {selected,
            inspect_lichess_profile_candidate(
                selected, "uci_arena_lichess_transport_profile_v",
                "uci_arena_lichess_transport_profile_v2")};
}

ports::IndexedAssetQuery indexed_asset_query() {
    return {
        256,
        std::chrono::milliseconds(2000),
        {
            {ports::IndexedAssetKind::opening_book, {".book", ".bin"}, {}},
            {ports::IndexedAssetKind::syzygy, {".rtbw", ".rtbz"}, {}},
            {ports::IndexedAssetKind::timing_profile,
             {},
             {"uci_time_profile_v*.json", "*timing*profile*.json",
              "*time*profile*.json"}},
            {ports::IndexedAssetKind::lichess_single_game_profile,
             {},
             {"uci_arena_single_game_profile_v*.json",
              "*single*game*profile*.json"}},
            {ports::IndexedAssetKind::lichess_capacity_profile,
             {},
             {"uci_arena_lichess_capacity_profile_v*.json",
              "*lichess*capacity*profile*.json"}},
            {ports::IndexedAssetKind::lichess_transport_profile,
             {},
             {"transport_profile_v*.json",
              "*lichess*transport*profile*.json"}},
        },
    };
}

json indexed_search_projection(const ports::IndexedAssetSearchResult& result,
                               const std::string_view kind,
                               const std::size_t accepted,
                               const std::size_t rejected) {
    const std::size_t returned = static_cast<std::size_t>(std::count_if(
        result.candidates.begin(), result.candidates.end(),
        [kind](const ports::IndexedAssetCandidate& candidate) {
            return ports::indexed_asset_kind_name(candidate.kind) == kind;
        }));
    return {
        {"attempted", true},
        {"adapter", result.adapter},
        {"state", ports::indexed_search_state_name(result.state)},
        {"detail_code", result.detail_code},
        {"maximum_results", result.maximum_results},
        {"returned_candidates", returned},
        {"total_returned_candidates", result.candidates.size()},
        {"accepted_candidates", accepted},
        {"rejected_candidates", rejected},
        {"elapsed_ms", result.elapsed.count()},
        {"truncated", result.truncated},
        {"ownership", "external_dependency"},
        {"registration", "explicit_import_required"},
    };
}

std::optional<std::string> saved_locator_authority_mode(const json& locator,
                                                        const std::string& kind);

json discovery_rows(const SetupPaths& paths, const std::vector<fs::path>& extra_roots,
                    ports::AssetSearchIndex* search_index) {
    json rows = json::object();
    for (const auto kind : discovery_kinds) {
        rows[std::string(kind)] = {
            {"schema", "arena_setup_discovery_v1"},
            {"schema_version", 1},
            {"kind", kind},
            {"precedence",
             {"saved_locator", "install_receipt", "provider_manifest", "indexed",
              "discovered", "unresolved"}},
            {"selection", nullptr},
            {"candidates", json::array()},
            {"browse", browse_contract(kind)},
            {"indexed_search",
             {{"attempted", false},
              {"state", "not_requested"},
              {"ownership", "external_dependency"},
              {"registration", "explicit_import_required"}}},
        };
    }
    std::map<std::string, std::set<std::string>> seen;

    for (const auto& receipt : receipt_rows(paths)) {
        const std::string state = receipt.value("state", "");
        if ((state != "installed_managed" && state != "installed_disabled") ||
            !receipt.contains("active_root") || !receipt.at("active_root").is_string()) {
            continue;
        }
        try {
            const fs::path active_root =
                normalized_absolute(receipt.at("active_root").get<std::string>());
            const bool managed_component_root = within(paths.install_root, active_root);
            const bool managed_knowledge_root = within(paths.knowledge_root, active_root);
            if ((!managed_component_root && !managed_knowledge_root) ||
                contains_symbolic_link(active_root)) {
                continue;
            }
            const json manifest = read_json(active_root / "arena-component.json");
            if (manifest.value("schema", "") != "arena_install_component_v1" ||
                manifest.value("component_id", "") != receipt.value("component_id", "") ||
                manifest.value("component_version", "") !=
                    receipt.value("component_version", "")) {
                continue;
            }
            const auto listed_files = manifest_listed_files(manifest);
            validate_component_discovery(manifest, listed_files);
            validate_component_activation(manifest, listed_files);
            const json activated_resources =
                expected_receipt_knowledge_resources(active_root, manifest, paths, receipt);
            if (receipt.value("activated_resources", json::array()) != activated_resources ||
                receipt.value("activated_files", json::array()) !=
                    flattened_activated_files(activated_resources)) {
                throw std::runtime_error(
                    "receipt knowledge activation evidence does not match the manifest");
            }
            std::set<std::string> activated_kinds;
            for (const auto& resource : activated_resources) {
                const std::string kind = resource.at("kind").get<std::string>();
                const fs::path resource_path = resource.at("path").get<std::string>();
                bool healthy = within(paths.knowledge_root, resource_path) &&
                               fs::is_directory(resource_path) &&
                               !contains_symbolic_link(resource_path);
                for (const auto& file : resource.at("activated_files")) {
                    const fs::path path = file.at("path").get<std::string>();
                    healthy = healthy && within(resource_path, path) && fs::is_regular_file(path) &&
                              !fs::is_symlink(path) &&
                              fs::file_size(path) == integer_field(file, "size_bytes");
                }
                append_discovery_candidate(
                    rows, seen, kind, "install_receipt", resource_path,
                    healthy ? state : "broken",
                    {{"component_id", receipt.value("component_id", "")},
                     {"component_version", receipt.value("component_version", "")}});
                activated_kinds.insert(kind);
            }
            for (const auto& declaration : manifest.value("discovery", json::array())) {
                const std::string kind = declaration.at("kind").get<std::string>();
                if (activated_kinds.contains(kind)) {
                    continue;
                }
                const fs::path locator =
                    component_discovery_locator(active_root, manifest, declaration);
                const bool healthy = fs::exists(locator) && !contains_symbolic_link(locator);
                append_discovery_candidate(
                    rows, seen, kind, "install_receipt", locator,
                    healthy ? state : "broken",
                    {{"component_id", receipt.value("component_id", "")},
                     {"component_version", receipt.value("component_version", "")}});
            }
        } catch (const std::exception&) {
        }
    }

    const fs::path locator_root = paths.data_root / "locators";
    if (fs::is_directory(locator_root) && !contains_symbolic_link(locator_root)) {
        for (const auto kind : discovery_kinds) {
            const fs::path locator_path = locator_root / (std::string(kind) + ".json");
            if (!fs::is_regular_file(locator_path) || contains_symbolic_link(locator_path)) {
                continue;
            }
            try {
                const json locator = read_json(locator_path);
                if (locator.value("schema", "") != "arena_saved_locator_v1" ||
                    locator.value("schema_version", 0) != 1 || locator.value("kind", "") != kind ||
                    !locator.contains("path") || !locator.at("path").is_string()) {
                    continue;
                }
                const fs::path selected = locator.at("path").get<std::string>();
                bool healthy = fs::exists(selected) && !contains_symbolic_link(selected);
                json details{{"mode", locator.value("mode", "in_place_reference")}};
                if (const auto authority =
                        saved_locator_authority_mode(locator, std::string(kind));
                    authority.has_value()) {
                    details["authority_mode"] = *authority;
                }
                if (healthy) {
                    try {
                        if (kind == "opening_book") {
                            details["validation"] = validate_book_locator(selected);
                        } else if (kind == "syzygy") {
                            details["validation"] = validate_syzygy_locator(selected);
                        } else if (kind == "timing_profile") {
                            details["validation"] = inspect_timing_evidence_candidate(selected);
                        } else if (kind == "lichess_single_game_profile") {
                            details["validation"] = inspect_lichess_profile_candidate(
                                selected, "uci_arena_single_game_profile_v",
                                "uci_arena_single_game_profile_v1");
                        } else if (kind == "lichess_capacity_profile") {
                            details["validation"] = inspect_lichess_profile_candidate(
                                selected, "uci_arena_lichess_capacity_profile_v",
                                "uci_arena_lichess_capacity_profile_v2");
                        } else if (kind == "lichess_transport_profile") {
                            details["validation"] = inspect_lichess_profile_candidate(
                                selected, "uci_arena_lichess_transport_profile_v",
                                "uci_arena_lichess_transport_profile_v2");
                        } else if (locator.contains("validation") &&
                                   locator.at("validation").is_object()) {
                            details["validation"] = locator.at("validation");
                        }
                    } catch (const std::exception&) {
                        healthy = false;
                        details["validation"] = {
                            {"classification", "unreadable_or_invalid"},
                            {"activation", "requires_explicit_reselection"},
                        };
                    }
                }
                append_discovery_candidate(rows, seen, std::string(kind), "saved_locator", selected,
                                            healthy ? "saved" : "broken",
                                            details);
            } catch (const std::exception&) {
            }
        }
    }

    std::vector<fs::path> bounded_roots{
        paths.install_root,
        paths.workspace_root,
        paths.data_root / "provider-data",
        paths.knowledge_root,
    };
    bounded_roots.insert(bounded_roots.end(), extra_roots.begin(), extra_roots.end());
    inspect_provider_roots(rows, seen, bounded_roots);

    if (search_index != nullptr) {
        const ports::IndexedAssetSearchResult indexed =
            search_index->search(indexed_asset_query());
        std::map<std::string, std::size_t> accepted;
        std::map<std::string, std::size_t> rejected;
        for (const auto& candidate : indexed.candidates) {
            const std::string kind(
                ports::indexed_asset_kind_name(candidate.kind));
            try {
                InspectedIndexedCandidate inspected =
                    inspect_indexed_asset_candidate(candidate);
                append_discovery_candidate(
                    rows, seen, kind, "indexed", inspected.path,
                    "detected_unmanaged",
                    {{"adapter", indexed.adapter},
                     {"ownership", "external_dependency"},
                     {"validation", std::move(inspected.validation)}});
                ++accepted[kind];
            } catch (const std::exception&) {
                ++rejected[kind];
            }
        }
        for (const auto kind : {"opening_book", "syzygy", "timing_profile",
                                "lichess_single_game_profile",
                                "lichess_capacity_profile",
                                "lichess_transport_profile"}) {
            rows.at(kind)["indexed_search"] = indexed_search_projection(
                indexed, kind, accepted[kind], rejected[kind]);
        }
    }

    const fs::path book_root = paths.knowledge_root / "opening_books";
    if (directory_has_extension(book_root, {".bin", ".book"})) {
        try {
            append_discovery_candidate(rows, seen, "opening_book", "discovered", book_root,
                                       "detected_unmanaged",
                                       {{"validation", validate_book_locator(book_root)}});
        } catch (const std::exception&) {
        }
    }
    const fs::path tablebase_root = paths.knowledge_root / "tablebases";
    if (directory_has_extension(tablebase_root, {".rtbw", ".rtbz"})) {
        append_discovery_candidate(rows, seen, "syzygy", "discovered", tablebase_root,
                                   "detected_unmanaged",
                                   {{"validation", validate_syzygy_locator(tablebase_root)}});
    }

    const auto selected_roots = bounded_root_candidates(extra_roots);
    for (const auto& selected : selected_roots) {
        if (fs::is_regular_file(selected)) {
            std::string extension = selected.extension().string();
            std::ranges::transform(extension, extension.begin(), [](const unsigned char character) {
                return static_cast<char>(std::tolower(character));
            });
            if (extension == ".bin" || extension == ".book") {
                append_discovery_candidate(rows, seen, "opening_book", "discovered", selected,
                                           "detected_unmanaged",
                                           {{"validation", validate_book_locator(selected)}});
            }
            continue;
        }
        if (directory_has_extension(selected, {".bin", ".book"})) {
            try {
                append_discovery_candidate(rows, seen, "opening_book", "discovered", selected,
                                           "detected_unmanaged",
                                           {{"validation", validate_book_locator(selected)}});
            } catch (const std::exception&) {
            }
        }
        if (directory_has_extension(selected, {".rtbw", ".rtbz"})) {
            append_discovery_candidate(rows, seen, "syzygy", "discovered", selected,
                                       "detected_unmanaged",
                                       {{"validation", validate_syzygy_locator(selected)}});
        }
    }

    const ports::IndexedAssetQuery asset_query = indexed_asset_query();
    std::map<std::string, fs::path> bounded_files;
    for (const auto& candidate_root : extra_roots) {
        if (candidate_root.empty()) {
            continue;
        }
        const fs::path root = normalized_absolute(candidate_root);
        if (contains_symbolic_link(root)) {
            continue;
        }
        if (fs::is_regular_file(root) && !fs::is_symlink(root)) {
            bounded_files.emplace(root.string(), root);
            continue;
        }
        if (!fs::is_directory(root)) {
            continue;
        }
        std::error_code error;
        for (fs::directory_iterator
                 iterator(root, fs::directory_options::skip_permission_denied, error),
             end;
             iterator != end && !error; iterator.increment(error)) {
            if (iterator->is_regular_file(error) && !error &&
                !iterator->is_symlink(error) && !error) {
                const fs::path child = normalized_absolute(iterator->path());
                bounded_files.emplace(child.string(), child);
            }
            error.clear();
        }
    }
    for (const auto& [key, file] : bounded_files) {
        (void)key;
        for (const auto& rule : asset_query.rules) {
            if (rule.kind == ports::IndexedAssetKind::syzygy ||
                !ports::indexed_asset_rule_matches(rule, file)) {
                continue;
            }
            try {
                const ports::IndexedAssetCandidate candidate{rule.kind, file};
                InspectedIndexedCandidate inspected =
                    inspect_indexed_asset_candidate(candidate);
                append_discovery_candidate(
                    rows, seen, std::string(ports::indexed_asset_kind_name(rule.kind)),
                    "discovered", inspected.path, "detected_unmanaged",
                    {{"ownership", "external_dependency"},
                     {"validation", std::move(inspected.validation)}});
            } catch (const std::exception&) {
            }
        }
    }

    if (const char* path_environment = std::getenv("PATH")) {
#ifdef _WIN32
        constexpr char separator = ';';
#else
        constexpr char separator = ':';
#endif
        std::stringstream stream(path_environment);
        for (std::string item; std::getline(stream, item, separator);) {
            if (!item.empty()) {
                bounded_roots.emplace_back(item);
            }
        }
    }
    const std::set<std::string> engine_names{
#ifdef _WIN32
        "uci_arena_engine.exe", "stockfish.exe", "lc0.exe"
#else
        "uci_arena_engine", "stockfish", "lc0"
#endif
    };
    std::set<std::string> engine_roots;
    for (const auto& root : bounded_root_candidates(bounded_roots)) {
        if (fs::is_regular_file(root)) {
            if (engine_names.contains(root.filename().string())) {
                append_discovery_candidate(rows, seen, "uci_engine", "discovered", root,
                                           "detected_unmanaged", {{"requires_probe", true}});
            }
            continue;
        }
        if (!engine_roots.insert(root.string()).second || !fs::is_directory(root)) {
            continue;
        }
        for (const auto& name : engine_names) {
            const fs::path executable = root / name;
            if (fs::is_regular_file(executable) && !contains_symbolic_link(executable)) {
                append_discovery_candidate(rows, seen, "uci_engine", "discovered", executable,
                                           "detected_unmanaged", {{"requires_probe", true}});
            }
        }
    }

    const std::array<std::tuple<const char*, const char*, const char*>, 7> environment_hints{{
        {"BOOK_FORGE_CLI", "book_forge", "provider_cli"},
        {"UCI_ARENA_EVIDENCE_CLI", "evidence_service", "provider_cli"},
        {"UCI_ARENA_SINGLE_GAME_PROFILE", "lichess_single_game_profile",
         "qualified_profile"},
        {"UCI_ARENA_LICHESS_CAPACITY_PROFILE", "lichess_capacity_profile",
         "qualified_profile"},
        {"UCI_ARENA_LICHESS_TRANSPORT_PROFILE", "lichess_transport_profile",
         "qualified_profile"},
        {"LICHESS_BOT_TOKEN_PATH", "lichess_token_path", "token_path_reference"},
        {"RESTAURANT_ROOT", "restaurant", "detect_only"},
    }};
    for (const auto& [name, kind, hint] : environment_hints) {
        const char* value = std::getenv(name);
        if (value == nullptr || *value == '\0') {
            continue;
        }
        const fs::path path = value;
        if (!fs::exists(path) || contains_symbolic_link(path)) {
            continue;
        }
        append_discovery_candidate(rows, seen, kind, "discovered", path, "detected_unmanaged",
                                   {{"hint", hint}});
    }

    select_discovery_candidates(rows);
    return rows;
}

json validate_provider_locator(const fs::path& path, const std::string& expected_kind) {
    if (fs::is_regular_file(path)) {
        return {
            {"validation", "entrypoint_present"},
            {"entrypoint", path.string()},
        };
    }
    if (!fs::is_directory(path)) {
        throw std::runtime_error(
            "provider locator must be a regular CLI entrypoint or provider root");
    }
    const fs::path manifest_path = path / "arena-component.json";
    if (!fs::is_regular_file(manifest_path) || contains_symbolic_link(manifest_path)) {
        throw std::runtime_error("provider root does not contain a safe arena-component.json");
    }
    std::error_code error;
    const auto size = fs::file_size(manifest_path, error);
    if (error || size > 1024ULL * 1024ULL) {
        throw std::runtime_error("provider manifest exceeds the bounded size");
    }
    const json manifest = read_json(manifest_path);
    if (manifest.value("schema", "") != "arena_install_component_v1") {
        throw std::runtime_error("provider manifest has an incompatible schema");
    }
    const auto listed_files = manifest_listed_files(manifest);
    validate_component_discovery(manifest, listed_files);
    if (!manifest.contains("discovery")) {
        throw std::runtime_error("provider manifest has no discovery declaration");
    }
    const json& declarations = manifest.at("discovery");
    const auto declaration = std::ranges::find_if(
        declarations, [&](const json& candidate) {
            return candidate.value("kind", "") == expected_kind;
        });
    if (declaration == declarations.end()) {
        throw std::runtime_error("provider manifest does not declare the requested resource kind");
    }
    const fs::path locator = component_discovery_locator(path, manifest, *declaration);
    if (!fs::exists(locator) || contains_symbolic_link(locator)) {
        throw std::runtime_error("provider manifest discovery locator is unavailable");
    }
    return {
        {"validation", "provider_manifest"},
        {"component_id", manifest.value("component_id", "")},
        {"component_version", manifest.value("component_version", "")},
        {"locator", locator.string()},
    };
}

json validate_book_locator(const fs::path& path) {
    if (fs::is_regular_file(path)) {
        const std::uint64_t bytes = fs::file_size(path);
        return {
            {"validation", "regular_file"},
            {"file_name", path.filename().string()},
            {"book_bytes", bytes},
            {"quality_score", bytes},
        };
    }
    if (!fs::is_directory(path)) {
        throw std::runtime_error("opening book locator must be a regular file or flat directory");
    }
    std::uint64_t file_count = 0;
    std::uint64_t book_bytes = 0;
    bool has_book = false;
    for (const auto& entry : fs::directory_iterator(path)) {
        if (entry.is_symlink()) {
            throw std::runtime_error("opening book directory contains a symbolic link");
        }
        if (!entry.is_regular_file()) {
            throw std::runtime_error(
                "opening book directory must be flat and contain only regular files");
        }
        ++file_count;
        std::string extension = entry.path().extension().string();
        std::ranges::transform(extension, extension.begin(), [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        const bool supported_book = extension == ".bin" || extension == ".book";
        has_book = has_book || supported_book;
        if (supported_book) {
            const std::uint64_t bytes = entry.file_size();
            if (bytes > (std::numeric_limits<std::uint64_t>::max)() - book_bytes) {
                throw std::runtime_error("opening book size exceeds the supported range");
            }
            book_bytes += bytes;
        }
    }
    if (file_count == 0 || !has_book) {
        throw std::runtime_error("opening book directory contains no supported book file");
    }
    return {
        {"validation", "flat_directory"},
        {"file_count", file_count},
        {"book_bytes", book_bytes},
        {"quality_score", book_bytes},
    };
}

json validate_syzygy_locator(const fs::path& path) {
    if (!fs::is_directory(path)) {
        throw std::runtime_error("Syzygy locator must be a directory");
    }
    std::uint64_t wdl_files = 0;
    std::uint64_t dtz_files = 0;
    std::uint64_t bytes = 0;
    for (const auto& entry : fs::directory_iterator(path)) {
        if (entry.is_symlink()) {
            throw std::runtime_error("Syzygy directory contains a symbolic link");
        }
        if (!entry.is_regular_file()) {
            continue;
        }
        std::string extension = entry.path().extension().string();
        std::ranges::transform(extension, extension.begin(), [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        if (extension == ".rtbw") {
            ++wdl_files;
        } else if (extension == ".rtbz") {
            ++dtz_files;
        } else {
            continue;
        }
        const auto file_bytes = static_cast<std::uint64_t>(entry.file_size());
        if (file_bytes > (std::numeric_limits<std::uint64_t>::max)() - bytes) {
            throw std::runtime_error("Syzygy collection size exceeds the supported range");
        }
        bytes += file_bytes;
    }
    if (wdl_files == 0 && dtz_files == 0) {
        throw std::runtime_error("Syzygy directory contains no .rtbw or .rtbz files");
    }
    return {
        {"validation", "syzygy_directory"},
        {"wdl_files", wdl_files},
        {"dtz_files", dtz_files},
        {"bytes", bytes},
        {"quality_score", std::min<std::uint64_t>(
                              bytes, (std::numeric_limits<std::uint64_t>::max)() -
                                         (wdl_files + dtz_files)) +
                              wdl_files + dtz_files},
    };
}

json inspect_indexed_book_candidate(const fs::path& path) {
    if (!fs::is_regular_file(path) || contains_symbolic_link(path)) {
        throw std::runtime_error(
            "indexed opening-book candidate must be a safe regular file");
    }
    std::string extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    const std::uint64_t bytes = fs::file_size(path);
    if (extension == ".book" && bytes > 0) {
        return {
            {"validation", "opaque_book_file"},
            {"classification", "extension_candidate"},
            {"bytes", bytes},
            {"quality_score", bytes},
        };
    }
    if (extension != ".bin" || bytes < 16 || bytes % 16 != 0) {
        throw std::runtime_error(
            "indexed .bin candidate does not have Polyglot record framing");
    }
    std::ifstream input(path, std::ios::binary);
    std::array<unsigned char, 16> record{};
    std::uint64_t previous_key{};
    std::size_t inspected{};
    bool first = true;
    constexpr std::size_t maximum_records = 64;
    while (inspected < maximum_records && input.read(
               reinterpret_cast<char*>(record.data()), record.size())) {
        std::uint64_t key{};
        for (std::size_t index = 0; index < 8; ++index) {
            key = (key << 8U) | record[index];
        }
        const std::uint16_t move = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(record[8]) << 8U) | record[9]);
        const std::uint16_t promotion = (move >> 12U) & 0x7U;
        if ((!first && key < previous_key) || promotion > 4U) {
            throw std::runtime_error(
                "indexed .bin candidate does not satisfy Polyglot record semantics");
        }
        previous_key = key;
        first = false;
        ++inspected;
    }
    if (inspected == 0 || (!input.eof() && input.fail())) {
        throw std::runtime_error(
            "indexed .bin candidate could not be inspected as Polyglot");
    }
    return {
        {"validation", "polyglot_sample"},
        {"classification", "polyglot_framed"},
        {"bytes", bytes},
        {"sampled_records", inspected},
        {"quality_score", bytes},
    };
}

json inspect_timing_evidence_candidate(const fs::path& path) {
    std::error_code error;
    if (!fs::is_regular_file(path) ||
        fs::is_symlink(fs::symlink_status(path, error)) || error) {
        throw std::runtime_error(
            "timing evidence candidate must reference a safe regular file");
    }
    const auto size = fs::file_size(path, error);
    if (error || size == 0 || size > 16ULL * 1024ULL * 1024ULL) {
        throw std::runtime_error(
            "timing evidence candidate must be non-empty and no larger than 16 MiB");
    }
    const json profile = read_json(path);
    const std::string schema = profile.value("schema", "");
    const bool timing_schema =
        schema.starts_with("uci_time_profile_v") ||
        (schema.find("timing") != std::string::npos &&
         profile.is_object());
    if (!timing_schema) {
        throw std::runtime_error("JSON candidate is not timing evidence");
    }
    const json identity = profile.value("identity", json::object());
    const std::string identity_sha256 = profile.value("identity_sha256", "");
    const bool identity_valid =
        identity.is_object() && !identity.empty() &&
        identity_sha256 == "sha256:" + sha256_text(identity.dump());
    const bool qualified =
        schema == "uci_time_profile_v3" && profile.value("qualified", false) &&
        profile.value("gates", json::object()).value("passed", false);
    const std::string classification =
        schema != "uci_time_profile_v3"
            ? "legacy_parseable"
            : qualified && identity_valid ? "qualified_identity"
                                          : "parseable_incomplete";
    const std::uint64_t sample_count =
        profile.value("samples", json::array()).is_array()
            ? profile.value("samples", json::array()).size()
            : 0;
    const std::uint64_t quality_score =
        (classification == "qualified_identity"
             ? 3'000'000'000ULL
             : classification == "parseable_incomplete" ? 2'000'000'000ULL
                                                          : 1'000'000'000ULL) +
        std::min<std::uint64_t>(sample_count, 999'999'999ULL);
    return {
        {"validation", "timing_evidence_inspection"},
        {"schema", schema},
        {"sha256", sha256_file(path)},
        {"classification", classification},
        {"identity_valid", identity_valid},
        {"qualified", qualified},
        {"sample_count", sample_count},
        {"quality_score", quality_score},
        {"activation", qualified && identity_valid
                           ? "requires_semantic_timing_authority_match"
                           : "catalog_only_until_migrated_or_requalified"},
    };
}

json inspect_lichess_profile_candidate(const fs::path& path,
                                       const std::string_view schema_prefix,
                                       const std::string_view current_schema) {
    std::error_code error;
    if (!fs::is_regular_file(path) ||
        fs::is_symlink(fs::symlink_status(path, error)) || error) {
        throw std::runtime_error(
            "Lichess evidence candidate must reference a safe regular file");
    }
    const auto size = fs::file_size(path, error);
    if (error || size == 0 || size > 16ULL * 1024ULL * 1024ULL) {
        throw std::runtime_error(
            "Lichess evidence candidate must be non-empty and no larger than 16 MiB");
    }
    const json profile = read_json(path);
    if (!profile.is_object()) {
        throw std::runtime_error("Lichess evidence candidate must be a JSON object");
    }
    const std::string schema = profile.value("schema", "");
    if (!schema.starts_with(schema_prefix)) {
        throw std::runtime_error("JSON candidate is not the requested Lichess evidence kind");
    }
    const bool current = schema == current_schema;
    const bool qualified = profile.value("qualified", false);
    const std::string classification =
        current ? qualified ? "qualified_declared" : "parseable_incomplete"
                : "legacy_parseable";
    std::uint64_t evidence_count = 0;
    if (profile.contains("evidence") && profile.at("evidence").is_object()) {
        evidence_count = profile.at("evidence").size();
    }
    std::uint64_t measurement_count = 0;
    if (profile.contains("samples") && profile.at("samples").is_array()) {
        measurement_count = profile.at("samples").size();
    }
    if (profile.contains("sample_count") &&
        profile.at("sample_count").is_number_unsigned()) {
        measurement_count = (std::max)(
            measurement_count,
            profile.at("sample_count").get<std::uint64_t>());
    }
    const std::uint64_t quality_score =
        (current && qualified ? 3'000'000'000ULL
                             : current ? 2'000'000'000ULL
                                       : 1'000'000'000ULL) +
        std::min<std::uint64_t>(evidence_count * 1'000'000ULL,
                                500'000'000ULL) +
        std::min<std::uint64_t>(measurement_count, 499'999'999ULL);
    json result{
        {"validation", "lichess_evidence_inspection"},
        {"schema", schema},
        {"sha256", sha256_file(path)},
        {"classification", classification},
        {"qualified", qualified},
        {"evidence_count", evidence_count},
        {"measurement_count", measurement_count},
        {"quality_score", quality_score},
        {"activation", current && qualified
                           ? "requires_exact_import_and_runtime_validation"
                           : "catalog_only_until_migrated_or_requalified"},
    };
    if (profile.contains("published_at") && profile.at("published_at").is_string()) {
        result["published_at"] = profile.at("published_at");
    }
    if (profile.contains("generated_at") && profile.at("generated_at").is_string()) {
        result["generated_at"] = profile.at("generated_at");
    }
    if (profile.contains("maximum_concurrency") &&
        profile.at("maximum_concurrency").is_number_unsigned()) {
        result["maximum_concurrency"] = profile.at("maximum_concurrency");
    }
    return result;
}

json validate_uci_locator(const fs::path& path) {
    if (!fs::is_regular_file(path)) {
        throw std::runtime_error("UCI engine locator must be a regular file");
    }
    const auto probe = adapters::UciProcess::probe(path, path.parent_path(), {});
    if (!probe.ok) {
        throw std::runtime_error("UCI engine probe failed: " + probe.error);
    }
    return {
        {"validation", "uci_startup_interrogation"},
        {"engine_name", probe.engine_name},
        {"engine_author", probe.engine_author},
        {"fingerprint", probe.fingerprint},
        {"option_count", probe.options.size()},
        {"bestmove", probe.bestmove},
        {"elapsed_ms", probe.elapsed_ms},
    };
}

bool timing_profile_gates_passed(const json& profile) {
    static constexpr std::array<std::string_view, 10> base_gates{
        "legal_moves",
        "nonterminal_null_moves",
        "controller_disabled",
        "provider_timing_captured",
        "provider_service_reserve_valid",
        "applied_provider_service_reserve_covered",
        "applied_backup_emit_reserve_covered",
        "p99_deadline_overshoot_passed",
        "maximum_deadline_overshoot_passed",
        "passed",
    };
    static constexpr std::array<std::string_view, 10> mcgs_gates{
        "mcgs_root_coverage_passed",
        "mcgs_policy_only_fallback_passed",
        "mcgs_graph_population_passed",
        "mcgs_async_overlap_passed",
        "mcgs_single_owner_passed",
        "mcgs_low_budget_fallback_passed",
        "mcgs_deterministic_fallback_passed",
        "mcgs_completion_integrity_passed",
        "mcgs_temporal_policy_passed",
        "mcgs_deadline_tail_telemetry_passed",
    };
    if (profile.value("schema", "") != "uci_time_profile_v3" ||
        !profile.value("qualified", false)) {
        return false;
    }
    const json gates = profile.value("gates", json::object());
    if (!gates.is_object()) {
        return false;
    }
    const auto all_true = [&gates](const auto& required) {
        return std::ranges::all_of(required, [&gates](const std::string_view name) {
            const std::string field{name};
            return gates.contains(field) && gates.at(field).is_boolean() &&
                   gates.at(field).get<bool>();
        });
    };
    if (!all_true(base_gates)) {
        return false;
    }
    const std::string search_core = profile.value("identity", json::object())
                                        .value("search", json::object())
                                        .value("search_core", "");
    return search_core != "mcgs" || all_true(mcgs_gates);
}

std::optional<bool> explicit_timing_permission(const json& declared,
                                               const std::string_view name) {
    const std::string field{name};
    if (!declared.contains(field)) {
        return std::nullopt;
    }
    if (!declared.at(field).is_boolean()) {
        throw std::runtime_error("timing profile permission " + field +
                                 " must be boolean");
    }
    return declared.at(field).get<bool>();
}

json timing_profile_permissions(const json& profile) {
    const json declared = profile.contains("permissions") ? profile.at("permissions")
                                                          : json::object();
    if (!declared.is_object()) {
        throw std::runtime_error("timing profile permissions must be an object");
    }
    const bool gates_passed = timing_profile_gates_passed(profile);
    const auto provider =
        explicit_timing_permission(declared, "provider_safety_qualified");
    const auto forecast =
        explicit_timing_permission(declared, "forecast_shadow_qualified");
    const auto adaptive =
        explicit_timing_permission(declared, "adaptive_play_qualified");
    return {
        {"provider_safety_qualified", gates_passed && provider != false},
        {"forecast_shadow_qualified", gates_passed && forecast != false},
        {"adaptive_play_qualified", adaptive == true},
    };
}

json validate_timing_profile_locator(const fs::path& path) {
    if (!fs::is_regular_file(path)) {
        throw std::runtime_error("timing profile locator must reference a regular file");
    }
    std::error_code error;
    const auto size = fs::file_size(path, error);
    if (error || size == 0 || size > 16ULL * 1024ULL * 1024ULL) {
        throw std::runtime_error("timing profile must be non-empty and no larger than 16 MiB");
    }
    const json profile = read_json(path);
    if (profile.value("schema", "") != "uci_time_profile_v3" ||
        profile.value("qualified", false) != true ||
        profile.value("gates", json::object()).value("passed", false) != true) {
        throw std::runtime_error("timing profile is not a qualified uci_time_profile_v3");
    }
    const json permissions = timing_profile_permissions(profile);
    if (!permissions.at("provider_safety_qualified").get<bool>()) {
        throw std::runtime_error("timing profile lacks provider-safety qualification");
    }
    const json identity = profile.value("identity", json::object());
    const std::string identity_sha256 = profile.value("identity_sha256", "");
    const std::string expected_identity_sha256 = "sha256:" + sha256_text(identity.dump());
    if (!identity.is_object() || identity_sha256 != expected_identity_sha256) {
        throw std::runtime_error("timing profile identity digest is missing or invalid");
    }
    const std::string model_id = identity.value("model", json::object()).value("model_id", "");
    const std::string model_fingerprint =
        identity.value("model", json::object()).value("model_fingerprint", "");
    const std::string qualified_engine_provenance_sha256 =
        identity.value("engine", json::object()).value("executable_sha256", "");
    const std::string provider =
        identity.value("search", json::object()).value("provider_effective", "");
    if (!valid_identifier(model_id) || model_fingerprint.empty() ||
        qualified_engine_provenance_sha256.size() != 71 ||
        !qualified_engine_provenance_sha256.starts_with("sha256:") || provider.empty()) {
        throw std::runtime_error(
            "timing profile provenance, model, or provider identity is incomplete");
    }
    return {
        {"validation", "qualified_uci_time_profile_v3"},
        {"sha256", sha256_file(path)},
        {"identity_sha256", identity_sha256},
        {"model_id", model_id},
        {"model_fingerprint", model_fingerprint},
        {"qualified_engine_provenance_sha256", qualified_engine_provenance_sha256},
        {"provider", provider},
        {"permissions", permissions},
    };
}

json validate_qualified_profile_locator(const fs::path& path, const std::string_view schema,
                                        const std::string_view label) {
    if (!fs::is_regular_file(path)) {
        throw std::runtime_error(std::string(label) +
                                 " locator must reference a regular file");
    }
    std::error_code error;
    const auto size = fs::file_size(path, error);
    if (error || size == 0 || size > 16ULL * 1024ULL * 1024ULL) {
        throw std::runtime_error(std::string(label) +
                                 " must be non-empty and no larger than 16 MiB");
    }
    const json profile = read_json(path);
    if (profile.value("schema", "") != schema ||
        !profile.value("qualified", false)) {
        throw std::runtime_error(std::string(label) + " is not qualified");
    }
    return {
        {"validation", "qualified_" + std::string(schema)},
        {"sha256", sha256_file(path)},
    };
}

struct ManagedProfileFile {
    fs::path source;
    fs::path relative;
    std::uint64_t size{};
    std::string sha256;
};

std::vector<ManagedProfileFile> lichess_single_game_profile_files(const fs::path& profile_path) {
    const json profile = read_json(profile_path);
    if (profile.value("schema", "") != "uci_arena_single_game_profile_v1" ||
        !profile.value("qualified", false)) {
        throw std::runtime_error("Lichess single-game profile is not qualified");
    }
    const json evidence = profile.value("evidence", json::object());
    static constexpr std::array<std::string_view, 5> required_evidence{
        "cursor_correctness", "provider_parity", "deadline", "topology", "complete_game"};
    std::vector<ManagedProfileFile> files;
    const auto profile_size = static_cast<std::uint64_t>(
        fs::file_size(platform::filesystem_api_path(profile_path)));
    if (profile_size == 0 || profile_size > managed_profile_max_bytes) {
        throw std::runtime_error("Lichess single-game profile exceeds the managed closure limit");
    }
    files.push_back({profile_path, profile_path.filename(), profile_size,
                     sha256_file(profile_path)});
    std::map<std::string, std::string> relative_digests{
        {profile_path.filename().generic_string(), "sha256:" + files.front().sha256}};
    std::uint64_t total = profile_size;

    for (const auto name : required_evidence) {
        if (!evidence.contains(name) || !evidence.at(name).is_object()) {
            throw std::runtime_error("Lichess single-game profile evidence " +
                                     std::string(name) + " is missing");
        }
        const json& record = evidence.at(name);
        if (!record.value("passed", false) || !record.contains("artifact") ||
            !record.at("artifact").is_string() || !record.contains("artifact_sha256") ||
            !record.at("artifact_sha256").is_string()) {
            throw std::runtime_error("Lichess single-game profile evidence " +
                                     std::string(name) + " is incomplete");
        }
        const fs::path relative = fs::path(record.at("artifact").get<std::string>())
                                      .lexically_normal();
        if (!safe_relative_path(relative)) {
            throw std::runtime_error("Lichess single-game profile evidence " +
                                     std::string(name) + " has an unsafe relative path");
        }
        const std::string expected = record.at("artifact_sha256").get<std::string>();
        if (expected.size() != 71 || !expected.starts_with("sha256:")) {
            throw std::runtime_error("Lichess single-game profile evidence " +
                                     std::string(name) + " has an invalid digest");
        }
        const std::string relative_key = relative.generic_string();
        if (const auto existing = relative_digests.find(relative_key);
            existing != relative_digests.end()) {
            if (existing->second != expected) {
                throw std::runtime_error("Lichess single-game profile evidence " +
                                         std::string(name) +
                                         " conflicts with another digest for " +
                                         relative_key);
            }
            continue;
        }

        std::optional<fs::path> match;
        std::string last_observation = "candidate_not_found";
        fs::path root = profile_path.parent_path();
        for (int depth = 0; depth < 8 && !root.empty(); ++depth) {
            const fs::path candidate = normalized_absolute(root / relative);
            if (!within(root, candidate)) {
                last_observation = "candidate_escaped_root";
            } else {
                std::error_code file_error;
                const bool regular = fs::is_regular_file(
                    platform::filesystem_api_path(candidate), file_error);
                if (file_error || !regular) {
                    last_observation = file_error
                                           ? "candidate_inspection_failed:" +
                                                 file_error.message()
                                           : "candidate_not_regular";
                } else if (contains_symbolic_link(candidate)) {
                    last_observation = "candidate_contains_symbolic_link";
                } else {
                    const std::string observed = "sha256:" + sha256_file(candidate);
                    if (observed == expected) {
                        match = candidate;
                        break;
                    }
                    last_observation = "candidate_digest_mismatch:" + observed;
                }
            }
            const fs::path parent = root.parent_path();
            if (parent == root) {
                break;
            }
            root = parent;
        }
        if (!match.has_value()) {
            throw std::runtime_error("Lichess single-game profile evidence " +
                                     std::string(name) +
                                     " is missing or does not match its declared digest; " +
                                     last_observation);
        }
        const auto size = static_cast<std::uint64_t>(
            fs::file_size(platform::filesystem_api_path(*match)));
        if (files.size() >= managed_profile_max_files ||
            size > managed_profile_max_bytes - total) {
            throw std::runtime_error(
                "Lichess single-game profile evidence closure exceeds its managed limit");
        }
        files.push_back({*match, relative, size, expected.substr(7)});
        relative_digests.emplace(relative_key, expected);
        total += size;
    }
    return files;
}

json validate_import_locator(const std::string& kind, const fs::path& path) {
    if (!valid_discovery_kind(kind)) {
        throw std::runtime_error("unknown import kind " + kind);
    }
    if (kind == "restaurant") {
        throw std::runtime_error("Restaurant is detect-only and cannot be imported");
    }
    if (!fs::exists(path)) {
        throw std::runtime_error("import path does not exist");
    }
    require_no_symbolic_link(path);
    if (kind == "opening_book") {
        return validate_book_locator(path);
    }
    if (kind == "syzygy") {
        return validate_syzygy_locator(path);
    }
    if (kind == "uci_engine") {
        return validate_uci_locator(path);
    }
    if (kind == "book_forge") {
        return validate_provider_locator(path, kind);
    }
    if (kind == "evidence_service") {
        return validate_provider_locator(path, kind);
    }
    if (kind == "timing_profile") {
        return validate_timing_profile_locator(path);
    }
    if (kind == "lichess_single_game_profile") {
        const auto files = lichess_single_game_profile_files(path);
        std::uint64_t total = 0;
        for (const auto& file : files) {
            total += file.size;
        }
        return {
            {"validation", "qualified_uci_arena_single_game_profile_v1"},
            {"sha256", sha256_file(path)},
            {"evidence_files", files.size() - 1},
            {"closure_bytes", total},
        };
    }
    if (kind == "lichess_capacity_profile") {
        return validate_qualified_profile_locator(
            path, "uci_arena_lichess_capacity_profile_v2", "Lichess capacity profile");
    }
    if (kind == "lichess_transport_profile") {
        return validate_qualified_profile_locator(
            path, "uci_arena_lichess_transport_profile_v2", "Lichess transport profile");
    }
    if (kind == "lichess_token_path") {
        if (!fs::is_regular_file(path)) {
            throw std::runtime_error("Lichess token locator must reference a regular file");
        }
        return {{"validation", "path_reference_only"}};
    }
    throw std::runtime_error("unsupported import kind " + kind);
}

struct ManagedBookFile {
    fs::path source;
    fs::path relative;
    std::uint64_t size{};
    std::string sha256;
};

std::vector<ManagedBookFile> managed_book_files(const fs::path& source) {
    std::vector<ManagedBookFile> files;
    std::uint64_t total = 0;
    if (fs::is_regular_file(source)) {
        const auto size = static_cast<std::uint64_t>(fs::file_size(source));
        if (size > managed_book_max_bytes) {
            throw std::runtime_error("managed opening-book copy exceeds the 256 MiB limit");
        }
        files.push_back({source, source.filename(), size, sha256_file(source)});
        total = size;
    } else {
        for (const auto& entry : fs::directory_iterator(source)) {
            if (entry.is_symlink()) {
                throw std::runtime_error(
                    "managed opening-book copy cannot traverse a symbolic link");
            }
            if (!entry.is_regular_file()) {
                throw std::runtime_error("managed opening-book copy requires a flat directory");
            }
            if (files.size() >= managed_book_max_files) {
                throw std::runtime_error("managed opening-book copy exceeds the 64-file limit");
            }
            const auto size = static_cast<std::uint64_t>(entry.file_size());
            if (size > managed_book_max_bytes - total) {
                throw std::runtime_error("managed opening-book copy exceeds the 256 MiB limit");
            }
            files.push_back(
                {entry.path(), entry.path().filename(), size, sha256_file(entry.path())});
            total += size;
        }
    }
    std::ranges::sort(files, {},
                      [](const ManagedBookFile& file) { return file.relative.generic_string(); });
    for (const auto& file : files) {
        if (!safe_relative_path(file.relative)) {
            throw std::runtime_error("managed opening-book copy has an unsafe file name");
        }
    }
    if (files.empty()) {
        throw std::runtime_error("managed opening-book copy has no files");
    }
    return files;
}

json copy_managed_book(const SetupPaths& paths, const fs::path& source, const json& validation) {
    const auto files = managed_book_files(source);
    json identity_rows = json::array();
    for (const auto& file : files) {
        identity_rows.push_back({{"path", file.relative.generic_string()},
                                 {"size_bytes", file.size},
                                 {"sha256", file.sha256}});
    }
    const std::string fingerprint = sha256_text(identity_rows.dump());
    const std::string component_id = "opening_book.import." + fingerprint.substr(0, 16);
    const std::string component_version = "1";
    const fs::path import_root = paths.knowledge_root / "imports" / "opening_books";
    const fs::path active_root = import_root / fingerprint;
    const fs::path staging_root = import_root / (".staging-" + fingerprint);
    const bool active_root_existed = fs::exists(active_root);
    const json discovery = json::array(
        {{{"kind", "opening_book"}, {"locator", {{"source", "active_root"}}}}});
    fs::create_directories(import_root);
    require_no_symbolic_link(import_root);

    std::error_code ignored;
    fs::remove_all(staging_root, ignored);
    if (!fs::exists(active_root)) {
        fs::create_directories(staging_root);
        try {
            for (const auto& file : files) {
                const fs::path target = staging_root / file.relative;
                if (!within(staging_root, target)) {
                    throw std::runtime_error("managed opening-book path escapes staging");
                }
                fs::copy_file(file.source, target, fs::copy_options::overwrite_existing);
                if (fs::file_size(target) != file.size || sha256_file(target) != file.sha256) {
                    throw std::runtime_error("managed opening-book copy verification failed");
                }
            }
            const json manifest{
                {"schema", "arena_install_component_v1"},
                {"schema_version", 1},
                {"component_id", component_id},
                {"component_version", component_version},
                {"discovery", discovery},
                {"files", identity_rows},
            };
            write_json_atomic(staging_root / "arena-component.json", manifest);
            fs::rename(staging_root, active_root);
        } catch (...) {
            fs::remove_all(staging_root, ignored);
            throw;
        }
    } else {
        require_no_symbolic_link(active_root);
        json manifest =
            validate_component_payload(active_root, component_id, component_version).manifest;
        if (manifest.contains("discovery") && manifest.at("discovery") != discovery) {
            throw std::runtime_error(
                "managed opening-book discovery declaration does not match its content role");
        }
        if (!manifest.contains("discovery")) {
            manifest["discovery"] = discovery;
            write_json_atomic(active_root / "arena-component.json", manifest);
        }
    }

    json activated_files = json::array();
    json ownership = json::array();
    for (const auto& file : files) {
        const fs::path path = active_root / file.relative;
        activated_files.push_back({{"path", path.string()},
                                   {"size_bytes", file.size},
                                   {"sha256", file.sha256}});
        ownership.push_back({
            {"path", path.string()},
            {"ownership_class", "generated"},
            {"normal_uninstall", "preserve"},
            {"purge", "exact_identity"},
            {"kind", "file"},
            {"size_bytes", file.size},
            {"sha256", file.sha256},
            {"source", "managed_copy"},
        });
    }
    const fs::path manifest_path = active_root / "arena-component.json";
    ownership.push_back({
        {"path", manifest_path.string()},
        {"ownership_class", "generated"},
        {"normal_uninstall", "preserve"},
        {"purge", "exact_identity"},
        {"kind", "file"},
        {"size_bytes", fs::file_size(manifest_path)},
        {"sha256", sha256_file(manifest_path)},
        {"source", "managed_copy_manifest"},
    });
    ownership.push_back({
        {"path", active_root.string()},
        {"ownership_class", "generated"},
        {"normal_uninstall", "preserve"},
        {"purge", "empty_only"},
        {"kind", "directory_tree"},
        {"source", "managed_copy_root"},
    });
    const json receipt{
        {"schema", "arena_install_receipt_v1"}, {"schema_version", 1},
        {"component_id", component_id},         {"component_version", component_version},
        {"state", "installed_managed"},         {"active_root", active_root.string()},
        {"locator_path", active_root.string()}, {"artifact_sha256", fingerprint},
        {"import_mode", "managed_copy"},        {"activated_files", activated_files},
        {"activated_resources",
         json::array({{{"kind", "opening_book"},
                       {"path", normalized_absolute(active_root).string()},
                       {"activated_files", activated_files}}})},
        {"ownership", std::move(ownership)},
        {"installed_at", current_utc_iso()},
    };
    const fs::path receipt_path = paths.data_root / "receipts" / (component_id + ".json");
    const bool receipt_existed = fs::is_regular_file(receipt_path);
    write_json_atomic(receipt_path, receipt);
    return {
        {"schema", "arena_setup_import_result_v1"},
        {"schema_version", 1},
        {"kind", "opening_book"},
        {"mode", "managed_copy"},
        {"state", "installed_managed"},
        {"path", active_root.string()},
        {"validation", validation},
        {"receipt", receipt},
        {"_active_root_existed", active_root_existed},
        {"_receipt_existed", receipt_existed},
    };
}

fs::path installed_entrypoint(const fs::path& receipts_root, const std::string& component_id,
                              const std::string& entrypoint_name) {
    const json receipt = read_json(receipts_root / (component_id + ".json"));
    const std::string receipt_state = receipt.value("state", "");
    if (receipt.value("schema", "") != "arena_install_receipt_v1" ||
        receipt.value("component_id", "") != component_id ||
        (receipt_state != "installed_managed" &&
         receipt_state != "installed_disabled")) {
        throw std::runtime_error("required installed dependency is unavailable: " + component_id);
    }
    const fs::path active_root = receipt.at("active_root").get<std::string>();
    const json manifest = read_json(active_root / "arena-component.json");
    if (manifest.value("schema", "") != "arena_install_component_v1" ||
        manifest.value("component_id", "") != component_id || !manifest.contains("entrypoints") ||
        !manifest.at("entrypoints").is_object()) {
        throw std::runtime_error("required installed dependency entrypoint is unavailable: " +
                                 component_id + ":" + entrypoint_name);
    }
    const json& entrypoints = manifest.at("entrypoints");
    std::string resolved_name = entrypoint_name;
    const bool exact_legacy_managed_alias =
        (component_id == "node_runtime.private" && entrypoint_name == "node") ||
        (component_id == "stockfish.engine" && entrypoint_name == "stockfish");
    if (!entrypoints.contains(resolved_name) && exact_legacy_managed_alias &&
        entrypoints.contains("executable")) {
        resolved_name = "executable";
    }
    if (!entrypoints.contains(resolved_name) || !entrypoints.at(resolved_name).is_string()) {
        throw std::runtime_error("required installed dependency entrypoint is unavailable: " +
                                 component_id + ":" + entrypoint_name);
    }
    const fs::path relative = entrypoints.at(resolved_name).get<std::string>();
    if (!safe_relative_path(relative)) {
        throw std::runtime_error("installed dependency entrypoint is unsafe");
    }
    const fs::path entrypoint = active_root / relative;
    for (const auto& file : manifest.at("files")) {
        if (file.value("path", "") == relative.generic_string() &&
            fs::is_regular_file(entrypoint) && !fs::is_symlink(entrypoint) &&
            fs::file_size(entrypoint) == integer_field(file, "size_bytes") &&
            sha256_file(entrypoint) == file.at("sha256").get<std::string>()) {
            return normalized_absolute(entrypoint);
        }
    }
    throw std::runtime_error("installed dependency entrypoint digest mismatch: " + component_id +
                             ":" + entrypoint_name);
}

fs::path installed_active_root(const fs::path& receipts_root, const std::string& component_id) {
    const json receipt = read_json(receipts_root / (component_id + ".json"));
    if (receipt.value("schema", "") != "arena_install_receipt_v1" ||
        receipt.value("component_id", "") != component_id ||
        receipt.value("state", "") != "installed_managed" || !receipt.contains("active_root") ||
        !receipt.at("active_root").is_string()) {
        throw std::runtime_error("required installed dependency is unavailable: " + component_id);
    }
    const fs::path root = normalized_absolute(receipt.at("active_root").get<std::string>());
    if (!fs::is_directory(root) || contains_symbolic_link(root)) {
        throw std::runtime_error("required installed dependency root is unavailable: " +
                                 component_id);
    }
    return root;
}

std::optional<std::string> saved_locator_authority_mode(const json& locator,
                                                        const std::string& kind) {
    if (!locator.contains("authority_mode")) {
        return kind == "opening_book"
            ? std::optional<std::string>{"immutable_pinned_snapshot"}
            : std::nullopt;
    }
    if (kind != "opening_book" || !locator.at("authority_mode").is_string()) {
        throw std::runtime_error("saved locator authority mode is not allowed for " + kind);
    }
    const std::string authority = locator.at("authority_mode").get<std::string>();
    if (authority != "service_managed_live_channel" &&
        authority != "immutable_pinned_snapshot") {
        throw std::runtime_error("saved opening_book locator authority mode is invalid");
    }
    return authority;
}

struct ProviderLocatorSelection {
    fs::path path;
    std::string source;
    std::string storage_mode;
    std::optional<std::string> authority_mode;
};

std::optional<ProviderLocatorSelection>
saved_locator_path(const fs::path& data_root, const std::string& kind) {
    const fs::path locator_path = data_root / "locators" / (kind + ".json");
    if (!fs::is_regular_file(locator_path) || contains_symbolic_link(locator_path)) {
        return std::nullopt;
    }
    const json locator = read_json(locator_path);
    const std::string mode = locator.value("mode", "");
    if (locator.value("schema", "") != "arena_saved_locator_v1" ||
        locator.value("schema_version", 0) != 1 || locator.value("kind", "") != kind ||
        (mode != "in_place_reference" &&
         !((kind == "opening_book" || kind == "lichess_single_game_profile") &&
           mode == "managed_copy")) ||
        !locator.contains("path") ||
        !locator.at("path").is_string()) {
        throw std::runtime_error("saved " + kind + " locator is invalid");
    }
    const fs::path path = normalized_absolute(locator.at("path").get<std::string>());
    static_cast<void>(validate_import_locator(kind, path));
    return ProviderLocatorSelection{
        path,
        "saved_locator",
        mode,
        saved_locator_authority_mode(locator, kind),
    };
}

std::optional<ProviderLocatorSelection> provider_locator_path(
    const SetupPaths& paths, const std::string& kind, const json& binding,
    std::optional<json>& discovery_cache) {
    const auto accepts = [&binding](const std::string_view source) {
        if (!binding.contains("accepted_sources")) {
            return true;
        }
        return std::ranges::any_of(
            binding.at("accepted_sources"), [source](const json& candidate) {
                return candidate.get<std::string>() == source;
            });
    };

    if (accepts("saved_locator")) {
        try {
            if (const auto saved = saved_locator_path(paths.data_root, kind);
                saved.has_value()) {
                return saved;
            }
        } catch (const std::exception&) {
            // Retained locator evidence is not authority. A broken optional
            // selection may still use a verified receipt when the provider
            // explicitly accepts that source.
        }
    }
    if (!accepts("install_receipt")) {
        return std::nullopt;
    }

    if (!discovery_cache.has_value()) {
        discovery_cache = discovery_rows(paths, {}, nullptr);
    }
    const json* selected = nullptr;
    for (const auto& candidate : discovery_cache->at(kind).at("candidates")) {
        if (candidate.value("source", "") != "install_receipt" ||
            candidate.value("state", "") != "installed_managed" ||
            !candidate.contains("path") || !candidate.at("path").is_string()) {
            continue;
        }
        if (selected == nullptr ||
            discovery_quality_score(candidate) > discovery_quality_score(*selected) ||
            (discovery_quality_score(candidate) == discovery_quality_score(*selected) &&
             candidate.at("path").get<std::string>() <
                 selected->at("path").get<std::string>())) {
            selected = &candidate;
        }
    }
    if (selected == nullptr) {
        return std::nullopt;
    }
    const fs::path path =
        normalized_absolute(selected->at("path").get<std::string>());
    static_cast<void>(validate_import_locator(kind, path));
    return ProviderLocatorSelection{
        path,
        "install_receipt",
        "installed_component",
        kind == "opening_book"
            ? std::optional<std::string>{"immutable_pinned_snapshot"}
            : std::nullopt,
    };
}

struct ProviderInstallResult {
    json configuration;
    json generated_documents;
};

ProviderInstallResult render_provider_installation(
    const SetupPaths& paths, const json& manifest, const fs::path& component_root,
    const fs::path& workspace, const bool enabled) {
    const json& integration = manifest.at("installer_integration");
    const fs::path receipts_root = paths.data_root / "receipts";
    json bindings = json::object();
    for (const auto& binding : integration.at("dependency_bindings")) {
        const std::string name = binding.at("name").get<std::string>();
        const std::string component_id = binding.at("component_id").get<std::string>();
        const bool required = binding.at("required").get<bool>();
        try {
            const std::string source = binding.at("source").get<std::string>();
            if (source == "entrypoint") {
                bindings[name] = installed_entrypoint(
                    receipts_root, component_id,
                    binding.at("entrypoint").get<std::string>())
                                     .string();
            } else {
                const fs::path active_root =
                    installed_active_root(receipts_root, component_id);
                if (source == "active_root") {
                    bindings[name] = active_root.string();
                } else {
                    const fs::path relative = fs::path(
                        binding.at("path").get<std::string>()).lexically_normal();
                    const fs::path selected = normalized_absolute(active_root / relative);
                    if (!within(active_root, selected) || !fs::exists(selected) ||
                        contains_symbolic_link(selected)) {
                        throw std::runtime_error(
                            "installed component-path binding is unavailable: " + name);
                    }
                    bindings[name] = selected.string();
                }
            }
        } catch (const std::exception&) {
            if (required) {
                throw;
            }
        }
    }

    json locators = json::object();
    json locator_details = json::object();
    std::optional<json> discovery_cache;
    for (const auto& binding : integration.at("locator_bindings")) {
        const std::string name = binding.at("name").get<std::string>();
        const std::string kind = binding.at("kind").get<std::string>();
        try {
            const auto selected =
                provider_locator_path(paths, kind, binding, discovery_cache);
            if (selected.has_value()) {
                locators[name] = selected->path.string();
                json details{
                    {"kind", kind},
                    {"path", selected->path.string()},
                    {"source", selected->source},
                    {"storage_mode", selected->storage_mode},
                };
                if (selected->authority_mode.has_value()) {
                    details["authority_mode"] = *selected->authority_mode;
                }
                locator_details[name] = std::move(details);
            } else if (binding.at("required").get<bool>()) {
                throw std::runtime_error("required saved locator is unavailable: " + kind);
            }
        } catch (const std::exception&) {
            if (binding.at("required").get<bool>()) {
                throw;
            }
        }
    }
    const std::string component_id = manifest.at("component_id").get<std::string>();
    const std::string component_version = manifest.at("component_version").get<std::string>();
    const json context{
        {"schema", "arena_provider_install_context_v1"},
        {"schema_version", 1},
        {"component",
         {{"id", component_id},
          {"version", component_version},
          {"root", component_root.string()}}},
        {"workspace", workspace.string()},
        {"enabled", enabled},
        {"bindings", std::move(bindings)},
        {"locators", std::move(locators)},
        {"locator_details", std::move(locator_details)},
    };
    const fs::path context_root = paths.cache_root / "provider-context";
    fs::create_directories(context_root);
    if (!within(paths.cache_root, context_root) || contains_symbolic_link(context_root)) {
        throw std::runtime_error(
            "provider installer context root contains a symbolic-link boundary");
    }
    const auto nonce = std::chrono::system_clock::now().time_since_epoch().count();
    const fs::path context_path =
        context_root / (component_id + ".installer-context-" +
                        std::to_string(nonce) + ".json");
    if (!within(context_root, context_path) || fs::exists(context_path)) {
        throw std::runtime_error("provider installer context path is unsafe or occupied");
    }
    write_json_atomic(context_path, context);
    json response;
    try {
        const json& invocation = integration.at("invocation");
        const fs::path runtime = installed_entrypoint(
            receipts_root, invocation.at("runtime_component_id").get<std::string>(),
            invocation.at("runtime_entrypoint").get<std::string>());
        const std::string entrypoint_name =
            integration.at("entrypoint").get<std::string>();
        const fs::path renderer = component_root /
            manifest.at("entrypoints").at(entrypoint_name).get<std::string>();
        arena::application::ProcessSupervisor process;
        response = process.run_json(
            runtime, component_root,
            {renderer.string(), "--context", context_path.string()},
            std::chrono::seconds(15));
    } catch (...) {
        std::error_code ignored;
        fs::remove(context_path, ignored);
        throw;
    }
    std::error_code ignored;
    fs::remove(context_path, ignored);
    if (!response.is_object() || response.size() != 4 ||
        response.value("schema", "") != "arena_provider_install_result_v1" ||
        response.value("schema_version", 0) != 1 ||
        !response.contains("configuration") ||
        !response.at("configuration").is_object() ||
        !response.contains("generated_documents") ||
        !response.at("generated_documents").is_array() ||
        response.at("generated_documents").size() > 16) {
        throw std::runtime_error(
            "provider installer-integration result is incompatible: " + component_id);
    }
    std::set<fs::path> generated_paths;
    for (const auto& generated : response.at("generated_documents")) {
        if (!generated.is_object() || generated.size() != 2 ||
            !generated.contains("path") || !generated.at("path").is_string() ||
            !generated.contains("document") || !generated.at("document").is_object()) {
            throw std::runtime_error(
                "provider installer generated-document contract is invalid");
        }
        const fs::path relative =
            fs::path(generated.at("path").get<std::string>()).lexically_normal();
        if (!safe_relative_path(relative) ||
            !generated_paths.insert(relative).second ||
            !within(workspace, workspace / relative)) {
            throw std::runtime_error(
                "provider installer generated-document path is unsafe");
        }
    }
    return {response.at("configuration"), response.at("generated_documents")};
}

struct ImportWrite {
    std::string component_id;
    fs::path path;
    json document;
};

std::set<std::string> provider_dependency_component_ids(
    const json& receipt, const json& manifest) {
    std::set<std::string> component_ids;
    for (const auto& dependency : receipt.value("dependencies", json::array())) {
        if (dependency.is_string() &&
            valid_identifier(dependency.get_ref<const std::string&>())) {
            component_ids.insert(dependency.get<std::string>());
        }
    }
    if (!manifest.contains("installer_integration")) {
        return component_ids;
    }
    for (const auto& binding :
         manifest.at("installer_integration").at("dependency_bindings")) {
        component_ids.insert(binding.at("component_id").get<std::string>());
    }
    return component_ids;
}

bool provider_depends_on_any_component(
    const json& receipt, const json& manifest,
    const std::set<std::string>& component_ids) {
    const auto dependencies =
        provider_dependency_component_ids(receipt, manifest);
    return std::ranges::any_of(
        dependencies, [&component_ids](const std::string& dependency) {
            return component_ids.contains(dependency);
        });
}

json refresh_provider_consumers(
    const SetupPaths& paths,
    const std::function<bool(const json&, const json&)>& matches,
    const std::string& retained_transaction_id = {},
    json* retained_writes = nullptr,
    const std::function<void()>& checkpoint = {}) {
    const fs::path receipts_root = paths.data_root / "receipts";
    std::vector<ImportWrite> writes;
    std::set<std::string> refreshed_components;
    if (!fs::is_directory(receipts_root)) {
        return json::array();
    }
    std::vector<fs::path> receipt_paths;
    for (const auto& entry : fs::directory_iterator(receipts_root)) {
        if (entry.is_regular_file() && !entry.is_symlink() &&
            entry.path().extension() == ".json" &&
            entry.path().filename().string().find(".rollback-") == std::string::npos) {
            receipt_paths.push_back(entry.path());
        }
    }
    if (receipt_paths.size() > 256) {
        throw std::runtime_error("installed receipt inventory exceeds its bounded limit");
    }
    std::ranges::sort(receipt_paths);
    for (const auto& receipt_path : receipt_paths) {
        json receipt;
        try {
            receipt = read_json(receipt_path);
        } catch (const std::exception&) {
            continue;
        }
        const std::string component_id = receipt.value("component_id", "");
        const std::string receipt_state = receipt.value("state", "");
        if (receipt.value("schema", "") != "arena_install_receipt_v1" ||
            !valid_identifier(component_id) ||
            (receipt_state != "installed_managed" &&
             receipt_state != "installed_disabled") ||
            !receipt.contains("active_root") ||
            !receipt.at("active_root").is_string() ||
            !receipt.contains("component_version") ||
            !receipt.at("component_version").is_string()) {
            continue;
        }
        const fs::path component_root =
            normalized_absolute(receipt.at("active_root").get<std::string>());
        ValidatedComponentPayload validated;
        try {
            validated = validate_component_payload(
                component_root, component_id,
                receipt.at("component_version").get<std::string>());
        } catch (const std::exception&) {
            continue;
        }
        if (!validated.manifest.contains("installer_integration")) {
            continue;
        }
        const json& integration = validated.manifest.at("installer_integration");
        if (!matches(receipt, validated.manifest)) continue;
        const std::string configured = receipt.value("configuration_path", "");
        if (configured.empty()) {
            throw std::runtime_error("installed consumer has no generated configuration: " +
                                     component_id);
        }
        const fs::path configuration_path = normalized_absolute(configured);
        const fs::path workspace = configuration_path.parent_path();
        const fs::path declared_workspace = provider_workspace(paths, integration);
        if (workspace != declared_workspace ||
            !within(declared_workspace, configuration_path)) {
            throw std::runtime_error(
                "installed consumer configuration escapes its declared workspace");
        }
        arena::platform::grant_manager_workspace_access(
            workspace, paths.manager_service_access);
        const ProviderInstallResult generated = render_provider_installation(
            paths, validated.manifest, component_root, workspace,
            receipt_state == "installed_managed");
        for (const auto& document : generated.generated_documents) {
            const fs::path document_path = normalized_absolute(
                workspace /
                fs::path(document.at("path").get<std::string>()).lexically_normal());
            if (!within(workspace, document_path)) {
                throw std::runtime_error(
                    "generated provider document escapes its workspace");
            }
            writes.push_back(
                {component_id, document_path, document.at("document")});
        }
        writes.push_back({component_id, configuration_path, generated.configuration});
        refreshed_components.insert(component_id);
    }

    struct AppliedWrite {
        fs::path path;
        fs::path backup;
        bool existed;
        std::optional<std::size_t> retained_index;
    };
    std::vector<AppliedWrite> applied;
    const std::string transaction_id = retained_transaction_id.empty()
        ? std::to_string(std::chrono::system_clock::now().time_since_epoch().count())
        : retained_transaction_id;
    try {
        for (const auto& write : writes) {
            if (contains_symbolic_link(write.path.parent_path()) ||
                (fs::exists(write.path) && fs::is_symlink(write.path))) {
                throw std::runtime_error(
                    "generated import consumer path contains a symbolic-link boundary");
            }
            fs::create_directories(write.path.parent_path());
            const bool existed = fs::is_regular_file(write.path);
            const fs::path backup = write.path.string() + ".import-rollback-" + transaction_id;
            std::error_code ignored;
            fs::remove(backup, ignored);
            if (existed) {
                fs::copy_file(write.path, backup, fs::copy_options::overwrite_existing);
            }
            std::optional<std::size_t> retained_index;
            if (retained_writes != nullptr) {
                retained_index = retained_writes->size();
                retained_writes->push_back({
                    {"path", write.path.string()},
                    {"backup", backup.string()},
                    {"existed", existed},
                    {"state", "prepared"},
                });
                if (checkpoint) checkpoint();
            }
            applied.push_back({write.path, backup, existed, retained_index});
            write_json_atomic(write.path, write.document);
            if (read_json(write.path) != write.document) {
                throw std::runtime_error(
                    "generated import consumer configuration verification failed");
            }
            if (retained_index.has_value()) {
                (*retained_writes)[*retained_index]["state"] = "written";
                if (checkpoint) checkpoint();
            }
        }
    } catch (...) {
        std::error_code ignored;
        for (auto current = applied.rbegin(); current != applied.rend(); ++current) {
            fs::remove(current->path, ignored);
            ignored.clear();
            if (current->existed && fs::is_regular_file(current->backup)) {
                fs::rename(current->backup, current->path, ignored);
                ignored.clear();
            } else {
                fs::remove(current->backup, ignored);
                ignored.clear();
            }
            if (current->retained_index.has_value()) {
                (*retained_writes)[*current->retained_index]["state"] = "restored";
            }
        }
        if (checkpoint) checkpoint();
        throw;
    }

    if (retained_writes == nullptr) {
        std::error_code ignored;
        for (const auto& write : applied) {
            fs::remove(write.backup, ignored);
            ignored.clear();
        }
    }
    json refreshed = json::array();
    for (const auto& component_id : refreshed_components) {
        refreshed.push_back(component_id);
    }
    return refreshed;
}

json refresh_import_consumers(
    const SetupPaths& paths, const std::string& kind,
    const std::string& retained_transaction_id = {},
    json* retained_writes = nullptr,
    const std::function<void()>& checkpoint = {}) {
    return refresh_provider_consumers(
        paths,
        [&kind](const json&, const json& manifest) {
            const json& integration = manifest.at("installer_integration");
            return std::ranges::any_of(
                integration.at("locator_bindings"), [&kind](const json& binding) {
                    return binding.value("kind", "") == kind;
                });
        },
        retained_transaction_id, retained_writes, checkpoint);
}

json refresh_dependency_consumers(
    const SetupPaths& paths,
    const std::set<std::string>& replaced_component_ids,
    const std::set<std::string>& already_refreshed_component_ids,
    const std::string& retained_transaction_id,
    json* retained_writes,
    const std::function<void()>& checkpoint) {
    return refresh_provider_consumers(
        paths,
        [&replaced_component_ids,
         &already_refreshed_component_ids](const json& receipt,
                                           const json& manifest) {
            const std::string component_id = receipt.at("component_id").get<std::string>();
            if (replaced_component_ids.contains(component_id) ||
                already_refreshed_component_ids.contains(component_id)) {
                return false;
            }
            return provider_depends_on_any_component(
                receipt, manifest, replaced_component_ids);
        },
        retained_transaction_id, retained_writes, checkpoint);
}

json save_locator_and_refresh(const SetupPaths& paths, const std::string& kind,
                              const fs::path& normalized, const json& validation,
                              const std::string_view mode = "in_place_reference",
                              const std::string_view authority_mode = {}) {
    if (mode != "in_place_reference" &&
        !((kind == "opening_book" || kind == "lichess_single_game_profile") &&
          mode == "managed_copy")) {
        throw std::runtime_error("saved locator mode is not allowed for " + kind);
    }
    if (kind != "opening_book" && !authority_mode.empty()) {
        throw std::runtime_error("saved locator authority mode is not allowed for " + kind);
    }
    const std::string resolved_authority = kind == "opening_book"
        ? std::string(authority_mode.empty() ? "immutable_pinned_snapshot" : authority_mode)
        : std::string{};
    if (kind == "opening_book" &&
        resolved_authority != "service_managed_live_channel" &&
        resolved_authority != "immutable_pinned_snapshot") {
        throw std::runtime_error("saved opening_book locator authority mode is invalid");
    }
    if (resolved_authority == "service_managed_live_channel" &&
        mode != "in_place_reference") {
        throw std::runtime_error(
            "service-managed opening-book authority requires an in-place reference");
    }
    const fs::path locator_path = paths.data_root / "locators" / (kind + ".json");
    fs::create_directories(locator_path.parent_path());
    const bool locator_existed = fs::is_regular_file(locator_path);
    const fs::path backup = locator_path.string() + ".import-rollback";
    std::error_code ignored;
    fs::remove(backup, ignored);
    if (locator_existed) {
        fs::copy_file(locator_path, backup, fs::copy_options::overwrite_existing);
    }
    json locator{
        {"schema", "arena_saved_locator_v1"},
        {"schema_version", 1},
        {"kind", kind},
        {"mode", mode},
        {"path", normalized.string()},
        {"validation", validation},
        {"saved_at", current_utc_iso()},
    };
    if (!resolved_authority.empty()) {
        locator["authority_mode"] = resolved_authority;
    }
    try {
        write_json_atomic(locator_path, locator);
        const json refreshed = refresh_import_consumers(paths, kind);
        if (read_json(locator_path) != locator) {
            throw std::runtime_error("saved locator verification failed");
        }
        fs::remove(backup, ignored);
        return refreshed;
    } catch (...) {
        fs::remove(locator_path, ignored);
        ignored.clear();
        if (locator_existed && fs::is_regular_file(backup)) {
            fs::rename(backup, locator_path, ignored);
        } else {
            fs::remove(backup, ignored);
        }
        throw;
    }
}

void rename_tree_with_retry(const fs::path& source, const fs::path& destination,
                            const std::string_view failure_prefix) {
    constexpr int attempts = 100;
    constexpr auto retry_delay = std::chrono::milliseconds(100);
    std::string failure = "rename did not complete";
    for (int attempt = 0; attempt < attempts; ++attempt) {
        std::error_code rename_error;
        fs::rename(source, destination, rename_error);
        if (!rename_error) {
            return;
        }
        failure = rename_error.message();
        if (attempt + 1 < attempts) {
            std::this_thread::sleep_for(retry_delay);
        }
    }
    throw std::runtime_error(std::string(failure_prefix) + ": " + failure);
}

void migrate_provider_workspace(
    const SetupPaths& paths, const json& previous_receipt,
    const fs::path& destination, const std::string& transaction_id,
    json& migration, const std::function<void()>& checkpoint) {
    const json data_paths = previous_receipt.value("data_paths", json::array());
    const std::string configured = previous_receipt.value("configuration_path", "");
    if (!data_paths.is_array() || data_paths.size() != 1 ||
        !data_paths.at(0).is_string() || configured.empty()) {
        throw std::runtime_error(
            "provider workspace migration requires one exact receipt-owned data path");
    }
    const fs::path source =
        normalized_absolute(data_paths.at(0).get<std::string>());
    const fs::path configuration = normalized_absolute(configured);
    if (source == destination) {
        migration["state"] = "not_required";
        checkpoint();
        return;
    }
    if (configuration.parent_path() != source || !fs::is_directory(source) ||
        contains_symbolic_link(source)) {
        throw std::runtime_error(
            "provider workspace migration source does not match its validated receipt");
    }
    for (const auto& entry : fs::recursive_directory_iterator(source)) {
        if (entry.is_symlink()) {
            throw std::runtime_error(
                "provider workspace migration rejects symbolic-link content");
        }
    }
    if (fs::exists(destination)) {
        if (!fs::is_directory(destination) || !fs::is_empty(destination)) {
            throw std::runtime_error(
                "provider workspace migration destination is already occupied");
        }
        fs::remove(destination);
    }
    fs::create_directories(destination.parent_path());
    migration = {
        {"source", source.string()},
        {"destination", destination.string()},
        {"state", "prepared"},
        {"locator_writes", json::array()},
    };
    checkpoint();
    rename_tree_with_retry(
        source, destination,
        "provider workspace migration requires an atomic same-volume move");
    migration["state"] = "moved";
    checkpoint();

    const fs::path locators_root = paths.data_root / "locators";
    if (!fs::is_directory(locators_root)) return;
    std::vector<fs::path> locator_paths;
    for (const auto& entry : fs::directory_iterator(locators_root)) {
        if (entry.is_regular_file() && !entry.is_symlink() &&
            entry.path().extension() == ".json" &&
            entry.path().filename().string().find(".rollback-") == std::string::npos) {
            locator_paths.push_back(entry.path());
        }
    }
    if (locator_paths.size() > 64) {
        throw std::runtime_error("saved locator inventory exceeds its bounded limit");
    }
    std::ranges::sort(locator_paths);
    for (const fs::path& locator_path : locator_paths) {
        json locator = read_json(locator_path);
        const std::string kind = locator.value("kind", "");
        const std::string selected = locator.value("path", "");
        if (locator.value("schema", "") != "arena_saved_locator_v1" ||
            locator.value("schema_version", 0) != 1 || !valid_identifier(kind) ||
            selected.empty()) {
            continue;
        }
        const fs::path selected_path = normalized_absolute(selected);
        if (!within(source, selected_path)) continue;
        const fs::path relative = selected_path.lexically_relative(source);
        const fs::path rebased = normalized_absolute(destination / relative);
        if (!within(destination, rebased) || contains_symbolic_link(rebased)) {
            throw std::runtime_error(
                "rebased provider locator escapes its migrated workspace");
        }
        const fs::path backup = locator_path.string() +
            ".workspace-rollback-" + transaction_id;
        fs::copy_file(locator_path, backup, fs::copy_options::overwrite_existing);
        const std::size_t row = migration["locator_writes"].size();
        migration["locator_writes"].push_back({
            {"kind", kind},
            {"path", locator_path.string()},
            {"backup", backup.string()},
            {"previous_path", selected_path.string()},
            {"rebased_path", rebased.string()},
            {"state", "prepared"},
        });
        checkpoint();
        locator["path"] = rebased.string();
        locator["validation"] = validate_import_locator(kind, rebased);
        locator["saved_at"] = current_utc_iso();
        write_json_atomic(locator_path, locator);
        migration["locator_writes"][row]["state"] = "rebased";
        checkpoint();
    }
    migration["state"] = "rebased";
    checkpoint();
}

bool supports_startup_capability(const json& receipt, const std::string_view capability) {
    if (!receipt.contains("active_root") || !receipt.at("active_root").is_string()) {
        return false;
    }
    const fs::path manifest_path =
        fs::path(receipt.at("active_root").get<std::string>()) / "arena-component.json";
    if (!fs::is_regular_file(manifest_path) || fs::is_symlink(manifest_path)) {
        return false;
    }
    const json manifest = read_json(manifest_path);
    const auto capabilities = manifest.value("capabilities", json::array());
    return capabilities.is_array() && std::find(capabilities.begin(), capabilities.end(),
                                                std::string(capability)) != capabilities.end();
}

struct ProviderWorkspaceInvocation {
    fs::path workspace;
    fs::path control_workspace;
};

struct ReceiptLifecycleObservation {
    bool installed;
    bool active;
};

std::optional<ReceiptLifecycleObservation> receipt_lifecycle_observation(
    const json& receipt) {
    if (!receipt.contains("startup") || !receipt.at("startup").is_object()) {
        return std::nullopt;
    }
    const json& startup = receipt.at("startup");
    if (startup.value("schema", "") == "arena_provider_lifecycle_result_v1") {
        if (startup.value("schema_version", 0) != 1 ||
            startup.value("component_id", "") !=
                receipt.value("component_id", "") ||
            startup.value("component_version", "") !=
                receipt.value("component_version", "") ||
            !startup.contains("observed") || !startup.at("observed").is_object() ||
            !startup.contains("disposition") ||
            !startup.at("disposition").is_string() ||
            (startup.at("disposition") != "ready" &&
             startup.at("disposition") != "safe_inactive")) {
            return std::nullopt;
        }
        const json& observed = startup.at("observed");
        const std::string operation = startup.value("operation", "");
        const bool install = operation == "install";
        const bool post_replace = operation == "post_replace";
        const bool ready = startup.at("disposition") == "ready";
        if ((!install && !post_replace) ||
            observed.value("installation", "") != "installed" ||
            observed.value("activity", "") !=
                (ready ? "active" : "inactive") ||
            observed.value("configuration", "") != "matches" ||
            observed.value("replacement", "") !=
                (install ? "not_applicable" : "safe")) {
            return std::nullopt;
        }
        return ReceiptLifecycleObservation{true, ready};
    }
    if (!startup.contains("installed") ||
        !startup.at("installed").is_boolean()) {
        return std::nullopt;
    }
    const bool installed = startup.at("installed").get<bool>();
    if (startup.contains("active") && !startup.at("active").is_boolean()) {
        return std::nullopt;
    }
    return ReceiptLifecycleObservation{installed,
                                       startup.value("active", installed)};
}

ProviderWorkspaceInvocation validated_provider_workspace(
    const SetupPaths& paths, const json& receipt,
    const fs::path& workspace_relative_path,
    const bool trusted_existing_receipt_workspace) {
    const std::string component_id = receipt.at("component_id").get<std::string>();
    const auto data_paths = receipt.value("data_paths", json::array());
    if (data_paths.empty() || !data_paths.at(0).is_string()) {
        throw std::runtime_error("startup-capable component has no managed workspace: " +
                                 component_id);
    }
    const fs::path recorded_workspace = data_paths.at(0).get<std::string>();
    if (!recorded_workspace.is_absolute()) {
        throw std::runtime_error("startup-capable component workspace is not absolute: " +
                                 component_id);
    }
    const fs::path workspace = normalized_absolute(recorded_workspace);
    const auto lifecycle_observation = receipt_lifecycle_observation(receipt);
    const bool trusted_receipt_workspace = trusted_existing_receipt_workspace &&
                                           lifecycle_observation.has_value() &&
                                           lifecycle_observation->installed;
    const json manifest =
        validate_component_payload(
            receipt.at("active_root").get<std::string>(), component_id,
            receipt.at("component_version").get<std::string>())
            .manifest;
    const fs::path declared_workspace =
        provider_workspace(paths, manifest.at("installer_integration"));
    if (workspace != declared_workspace && !trusted_receipt_workspace) {
        throw std::runtime_error(
            "startup-capable component workspace differs from its declared placement");
    }
    const fs::path control_workspace = workspace_relative_path == fs::path(".")
                                           ? workspace
                                           : normalized_absolute(
                                                 workspace / workspace_relative_path);
    if (!at_or_within(workspace, control_workspace)) {
        throw std::runtime_error(
            "startup control workspace escapes its provider workspace");
    }
    if (contains_symbolic_link(control_workspace)) {
        throw std::runtime_error(
            "startup control workspace contains a symbolic-link boundary");
    }
    return {workspace, control_workspace};
}

ProviderLifecycleResult invoke_provider_lifecycle(
    const SetupPaths& paths, const json& receipt,
    const ProviderLifecycleOperation operation,
    const bool trusted_existing_receipt_workspace = false,
    const fs::path& staged_control_root = {},
    const std::string& staged_control_version = {},
    const fs::path& previous_root = {},
    const fs::path& replacement_root = {}) {
    const std::string component_id = receipt.at("component_id").get<std::string>();
    const bool staged_control = !staged_control_root.empty();
    const fs::path active_root = staged_control
                                     ? staged_control_root
                                     : fs::path(receipt.at("active_root").get<std::string>());
    const std::string control_version = staged_control
                                            ? staged_control_version
                                            : receipt.at("component_version").get<std::string>();
    const json manifest =
        validate_component_payload(
            active_root, component_id, control_version)
            .manifest;
    if (!manifest.contains("installer_integration") ||
        !manifest.at("installer_integration").contains("startup_control")) {
        throw std::runtime_error(
            "startup-capable component has no Installer startup control contract: " +
            component_id);
    }
    const ProviderLifecycleControl control = [&]() {
        try {
            return parse_provider_lifecycle_control(
                manifest.at("installer_integration").at("startup_control"));
        } catch (const std::invalid_argument&) {
            throw std::runtime_error(
                "provider lifecycle control contract is invalid: " +
                component_id);
        }
    }();
    const auto operation_control = control.operations.find(operation);
    if (operation_control == control.operations.end()) {
        throw std::runtime_error("provider lifecycle operation is unsupported: " +
                                 component_id);
    }
    const fs::path receipts_root = paths.data_root / "receipts";
    const json& integration = manifest.at("installer_integration");
    const json& invocation = integration.at("invocation");
    const fs::path runtime = installed_entrypoint(
        receipts_root, invocation.at("runtime_component_id").get<std::string>(),
        invocation.at("runtime_entrypoint").get<std::string>());
    const std::string& control_entrypoint = control.entrypoint;
    const fs::path cli = staged_control
                             ? active_root /
                                   manifest.at("entrypoints")
                                       .at(control_entrypoint)
                                       .get<std::string>()
                             : installed_entrypoint(receipts_root, component_id,
                                                    control_entrypoint);
    std::vector<std::string> arguments{cli.string()};
    arguments.insert(arguments.end(), operation_control->second.arguments.begin(),
                     operation_control->second.arguments.end());
    const ProviderWorkspaceInvocation provider_workspace =
        validated_provider_workspace(paths, receipt, control.workspace_relative_path,
                                     trusted_existing_receipt_workspace);
    arguments.push_back(control.workspace_flag);
    arguments.push_back(provider_workspace.control_workspace.string());
    if (operation == ProviderLifecycleOperation::pre_replace ||
        operation == ProviderLifecycleOperation::post_replace) {
        if (!control.previous_root_flag || !control.replacement_root_flag ||
            previous_root.empty() || replacement_root.empty()) {
            throw std::runtime_error(
                "provider lifecycle replacement roots are unavailable: " +
                component_id);
        }
        arguments.push_back(*control.previous_root_flag);
        arguments.push_back(normalized_absolute(previous_root).string());
        arguments.push_back(*control.replacement_root_flag);
        arguments.push_back(normalized_absolute(replacement_root).string());
    }
    const std::string configuration_path = receipt.value("configuration_path", "");
    if (!configuration_path.empty()) {
        const fs::path configuration = normalized_absolute(configuration_path);
        const bool valid_configuration = within(provider_workspace.workspace, configuration) &&
                                         fs::is_regular_file(configuration) &&
                                         !fs::is_symlink(configuration);
        if (!valid_configuration &&
            operation == ProviderLifecycleOperation::install) {
            throw std::runtime_error("startup-capable component configuration is unavailable");
        }
        if (valid_configuration) {
            arguments.push_back(control.configuration_flag);
            arguments.push_back(configuration.string());
        }
    }
    arena::application::ProcessSupervisor process_supervisor;
    auto lifecycle =
        arena::adapters::make_cli_provider_lifecycle(process_supervisor);
    const arena::ports::ProviderLifecycleRequest request{
        component_id,
        control_version,
        operation,
        runtime,
        active_root,
        std::move(arguments),
        {control.wire_protocol,
         control.legacy_envelope,
         control.legacy_require_configuration_match,
         operation_control->second.allow_safe_inactive},
        arena::ports::provider_lifecycle_timeout_default};
    auto result = lifecycle->invoke(request);
    if (result.disposition == ProviderLifecycleDisposition::failed) {
        throw std::runtime_error("provider lifecycle operation failed: " +
                                 component_id);
    }
    return result;
}

void verify_provider_service_health(const SetupPaths& paths, const json& receipt,
                                    const ValidatedComponentPayload& validated) {
    if (!validated.service_contract.has_value()) {
        throw std::runtime_error(
            "installed startup authority has no public service health contract: " +
            receipt.at("component_id").get<std::string>());
    }
    const json& service_contract = *validated.service_contract;
    const std::string service_contract_schema =
        service_contract.value("schema", "");
    const bool provider_service =
        service_contract_schema == "uci_arena_service_contract_v1";
    if (!provider_service &&
        service_contract_schema != "uci_arena_standalone_service_contract_v1") {
        throw std::runtime_error(
            "installed startup authority has no compatible public service health contract: " +
            receipt.at("component_id").get<std::string>());
    }
    const json& manifest = validated.manifest;
    if (!manifest.contains("entrypoints") ||
        !manifest.at("entrypoints").is_object() ||
        !manifest.at("entrypoints").contains("cli") ||
        !manifest.at("entrypoints").at("cli").is_string()) {
        throw std::runtime_error("installed service health CLI is unavailable");
    }

    const std::string component_id = receipt.at("component_id").get<std::string>();
    const std::string component_version =
        receipt.at("component_version").get<std::string>();
    const fs::path active_root = receipt.at("active_root").get<std::string>();
    const fs::path receipts_root = paths.data_root / "receipts";
    const fs::path cli = installed_entrypoint(receipts_root, component_id, "cli");
    fs::path executable = cli;
    std::vector<std::string> arguments;
    const json* installer_integration = nullptr;
    std::optional<ProviderLifecycleControl> lifecycle_control;
    if (manifest.contains("installer_integration") &&
        manifest.at("installer_integration").is_object()) {
        installer_integration = &manifest.at("installer_integration");
        if (installer_integration->contains("startup_control")) {
            lifecycle_control = parse_provider_lifecycle_control(
                installer_integration->at("startup_control"));
        }
    }

    std::string expected_status_schema;
    const bool manager_integrated = !provider_service &&
                                    service_contract.contains("manager_integration_api");
    if (provider_service) {
        const fs::path contract_path =
            active_root / fs::path(service_contract.at("path").get<std::string>());
        const json provider_contract = read_json(contract_path);
        const json& invocation = provider_contract.at("invocation");
        if (invocation.value("kind", "") == "dependency_runtime") {
            executable = installed_entrypoint(
                receipts_root,
                invocation.at("runtime_component_id").get<std::string>(),
                invocation.at("runtime_entrypoint").get<std::string>());
            arguments.push_back(cli.string());
        }
        const fs::path operations_path = active_root /
            fs::path(service_contract.at("manager_operations_path")
                         .get<std::string>());
        const json operations = read_json(operations_path);
        const json& status_operation =
            operations.at("operations").at("provider_status");
        for (const auto& argument : status_operation.at("arguments")) {
            arguments.push_back(argument.get<std::string>());
        }
        expected_status_schema =
            status_operation.at("response").get<std::string>();
    } else if (manager_integrated) {
        if (installer_integration != nullptr) {
            const json& invocation = installer_integration->at("invocation");
            executable = installed_entrypoint(
                receipts_root,
                invocation.at("runtime_component_id").get<std::string>(),
                invocation.at("runtime_entrypoint").get<std::string>());
            arguments.push_back(cli.string());
        }
        const json& api = service_contract.at("manager_integration_api");
        for (const auto& argument : api.at("arguments")) {
            arguments.push_back(argument.get<std::string>());
        }
        arguments.emplace_back("status");
        expected_status_schema =
            service_contract.at("status_schema").get<std::string>();
    } else {
        const fs::path contract_path =
            active_root / fs::path(manifest.at("contract_manifest").get<std::string>());
        const json contract = read_json(contract_path);
        arguments.push_back(
            contract.at("health_smoke").at("command").get<std::string>());
        expected_status_schema =
            service_contract.at("status_schema").get<std::string>();
    }

    if (lifecycle_control) {
        const ProviderWorkspaceInvocation provider_workspace =
            validated_provider_workspace(
                paths, receipt, lifecycle_control->workspace_relative_path, true);
        arguments.push_back(lifecycle_control->workspace_flag);
        arguments.push_back(provider_workspace.control_workspace.string());
        const std::string configuration_path =
            receipt.value("configuration_path", "");
        if (!configuration_path.empty()) {
            const fs::path configuration =
                normalized_absolute(configuration_path);
            if (!within(provider_workspace.workspace, configuration) ||
                !fs::is_regular_file(configuration) ||
                contains_symbolic_link(configuration)) {
                throw std::runtime_error(
                    "provider health configuration is unavailable: " +
                    component_id);
            }
            arguments.push_back(lifecycle_control->configuration_flag);
            arguments.push_back(configuration.string());
        }
    }

    arena::application::ProcessSupervisor process;
    const json response =
        process.run_json(executable, active_root, arguments, std::chrono::seconds(30));
    if (provider_service) {
        if (!response.is_object() ||
            response.value("schema", "") != expected_status_schema) {
            throw std::runtime_error(
                "provider status failed contract validation: " + component_id);
        }
        return;
    }
    if (manager_integrated) {
        const json& api = service_contract.at("manager_integration_api");
        if (!response.is_object()) {
            throw std::runtime_error(
                "provider Manager API status failed contract validation: " +
                component_id);
        }
        const std::string response_api_version =
            response.value("api_version", "");
        const std::string declared_api_version = api.value("version", "");
        const auto major_version = [](const std::string& version) {
            return version.substr(0, version.find('.'));
        };
        if (response.value("schema", "") !=
                "uci_arena_manager_service_api_v1" ||
            !response.contains("api_version") ||
            !response.at("api_version").is_string() ||
            !compatible_manager_api_version(
                response.at("api_version").get_ref<const std::string&>()) ||
            !api.contains("version") || !api.at("version").is_string() ||
            major_version(response_api_version) !=
                major_version(declared_api_version) ||
            response.value("component_id", "") != component_id ||
            response.value("component_version", "") != component_version ||
            response.value("operation", "") != "status" ||
            !response.contains("ok") || !response.at("ok").is_boolean() ||
            !response.at("ok").get<bool>() ||
            !response.contains("payload") || !response.at("payload").is_object() ||
            !response.at("payload").contains("status") ||
            !response.at("payload").at("status").is_object() ||
            response.at("payload").at("status").value("schema", "") !=
                expected_status_schema) {
            throw std::runtime_error(
                "provider Manager API status failed contract validation: " +
                component_id);
        }
        return;
    }

    if (!response.is_object()) {
        throw std::runtime_error("provider health response is not an object: " +
                                 component_id);
    }
    const json status = response.value("schema", "") == expected_status_schema
                            ? response
                            : response.value("status", json::object());
    if (!status.is_object() || status.value("schema", "") != expected_status_schema) {
        throw std::runtime_error("provider health status schema is invalid: " +
                                 component_id);
    }
}

void rename_managed_component_tree_with_retry(const fs::path& source,
                                               const fs::path& destination,
                                               const std::string& component_id) {
    rename_tree_with_retry(source, destination,
                           "cannot replace managed component " + component_id +
                               " after shutdown");
}

void remove_managed_component_tree_with_retry(const fs::path& target,
                                               const std::string& component_id,
                                               const std::string_view operation) {
    constexpr int attempts = 100;
    constexpr auto retry_delay = std::chrono::milliseconds(100);
    std::string failure = "managed component root remains present";
    for (int attempt = 0; attempt < attempts; ++attempt) {
        std::error_code remove_error;
        fs::remove_all(target, remove_error);
        if (!remove_error && !fs::exists(target)) {
            return;
        }
        if (remove_error) {
            failure = remove_error.message();
        }
        if (attempt + 1 < attempts) {
            std::this_thread::sleep_for(retry_delay);
        }
    }
    throw std::runtime_error("cannot remove managed component " + component_id +
                             " during " + std::string(operation) + ": " + failure);
}

json cleanup_superseded_component_roots(
    const SetupPaths& paths,
    const std::function<void(const json&)>& checkpoint) {
    const fs::path components_root =
        normalized_absolute(paths.install_root / "components");
    json cleanup{{"schema", "arena_superseded_component_cleanup_v1"},
                 {"roots", json::array()}};
    if (!fs::is_directory(platform::filesystem_api_path(components_root))) {
        return cleanup;
    }

    std::map<std::string, fs::path, std::less<>> active_roots;
    for (const auto& receipt : receipt_rows(paths)) {
        if (!receipt.contains("component_id") ||
            !receipt.at("component_id").is_string() ||
            !receipt.contains("active_root") ||
            !receipt.at("active_root").is_string()) {
            continue;
        }
        const std::string component_id =
            receipt.at("component_id").get<std::string>();
        if (!valid_identifier(component_id)) {
            continue;
        }
        const fs::path active_root = normalized_absolute(
            receipt.at("active_root").get<std::string>());
        const fs::path component_root =
            normalized_absolute(components_root / component_id);
        if (active_root.parent_path() == component_root &&
            within(components_root, active_root) && active_root != components_root) {
            active_roots.emplace(component_id, active_root);
        }
    }

    for (const auto& [component_id, active_root] : active_roots) {
        const fs::path component_root = active_root.parent_path();
        std::error_code iteration_error;
        std::vector<fs::path> candidates;
        fs::directory_iterator iterator(
            platform::filesystem_api_path(component_root), iteration_error);
        const fs::directory_iterator end;
        while (!iteration_error && iterator != end) {
            const fs::path candidate = normalized_absolute(
                component_root / iterator->path().filename());
            if (candidate != active_root) {
                candidates.push_back(candidate);
            }
            iterator.increment(iteration_error);
        }
        if (iteration_error) {
            cleanup["roots"].push_back(
                {{"component_id", component_id},
                 {"path", component_root.string()},
                 {"state", "cleanup_failed"},
                 {"error", "cannot enumerate component roots: " +
                               iteration_error.message()}});
            checkpoint(cleanup);
            continue;
        }

        for (const fs::path& candidate : candidates) {
            json outcome{{"component_id", component_id},
                         {"path", candidate.string()}};
            try {
                if (!within(component_root, candidate) || candidate == component_root ||
                    contains_symbolic_link(candidate) ||
                    !fs::is_directory(platform::filesystem_api_path(candidate))) {
                    throw std::runtime_error(
                        "candidate is not a bounded regular component directory");
                }
                const json manifest = read_json(candidate / "arena-component.json");
                const std::string version =
                    manifest.value("component_version", "");
                if (manifest.value("component_id", "") != component_id ||
                    !valid_identifier(version)) {
                    throw std::runtime_error(
                        "candidate manifest identity does not match its component");
                }
                static_cast<void>(
                    validate_component_payload(candidate, component_id, version));
            } catch (const std::exception& error) {
                outcome["state"] = "preserved_modified_or_unrecognized";
                outcome["error"] = error.what();
                cleanup["roots"].push_back(std::move(outcome));
                checkpoint(cleanup);
                continue;
            }

            outcome["state"] = "removing";
            cleanup["roots"].push_back(outcome);
            checkpoint(cleanup);
            try {
                remove_managed_component_tree_with_retry(
                    candidate, component_id, "post-commit cleanup");
                cleanup["roots"].back()["state"] = "removed";
            } catch (const std::exception& error) {
                cleanup["roots"].back()["state"] = "cleanup_failed";
                cleanup["roots"].back()["error"] = error.what();
            }
            checkpoint(cleanup);
        }
    }
    return cleanup;
}

bool requires_uninstall_startup(const json& receipt) {
    const auto lifecycle = receipt_lifecycle_observation(receipt);
    return (lifecycle && lifecycle->installed) ||
           supports_startup_capability(receipt, "startup_uninstall");
}

bool requires_replacement_startup_quiescence(const json& receipt) {
    const auto lifecycle = receipt_lifecycle_observation(receipt);
    const bool installer_managed_startup = lifecycle && lifecycle->active;
    const bool credential_gated_startup =
        receipt.contains("service_contract") &&
        receipt.at("service_contract").is_object() &&
        receipt.at("service_contract").value("credential_gated_startup", false);
    return installer_managed_startup ||
           (credential_gated_startup &&
            supports_startup_capability(receipt, "startup_uninstall"));
}

} // namespace

json copy_managed_lichess_profile(const SetupPaths& paths, const fs::path& source,
                                  const json& validation);

std::string SetupEngine::platform_key() {
#if defined(_WIN32)
    return sizeof(void*) == 8 ? "windows-x86_64" : "windows-x86";
#elif defined(__aarch64__)
    return "linux-aarch64";
#else
    return "linux-x86_64";
#endif
}

json SetupEngine::load_catalog(const fs::path& catalog_path, const fs::path& metadata_directory,
                               const bool allow_unsigned_development_catalog,
                               const fs::path& rollback_state_path,
                               const fs::path& retained_development_trust_path) {
    std::string catalog_trust;
    if (metadata_directory.empty()) {
        if (!allow_unsigned_development_catalog) {
            const fs::path expected_catalog =
                retained_development_trust_path.parent_path() /
                "catalog.json";
            if (retained_development_trust_path.empty() ||
                normalized_absolute(catalog_path) !=
                    normalized_absolute(expected_catalog) ||
                !fs::is_regular_file(catalog_path) ||
                contains_symbolic_link(catalog_path) ||
                !fs::is_regular_file(
                    retained_development_trust_path) ||
                contains_symbolic_link(
                    retained_development_trust_path)) {
                throw std::runtime_error(
                    "trusted TUF metadata is required; unsigned catalogs are "
                    "development-only");
            }
            const json retained_trust =
                read_json(retained_development_trust_path);
            if (retained_trust.value("schema", "") !=
                    "arena_retained_development_catalog_trust_v1" ||
                retained_trust.value("schema_version", 0) != 1 ||
                retained_trust.value("scope", "") !=
                    "exact_retained_catalog" ||
                retained_trust.value("catalog_sha256", "") !=
                    sha256_file(catalog_path)) {
                throw std::runtime_error(
                    "retained development catalog trust does not match the "
                    "exact catalog");
            }
            catalog_trust = "retained_unsigned_development";
        } else {
            catalog_trust = "explicit_unsigned_development";
        }
    } else {
        verify_tuf_chain(catalog_path, metadata_directory, rollback_state_path);
        catalog_trust = "tuf";
    }
    json catalog = read_json(catalog_path);
    if (catalog.value("schema", "") != "arena_install_catalog_v1" ||
        catalog.value("schema_version", 0) != 1 || !catalog.contains("components") ||
        !catalog.at("components").is_array() || !catalog.contains("presets") ||
        !catalog.at("presets").is_object()) {
        throw std::runtime_error("installation catalog does not satisfy arena_install_catalog_v1");
    }
    std::set<std::string> ids;
    for (const auto& component : catalog.at("components")) {
        const std::string id = component.value("id", "");
        if (!valid_identifier(id) || !ids.insert(id).second || !component.contains("version") ||
            !component.contains("artifacts")) {
            throw std::runtime_error("installation catalog has an invalid component identity");
        }
    }
    catalog["_catalog_directory"] = normalized_absolute(catalog_path.parent_path()).string();
    catalog["_catalog_trust"] = catalog_trust;
    return catalog;
}

json SetupEngine::validate_release_media(const json& catalog,
                                         const fs::path& catalog_path,
                                         const fs::path& metadata_directory,
                                         const bool retain_development_trust) {
    if (retain_development_trust && !metadata_directory.empty()) {
        throw std::runtime_error(
            "development catalog trust cannot replace TUF metadata");
    }
    const fs::path source_catalog = normalized_absolute(catalog_path);
    const fs::path source_root = source_catalog.parent_path();
    if (!fs::is_regular_file(source_catalog) || contains_symbolic_link(source_catalog)) {
        throw std::runtime_error("verified release catalog is unavailable for retention");
    }
    if (!metadata_directory.empty()) {
        const fs::path metadata_root = normalized_absolute(metadata_directory);
        for (const auto* required :
             {"root.json", "targets.json", "snapshot.json", "timestamp.json"}) {
            const fs::path source = metadata_root / required;
            if (!fs::is_regular_file(source) || contains_symbolic_link(source)) {
                throw std::runtime_error(std::string("verified TUF metadata is unavailable: ") +
                                         required);
            }
        }
    }

    std::set<std::string> retained_sources;
    std::uint64_t retained_bytes = 0;
    for (const auto& component : catalog.at("components")) {
        const json& artifact = platform_artifact(component);
        const std::string source = artifact.at("source").get<std::string>();
        if (source.starts_with("https://")) {
            continue;
        }
        const fs::path relative = fs::path(source).lexically_normal();
        if (!safe_relative_path(relative)) {
            throw std::runtime_error("release catalog contains an unsafe retained source");
        }
        if (!retained_sources.insert(relative.generic_string()).second) {
            continue;
        }
        const fs::path source_file = source_root / relative;
        if (!within(source_root, source_file) || !fs::is_regular_file(source_file) ||
            contains_symbolic_link(source_file) ||
            fs::file_size(source_file) != integer_field(artifact, "size_bytes") ||
            sha256_file(source_file) != artifact.at("sha256").get<std::string>()) {
            throw std::runtime_error(
                "release payload cannot be retained with exact identity: " +
                relative.generic_string());
        }
        retained_bytes += integer_field(artifact, "size_bytes");
    }
    return {
        {"schema", "arena_release_media_preflight_v1"},
        {"catalog", source_catalog.string()},
        {"catalog_sha256", sha256_file(source_catalog)},
        {"retained_payload_count", retained_sources.size()},
        {"retained_payload_bytes", retained_bytes},
        {"trust", metadata_directory.empty() ? "development" : "tuf"},
        {"ok", true},
    };
}

json SetupEngine::persist_release_media(const json& catalog, const SetupPaths& paths,
                                        const fs::path& catalog_path,
                                        const fs::path& metadata_directory,
                                        const bool retain_development_trust) {
    static_cast<void>(validate_release_media(catalog, catalog_path, metadata_directory,
                                             retain_development_trust));
    if (retain_development_trust && !metadata_directory.empty()) {
        throw std::runtime_error(
            "development catalog trust cannot replace TUF metadata");
    }
    const fs::path source_catalog = normalized_absolute(catalog_path);
    const fs::path source_root = source_catalog.parent_path();
    if (!fs::is_regular_file(source_catalog) || contains_symbolic_link(source_catalog)) {
        throw std::runtime_error("verified release catalog is unavailable for retention");
    }
    const fs::path release_root = paths.data_root / "release";
    const std::string transaction_id =
        std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    const fs::path stage = paths.data_root / ("release.staging-" + transaction_id);
    const fs::path backup = paths.data_root / ("release.rollback-" + transaction_id);
    std::error_code ignored;
    fs::create_directories(paths.data_root);
    fs::remove_all(stage, ignored);
    ignored.clear();
    fs::remove_all(backup, ignored);
    try {
        fs::create_directories(stage);
        fs::copy_file(source_catalog, stage / "catalog.json", fs::copy_options::overwrite_existing);

        if (retain_development_trust) {
            std::ofstream trust_output(
                stage / "development-trust.json",
                std::ios::binary | std::ios::trunc);
            trust_output
                << json{
                       {"schema",
                        "arena_retained_development_catalog_trust_v1"},
                       {"schema_version", 1},
                       {"scope", "exact_retained_catalog"},
                       {"catalog_sha256",
                        sha256_file(source_catalog)},
                   }
                       .dump(2);
            trust_output.flush();
            if (!trust_output) {
                throw std::runtime_error(
                    "cannot retain the exact development catalog trust");
            }
        }

        if (!metadata_directory.empty()) {
            const fs::path metadata_root = normalized_absolute(metadata_directory);
            for (const auto* required :
                 {"root.json", "targets.json", "snapshot.json", "timestamp.json"}) {
                const fs::path source = metadata_root / required;
                if (!fs::is_regular_file(source) || contains_symbolic_link(source)) {
                    throw std::runtime_error(std::string("verified TUF metadata is unavailable: ") +
                                             required);
                }
            }
            fs::create_directories(stage / "tuf");
            for (const auto& entry : fs::directory_iterator(metadata_root)) {
                if (entry.is_symlink() || !entry.is_regular_file() ||
                    entry.path().extension() != ".json" ||
                    entry.file_size() > 8ULL * 1024ULL * 1024ULL) {
                    continue;
                }
                fs::copy_file(entry.path(), stage / "tuf" / entry.path().filename(),
                              fs::copy_options::overwrite_existing);
            }
        }

        json retained = json::array();
        std::set<std::string> retained_sources;
        for (const auto& component : catalog.at("components")) {
            const json& artifact = platform_artifact(component);
            const std::string source = artifact.at("source").get<std::string>();
            if (source.starts_with("https://")) {
                continue;
            }
            const fs::path relative = fs::path(source).lexically_normal();
            if (!safe_relative_path(relative) ||
                !retained_sources.insert(relative.generic_string()).second) {
                if (retained_sources.contains(relative.generic_string())) {
                    continue;
                }
                throw std::runtime_error("release catalog contains an unsafe retained source");
            }
            const fs::path source_file = source_root / relative;
            if (!within(source_root, source_file) || !fs::is_regular_file(source_file) ||
                contains_symbolic_link(source_file) ||
                fs::file_size(source_file) != integer_field(artifact, "size_bytes") ||
                sha256_file(source_file) != artifact.at("sha256").get<std::string>()) {
                throw std::runtime_error(
                    "release payload cannot be retained with exact identity: " +
                    relative.generic_string());
            }
            const fs::path destination = stage / relative;
            if (!within(stage, destination)) {
                throw std::runtime_error("retained release payload escapes staging");
            }
            fs::create_directories(destination.parent_path());
            const fs::path cached = paths.cache_root / artifact.at("sha256").get<std::string>();
            const fs::path link_source =
                fs::is_regular_file(cached) &&
                        sha256_file(cached) == artifact.at("sha256").get<std::string>()
                    ? cached
                    : source_file;
            std::error_code link_error;
            fs::create_hard_link(link_source, destination, link_error);
            if (link_error) {
                fs::copy_file(source_file, destination, fs::copy_options::overwrite_existing);
            }
            retained.push_back(relative.generic_string());
        }

        const bool had_existing = fs::exists(release_root);
        if (had_existing) {
            fs::rename(release_root, backup);
        }
        try {
            fs::rename(stage, release_root);
        } catch (...) {
            if (had_existing && fs::exists(backup) && !fs::exists(release_root)) {
                fs::rename(backup, release_root);
            }
            throw;
        }
        fs::remove_all(backup, ignored);
        return {
            {"schema", "arena_retained_release_media_v1"},
            {"catalog", (release_root / "catalog.json").string()},
            {"metadata",
             metadata_directory.empty() ? json(nullptr) : json((release_root / "tuf").string())},
            {"development_trust",
             retain_development_trust
                 ? json(
                       (release_root /
                        "development-trust.json")
                           .string())
                 : json(nullptr)},
            {"payloads", std::move(retained)},
        };
    } catch (...) {
        fs::remove_all(stage, ignored);
        throw;
    }
}

json inspect_opening_book_snapshot(const fs::path& book_root) {
    json result{{"state", "missing"},
                {"path", book_root.string()},
                {"missing_files", json::array()}};
    const fs::path manifest_path = book_root / "snapshot.manifest.json";
    try {
        if (!fs::is_directory(book_root) || contains_symbolic_link(book_root) ||
            !fs::is_regular_file(manifest_path) || contains_symbolic_link(manifest_path)) {
            result["missing_files"].push_back("snapshot.manifest.json");
            return result;
        }
        if (fs::file_size(manifest_path) > 1024ULL * 1024ULL) {
            throw std::runtime_error("snapshot manifest exceeds one MiB");
        }
        const json manifest = read_json(manifest_path);
        if (!manifest.is_object() || !manifest.contains("schema") ||
            !manifest.at("schema").is_string() || !manifest.contains("artifacts") ||
            !manifest.at("artifacts").is_object() || manifest.at("artifacts").empty() ||
            manifest.at("artifacts").size() > managed_book_max_files) {
            throw std::runtime_error("snapshot manifest has no bounded artifact map");
        }
        result["snapshot_schema"] = manifest.at("schema");
        result["artifact_count"] = manifest.at("artifacts").size();
        for (const auto& [name, digest] : manifest.at("artifacts").items()) {
            const fs::path relative = fs::path(name);
            const bool valid_digest =
                digest.is_string() && digest.get_ref<const std::string&>().size() == 64 &&
                std::ranges::all_of(digest.get_ref<const std::string&>(),
                                    [](const unsigned char character) {
                                        return std::isdigit(character) != 0 ||
                                               (character >= 'a' && character <= 'f');
                                    });
            if (!safe_relative_path(relative) || relative == fs::path("snapshot.manifest.json") ||
                !valid_digest) {
                throw std::runtime_error("snapshot manifest contains an unsafe artifact declaration");
            }
            const fs::path artifact = book_root / relative;
            if (!fs::is_regular_file(artifact) || contains_symbolic_link(artifact)) {
                result["missing_files"].push_back(relative.generic_string());
            }
        }
        result["state"] = result["missing_files"].empty() ? "detected_unmanaged" : "partial";
    } catch (const std::exception& error) {
        result["state"] = "invalid";
        result["validation_error"] = error.what();
    }
    return result;
}

json SetupEngine::inventory(const SetupPaths& paths,
                             const std::vector<fs::path>& extra_roots,
                             ports::AssetSearchIndex* search_index) {
    json knowledge{
        {"opening_book", inspect_opening_book_snapshot(paths.knowledge_root / "opening_books")},
        {"syzygy",
         {{"state", "missing"},
          {"path", paths.knowledge_root.string()},
          {"wdl_files", 0},
          {"dtz_files", 0},
          {"bytes", 0}}},
    };
    json discovery = discovery_rows(paths, extra_roots, search_index);
    const auto managed_components = [&discovery](const std::string& kind) {
        std::set<std::string> ids;
        for (const auto& candidate : discovery.at(kind).at("candidates")) {
            if (candidate.value("source", "") == "install_receipt" &&
                candidate.value("state", "") == "installed_managed" &&
                candidate.contains("details") && candidate.at("details").is_object() &&
                candidate.at("details").contains("component_id") &&
                candidate.at("details").at("component_id").is_string()) {
                ids.insert(candidate.at("details").at("component_id").get<std::string>());
            }
        }
        return json(ids);
    };
    const fs::path tablebase_root = paths.knowledge_root / "tablebases";
    if (fs::is_directory(tablebase_root) && !contains_symbolic_link(tablebase_root)) {
        for (const auto& entry : fs::directory_iterator(tablebase_root)) {
            if (!entry.is_regular_file() || entry.is_symlink()) {
                continue;
            }
            if (entry.path().extension() == ".rtbw") {
                knowledge["syzygy"]["wdl_files"] =
                    knowledge["syzygy"]["wdl_files"].get<std::uint64_t>() + 1;
            } else if (entry.path().extension() == ".rtbz") {
                knowledge["syzygy"]["dtz_files"] =
                    knowledge["syzygy"]["dtz_files"].get<std::uint64_t>() + 1;
            } else {
                continue;
            }
            knowledge["syzygy"]["bytes"] =
                knowledge["syzygy"]["bytes"].get<std::uint64_t>() + entry.file_size();
        }
        if (knowledge["syzygy"]["wdl_files"].get<std::uint64_t>() > 0) {
            knowledge["syzygy"]["state"] = "detected_unmanaged";
            knowledge["syzygy"]["path"] = tablebase_root.string();
        }
    }
    const json& book_selection = discovery.at("opening_book").at("selection");
    const bool managed_book_selection =
        book_selection.value("source", "") == "install_receipt" &&
        book_selection.value("state", "") == "installed_managed";
    if (knowledge["opening_book"]["state"] == "missing" || managed_book_selection) {
        if (book_selection.value("state", "") != "unresolved" &&
            book_selection.value("state", "") != "broken" &&
            book_selection.contains("path") && book_selection.at("path").is_string()) {
            try {
                const fs::path selected = book_selection.at("path").get<std::string>();
                knowledge["opening_book"]["validation"] = validate_book_locator(selected);
                knowledge["opening_book"]["state"] =
                    managed_book_selection ? "installed_managed" : "detected_unmanaged";
                knowledge["opening_book"]["path"] = normalized_absolute(selected).string();
                knowledge["opening_book"]["source"] =
                    book_selection.value("source", "discovered");
                if (managed_book_selection && book_selection.contains("details") &&
                    book_selection.at("details").contains("component_id")) {
                    knowledge["opening_book"]["managed_components"] =
                        managed_components("opening_book");
                }
            } catch (const std::exception&) {
            }
        }
    }
    const json& syzygy_selection = discovery.at("syzygy").at("selection");
    const bool managed_syzygy_selection =
        syzygy_selection.value("source", "") == "install_receipt" &&
        syzygy_selection.value("state", "") == "installed_managed";
    if (knowledge["syzygy"]["state"] == "missing" || managed_syzygy_selection) {
        if (syzygy_selection.value("state", "") != "unresolved" &&
            syzygy_selection.value("state", "") != "broken" &&
            syzygy_selection.contains("path") && syzygy_selection.at("path").is_string()) {
            try {
                const fs::path selected = syzygy_selection.at("path").get<std::string>();
                const json validation = validate_syzygy_locator(selected);
                knowledge["syzygy"]["state"] =
                    managed_syzygy_selection ? "installed_managed" : "detected_unmanaged";
                knowledge["syzygy"]["path"] = normalized_absolute(selected).string();
                knowledge["syzygy"]["source"] =
                    syzygy_selection.value("source", "discovered");
                knowledge["syzygy"]["wdl_files"] = validation.at("wdl_files");
                knowledge["syzygy"]["dtz_files"] = validation.at("dtz_files");
                knowledge["syzygy"]["bytes"] = validation.at("bytes");
                if (managed_syzygy_selection && syzygy_selection.contains("details") &&
                    syzygy_selection.at("details").contains("component_id")) {
                    knowledge["syzygy"]["managed_components"] =
                        managed_components("syzygy");
                }
            } catch (const std::exception&) {
            }
        }
    }

    json candidates = json::array();
    std::vector<fs::path> bounded_roots = extra_roots;
    if (const char* path_environment = std::getenv("PATH")) {
#ifdef _WIN32
        constexpr char separator = ';';
#else
        constexpr char separator = ':';
#endif
        std::stringstream paths_stream(path_environment);
        for (std::string item; std::getline(paths_stream, item, separator);) {
            if (!item.empty()) {
                bounded_roots.emplace_back(item);
            }
        }
    }
    const std::set<std::string> candidate_names{
#ifdef _WIN32
        "uci_arena_engine.exe", "stockfish.exe", "lc0.exe"
#else
        "uci_arena_engine", "stockfish", "lc0"
#endif
    };
    std::set<std::string> seen;
    for (const auto& root : bounded_roots) {
        if (!fs::is_directory(root) || contains_symbolic_link(root)) {
            continue;
        }
        for (const auto& name : candidate_names) {
            const fs::path candidate = root / name;
            if (fs::is_regular_file(candidate) && !contains_symbolic_link(candidate)) {
                const std::string normalized = normalized_absolute(candidate).string();
                if (seen.insert(normalized).second) {
                    candidates.push_back({{"kind", "uci_engine"},
                                          {"state", "detected_unmanaged"},
                                          {"path", normalized},
                                          {"requires_probe", true}});
                }
            }
        }
    }

    json restaurant{{"state", "absent"}};
    if (const char* configured = std::getenv("RESTAURANT_ROOT");
        configured != nullptr && fs::is_directory(configured) &&
        !contains_symbolic_link(configured)) {
        restaurant = {
            {"state", "detected_unmanaged"},
            {"path", normalized_absolute(configured).string()},
            {"installable", false},
        };
    }
    return {
        {"schema", "arena_install_inventory_v1"},
        {"platform", platform_key()},
        {"managed_components", receipt_rows(paths)},
        {"knowledge", std::move(knowledge)},
        {"candidates", std::move(candidates)},
        {"discovery", std::move(discovery)},
        {"restaurant", std::move(restaurant)},
    };
}

json SetupEngine::import_path(const SetupPaths& paths, const std::string& kind,
                              const fs::path& path, const bool managed_copy,
                              const std::string& authority_mode) {
    const fs::path normalized = normalized_absolute(path);
    if (kind != "opening_book" && !authority_mode.empty()) {
        throw std::runtime_error("authority mode is supported only for opening_book");
    }
    const std::string opening_book_authority = kind == "opening_book"
        ? (authority_mode.empty() ? "immutable_pinned_snapshot" : authority_mode)
        : std::string{};
    if (kind == "opening_book" &&
        opening_book_authority != "service_managed_live_channel" &&
        opening_book_authority != "immutable_pinned_snapshot") {
        throw std::runtime_error("opening_book authority mode is invalid");
    }
    if (managed_copy && opening_book_authority == "service_managed_live_channel") {
        throw std::runtime_error(
            "service-managed opening-book authority cannot use managed copy");
    }
    if (opening_book_authority == "service_managed_live_channel" &&
        (!fs::is_directory(normalized) || contains_symbolic_link(normalized))) {
        throw std::runtime_error(
            "service-managed opening-book authority requires a regular directory");
    }
    if (kind == "lichess_token_path") {
        require_external_token_path(paths.data_root, normalized);
    }
    const json validation = validate_import_locator(kind, normalized);
    if (kind == "lichess_single_game_profile") {
        json result = copy_managed_lichess_profile(paths, normalized, validation);
        const fs::path active_profile =
            normalized_absolute(result.at("path").get<std::string>());
        const fs::path active_root =
            normalized_absolute(result.at("receipt").at("active_root").get<std::string>());
        const fs::path receipt_path =
            paths.data_root / "receipts" /
            (result.at("receipt").at("component_id").get<std::string>() + ".json");
        const bool active_root_existed = result.at("_active_root_existed").get<bool>();
        const bool receipt_existed = result.at("_receipt_existed").get<bool>();
        try {
            const json managed_validation =
                validate_import_locator(kind, active_profile);
            result["validation"] = managed_validation;
            result["refreshed_consumers"] = save_locator_and_refresh(
                paths, kind, active_profile, managed_validation, "managed_copy");
        } catch (...) {
            std::error_code ignored;
            if (!receipt_existed) {
                fs::remove(platform::filesystem_api_path(receipt_path), ignored);
                ignored.clear();
            }
            if (!active_root_existed) {
                fs::remove_all(platform::filesystem_api_path(active_root), ignored);
            }
            throw;
        }
        result.erase("_active_root_existed");
        result.erase("_receipt_existed");
        return result;
    }
    if (managed_copy) {
        if (kind != "opening_book") {
            throw std::runtime_error("managed copy is allowed only for opening books");
        }
        json result = copy_managed_book(paths, normalized, validation);
        const fs::path active_root = normalized_absolute(result.at("path").get<std::string>());
        const fs::path receipt_path =
            paths.data_root / "receipts" /
            (result.at("receipt").at("component_id").get<std::string>() + ".json");
        const bool active_root_existed = result.at("_active_root_existed").get<bool>();
        const bool receipt_existed = result.at("_receipt_existed").get<bool>();
        try {
            const json managed_validation = validate_import_locator(kind, active_root);
            result["refreshed_consumers"] =
                save_locator_and_refresh(paths, kind, active_root, managed_validation,
                                         "managed_copy", opening_book_authority);
            result["authority_mode"] = opening_book_authority;
        } catch (...) {
            std::error_code ignored;
            if (!receipt_existed) {
                fs::remove(platform::filesystem_api_path(receipt_path), ignored);
                ignored.clear();
            }
            if (!active_root_existed) {
                fs::remove_all(platform::filesystem_api_path(active_root), ignored);
            }
            throw;
        }
        result.erase("_active_root_existed");
        result.erase("_receipt_existed");
        return result;
    }

    json result{
        {"schema", "arena_setup_import_result_v1"},
        {"schema_version", 1},
        {"kind", kind},
        {"mode", "in_place_reference"},
        {"state", "saved"},
        {"path", normalized.string()},
        {"validation", validation},
        {"refreshed_consumers", save_locator_and_refresh(paths, kind, normalized, validation,
                                                          "in_place_reference",
                                                          opening_book_authority)},
    };
    if (!opening_book_authority.empty()) {
        result["authority_mode"] = opening_book_authority;
    }
    return result;
}

json SetupEngine::save_lichess_token_reference(
    const fs::path& data_root,
    const fs::path& path) {
    const fs::path normalized = normalized_absolute(path);
    require_external_token_path(data_root, normalized);
    const json validation =
        validate_import_locator("lichess_token_path", normalized);
    write_json_atomic(
        data_root / "locators" / "lichess_token_path.json",
        {
            {"schema", "arena_saved_locator_v1"},
            {"schema_version", 1},
            {"kind", "lichess_token_path"},
            {"mode", "in_place_reference"},
            {"path", normalized.string()},
            {"validation", validation},
            {"saved_at", current_utc_iso()},
        });
    return {
        {"schema", "arena_bot_token_reference_result_v1"},
        {"schema_version", 1},
        {"state", "saved"},
        {"validation", "path_reference_only"},
    };
}

json SetupEngine::resolve_feature_selection(
    const json& catalog, const std::vector<std::string>& assignments) {
    if (!catalog.contains("feature_selection") ||
        !catalog.at("feature_selection").is_object()) {
        throw std::runtime_error(
            "catalog does not declare installer feature selection");
    }
    const json& contract = catalog.at("feature_selection");
    if (!contract.contains("base_preset") ||
        !contract.at("base_preset").is_string() ||
        !contract.contains("components") ||
        !contract.at("components").is_array() ||
        !contract.contains("default_selected") ||
        !contract.at("default_selected").is_array()) {
        throw std::runtime_error("catalog feature-selection contract is invalid");
    }
    const std::string base_preset =
        contract.at("base_preset").get<std::string>();
    if (!valid_identifier(base_preset) ||
        !catalog.at("presets").contains(base_preset)) {
        throw std::runtime_error(
            "catalog feature-selection base preset is invalid");
    }

    std::vector<std::string> feature_ids;
    std::set<std::string, std::less<>> allowed;
    for (const auto& value : contract.at("components")) {
        if (!value.is_string()) {
            throw std::runtime_error(
                "catalog feature-selection component id is invalid");
        }
        const std::string id = value.get<std::string>();
        if (!valid_identifier(id) || !allowed.insert(id).second) {
            throw std::runtime_error(
                "catalog feature-selection component id is invalid");
        }
        static_cast<void>(component_by_id(catalog, id));
        feature_ids.push_back(id);
    }

    std::map<std::string, bool, std::less<>> selected;
    for (const auto& id : feature_ids) {
        selected.emplace(id, false);
    }
    for (const auto& value : contract.at("default_selected")) {
        if (!value.is_string() ||
            !selected.contains(value.get<std::string>())) {
            throw std::runtime_error(
                "catalog default feature is not operator-selectable");
        }
        selected.at(value.get<std::string>()) = true;
    }
    for (const auto& assignment : assignments) {
        const auto separator = assignment.find('=');
        if (separator == std::string::npos ||
            (assignment.substr(separator + 1) != "0" &&
             assignment.substr(separator + 1) != "1")) {
            throw std::runtime_error(
                "bundle feature must be component-id=0 or component-id=1");
        }
        const std::string id = assignment.substr(0, separator);
        if (!selected.contains(id)) {
            throw std::runtime_error("unknown bundle feature " + id);
        }
        selected.at(id) = assignment.substr(separator + 1) == "1";
    }

    std::vector<std::string> roots =
        catalog.at("presets").at(base_preset).get<std::vector<std::string>>();
    for (const auto& id : feature_ids) {
        if (selected.at(id)) {
            roots.push_back(id);
        }
    }
    const auto selected_closure =
        selected_ids(catalog, "custom", roots, false, false, false);
    const auto controlled_closure =
        selected_ids(catalog, "custom", feature_ids, false, false, false);
    const auto base_closure =
        selected_ids(catalog, base_preset, {}, false, false, false);
    const std::set<std::string, std::less<>> selected_set(
        selected_closure.begin(), selected_closure.end());
    const std::set<std::string, std::less<>> base_set(
        base_closure.begin(), base_closure.end());
    json removals = json::array();
    for (const auto& id : controlled_closure) {
        if (!selected_set.contains(id) && !base_set.contains(id)) {
            removals.push_back(id);
        }
    }
    json state = json::object();
    for (const auto& [id, enabled] : selected) {
        state[id] = enabled;
    }
    return {
        {"schema", "arena_install_feature_selection_v1"},
        {"base_preset", base_preset},
        {"state", std::move(state)},
        {"selected_roots", roots},
        {"selected_closure", selected_closure},
        {"removal_candidates", std::move(removals)},
    };
}

json SetupEngine::plan(const json& catalog, const SetupPaths& paths, const std::string& preset,
                       const std::vector<std::string>& selected_components, const bool offline,
                       const bool download_missing_knowledge, const bool download_missing_books,
                       const bool download_missing_tablebases, const bool repair,
                       const bool upgrade) {
    const auto ids = selected_ids(catalog, preset, selected_components, download_missing_knowledge,
                                  download_missing_books, download_missing_tablebases);
    json actions = json::array();
    json blockers = json::array();
    std::uint64_t download = 0;
    std::uint64_t staging = 0;
    std::uint64_t installed = 0;
    std::uint64_t knowledge_activation = 0;
    std::uint64_t backup = 0;
    std::uint64_t reserve = 0;
    std::set<std::string> artifact_hashes;
    for (const auto& id : ids) {
        const auto& component = component_by_id(catalog, id);
        const auto& artifact = platform_artifact(component);
        reserve = (std::max)(reserve, integer_field(component, "workspace_reserve_bytes"));
        const fs::path receipt_path = paths.data_root / "receipts" / (id + ".json");
        bool already_current = false;
        bool exact_current_artifact = false;
        bool current_enabled = true;
        if (fs::is_regular_file(receipt_path)) {
            try {
                const json receipt = read_json(receipt_path);
                if (receipt.value("component_id", "") == id &&
                    receipt.value("component_version", "") ==
                        component.at("version").get<std::string>() &&
                    (receipt.value("state", "") == "installed_managed" ||
                     receipt.value("state", "") == "installed_disabled")) {
                    validate_component_payload(receipt.at("active_root").get<std::string>(), id,
                                               receipt.at("component_version").get<std::string>());
                    already_current = true;
                    exact_current_artifact =
                        receipt.value("artifact_sha256", "") ==
                        artifact.at("sha256").get<std::string>();
                    current_enabled = receipt.value("state", "") == "installed_managed";
                    if (!repair && !upgrade) {
                        actions.push_back({{"action", "keep"},
                                           {"component_id", id},
                                           {"version", component.at("version")},
                                           {"enabled", current_enabled}});
                        continue;
                    }
                    if (upgrade && exact_current_artifact) {
                        actions.push_back(
                            {{"action", "refresh"},
                             {"component_id", id},
                             {"version", component.at("version")},
                             {"dependencies",
                              component.value("dependencies", json::array())},
                             {"enabled", current_enabled},
                             {"artifact", artifact}});
                        continue;
                    }
                }
            } catch (const std::exception&) {
            }
        }
        const std::string selected_version = component.at("version").get<std::string>();
        const fs::path selected_root =
            paths.install_root / "components" / id / selected_version;
        const bool partial_selected_root = fs::exists(selected_root) && !already_current;
        if (partial_selected_root && !repair && !upgrade) {
            blockers.push_back({
                {"code", "partial_installation_requires_repair"},
                {"component_id", id},
                {"version", selected_version},
                {"path", selected_root.string()},
            });
        }
        const std::string hash = artifact.at("sha256").get<std::string>();
        if (artifact_hashes.insert(hash).second) {
            download += artifact.value("type", "archive") == "file_set"
                            ? integer_field(artifact, "size_bytes") +
                                  integer_field(artifact, "total_size_bytes")
                            : integer_field(artifact, "size_bytes");
            staging += integer_field(component, "staging_bytes");
        }
        installed += integer_field(component, "installed_bytes");
        if (component.value("activation_role", "component") == "knowledge") {
            knowledge_activation += integer_field(component, "installed_bytes");
        }
        backup += integer_field(component, "backup_bytes");
        actions.push_back({{"action", already_current || partial_selected_root ? "repair"
                                                                             : "install"},
                           {"component_id", id},
                           {"version", component.at("version")},
                           {"dependencies", component.value("dependencies", json::array())},
                           {"enabled",
                            already_current ? current_enabled
                                            : component.value("default_enabled", true)},
                           {"artifact", artifact}});
    }
    std::uint64_t effective_reserve = reserve;
    if (preset != "complete_arena" && reserve > 0) {
        const fs::space_info workspace_space =
            fs::space(nearest_existing_ancestor(paths.workspace_root));
        if (workspace_space.available < reserve) {
            for (auto& action : actions) {
                const std::string id = action.at("component_id").get<std::string>();
                if ((action.value("action", "") == "install" ||
                     action.value("action", "") == "repair") &&
                    component_by_id(catalog, id).value(
                        "disable_when_workspace_capacity_unavailable", false)) {
                    action["enabled"] = false;
                    action["warning"] = "installed disabled until workspace reserve is available";
                }
            }
            effective_reserve = 0;
        }
    }
    const std::uint64_t headroom = (std::max)(gib, staging / 10);
    std::map<std::string, json> volumes;
    const auto allocate = [&volumes](const fs::path& path, const std::string& role,
                                     const char* field, const std::uint64_t bytes) {
        if (bytes == 0) {
            return;
        }
        const fs::path capacity_path = nearest_existing_ancestor(path);
        const std::string key = volume_key(capacity_path);
        if (!volumes.contains(key)) {
            const auto space = fs::space(capacity_path);
            volumes[key] = {
                {"volume_id", key},
                {"capacity_path", capacity_path.string()},
                {"roles", json::array()},
                {"download_cache_bytes", 0},
                {"staging_bytes", 0},
                {"installed_bytes", 0},
                {"backup_bytes", 0},
                {"operational_reserve_bytes", 0},
                {"headroom_bytes", 0},
                {"peak_required_bytes", 0},
                {"available_bytes", space.available},
            };
        }
        auto& volume = volumes.at(key);
        if (std::find(volume["roles"].begin(), volume["roles"].end(), role) ==
            volume["roles"].end()) {
            volume["roles"].push_back(role);
        }
        volume[field] = volume.at(field).get<std::uint64_t>() + bytes;
        volume["peak_required_bytes"] =
            volume.at("peak_required_bytes").get<std::uint64_t>() + bytes;
    };
    allocate(paths.cache_root, "download_cache", "download_cache_bytes", download);
    allocate(paths.install_root, "staging", "staging_bytes", staging);
    allocate(paths.install_root, "components", "installed_bytes", installed);
    allocate(paths.install_root, "rollback", "backup_bytes", backup);
    allocate(paths.install_root, "staging_headroom", "headroom_bytes", headroom);
    allocate(paths.knowledge_root, "knowledge_activation", "installed_bytes", knowledge_activation);
    allocate(paths.workspace_root, "operational_workspace", "operational_reserve_bytes",
             effective_reserve);

    json volume_rows = json::array();
    std::uint64_t peak = 0;
    for (auto& [key, volume] : volumes) {
        (void)key;
        const auto required = volume.at("peak_required_bytes").get<std::uint64_t>();
        const auto available = volume.at("available_bytes").get<std::uint64_t>();
        peak += required;
        if (available < required) {
            const bool workspace_volume =
                std::find(volume["roles"].begin(), volume["roles"].end(),
                          "operational_workspace") != volume["roles"].end();
            blockers.push_back(
                {{"code", preset == "complete_arena" && workspace_volume ? "workspace_reserve_unmet"
                                                                         : "insufficient_space"},
                 {"volume_id", volume.at("volume_id")},
                 {"path", volume.at("capacity_path")},
                 {"available_bytes", available},
                 {"required_bytes", required}});
        }
        volume_rows.push_back(volume);
    }
    return {
        {"schema", "arena_install_plan_v1"},
        {"schema_version", 1},
        {"suite_version", catalog.at("suite_version")},
        {"platform", platform_key()},
        {"preset", preset},
        {"offline", offline},
        {"repair", repair},
        {"upgrade", upgrade},
        {"download_missing_knowledge", download_missing_knowledge},
        {"download_missing_books", download_missing_knowledge || download_missing_books},
        {"download_missing_tablebases", download_missing_knowledge || download_missing_tablebases},
        {"catalog_directory", catalog.value("_catalog_directory", ".")},
        {"paths",
         {{"install_root", paths.install_root.string()},
          {"data_root", paths.data_root.string()},
          {"cache_root", paths.cache_root.string()},
          {"workspace_root", paths.workspace_root.string()},
          {"knowledge_root", paths.knowledge_root.string()}}},
        {"space",
         {{"download_cache_bytes", download},
          {"staging_bytes", staging},
          {"installed_bytes", installed},
          {"knowledge_activation_bytes", knowledge_activation},
          {"backup_bytes", backup},
          {"operational_reserve_bytes", effective_reserve},
          {"headroom_bytes", headroom},
          {"peak_required_bytes", peak},
          {"volumes", std::move(volume_rows)}}},
        {"actions", std::move(actions)},
        {"blockers", std::move(blockers)},
    };
}

json SetupEngine::apply(const json&, const json& plan, const SetupPaths& paths,
                        const bool confirmed, const bool repair) {
    if (!confirmed) {
        throw std::runtime_error("apply requires explicit confirmation");
    }
    if (plan.value("schema", "") != "arena_install_plan_v1" || !plan.at("blockers").empty()) {
        throw std::runtime_error("installation plan is invalid or blocked");
    }
    if (!paths.bundle_owner.empty() && !valid_bundle_owner(paths.bundle_owner)) {
        throw std::runtime_error("bundle owner is invalid");
    }
    const fs::path components_root = paths.install_root / "components";
    const fs::path staging_root = paths.install_root / ".staging";
    const fs::path receipts_root = paths.data_root / "receipts";
    const fs::path transactions_root = paths.data_root / "transactions";
    fs::create_directories(components_root);
    fs::create_directories(staging_root);
    fs::create_directories(receipts_root);
    fs::create_directories(transactions_root);
    const std::string transaction_id =
        std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    const fs::path journal_path = transactions_root / (transaction_id + ".json");
    json journal{
        {"schema", "arena_install_transaction_v1"},
        {"transaction_id", transaction_id},
        {"state", "running"},
        {"actions", json::array()},
    };
    write_json_atomic(journal_path, journal);
    json results = json::array();
    std::set<std::string> migrated_locator_kinds;
    journal["consumer_refresh_writes"] = json::array();
    journal["startup_quiescence"] = json::array();
    std::map<std::string, fs::path> staged_roots;
    std::map<std::string, std::string> staged_versions;
    std::set<std::string> quiesced_startup_components;
    json superseded_component_cleanup{
        {"schema", "arena_superseded_component_cleanup_v1"},
        {"roots", json::array()}};
    try {
        const fs::path catalog_directory = fs::path(plan.value("catalog_directory", "."));
        std::set<std::string> replaced_component_ids;
        std::set<std::string> refreshed_component_ids;
        for (const auto& action : plan.at("actions")) {
            const std::string action_kind = action.value("action", "install");
            if (action_kind == "keep") continue;
            const std::string id = action.at("component_id").get<std::string>();
            const std::string version = action.at("version").get<std::string>();
            if (!valid_identifier(id) || !valid_identifier(version)) {
                throw std::runtime_error(
                    "plan contains an unsafe component identity");
            }
            if (action_kind == "refresh") {
                if (replaced_component_ids.contains(id) ||
                    !refreshed_component_ids.insert(id).second) {
                    throw std::runtime_error(
                        "plan contains a duplicate refresh component identity");
                }
                continue;
            }
            if (!replaced_component_ids.insert(id).second ||
                refreshed_component_ids.contains(id)) {
                throw std::runtime_error(
                    "plan contains a duplicate component identity");
            }
            const fs::path artifact = acquire_artifact(
                action.at("artifact"), catalog_directory, paths,
                plan.value("offline", false));
            const fs::path stage = staging_root / transaction_id / id;
            std::error_code ignored;
            fs::remove_all(stage, ignored);
            if (action.at("artifact").value("type", "archive") == "file_set") {
                static_cast<void>(stage_file_set(
                    artifact, action.at("artifact"), catalog_directory, paths,
                    plan.value("offline", false), stage, id, version));
            } else {
                extract_archive(artifact, stage);
            }
            static_cast<void>(validate_component_payload(stage, id, version));
            staged_roots.emplace(id, stage);
            staged_versions.emplace(id, version);
        }

        std::map<std::string, json> affected_startup_receipts;
        std::map<std::string, std::set<std::string>> affected_startup_dependencies;
        for (const auto& receipt_entry : fs::directory_iterator(receipts_root)) {
            if (!receipt_entry.is_regular_file() || receipt_entry.is_symlink() ||
                receipt_entry.path().extension() != ".json") {
                continue;
            }
            const json receipt = read_json(receipt_entry.path());
            if (!receipt.contains("component_id") ||
                !receipt.at("component_id").is_string()) {
                continue;
            }
            const std::string id = receipt.at("component_id").get<std::string>();
            const bool startup_requires_quiescence =
                requires_replacement_startup_quiescence(receipt);
            if (!startup_requires_quiescence) continue;
            if (!receipt.contains("active_root") ||
                !receipt.at("active_root").is_string() ||
                !receipt.contains("component_version") ||
                !receipt.at("component_version").is_string()) {
                throw std::runtime_error(
                    "installed provider receipt cannot resolve dependency bindings: " +
                    id);
            }
            const json manifest = validate_component_payload(
                normalized_absolute(receipt.at("active_root").get<std::string>()),
                id, receipt.at("component_version").get<std::string>()).manifest;
            const auto dependencies =
                provider_dependency_component_ids(receipt, manifest);
            bool affected = replaced_component_ids.contains(id);
            if (!affected) {
                affected = std::ranges::any_of(
                    dependencies,
                    [&replaced_component_ids](const std::string& dependency) {
                        return replaced_component_ids.contains(dependency);
                    });
            }
            if (!affected) continue;
            if (!supports_startup_capability(receipt, "startup_uninstall")) {
                throw std::runtime_error(
                    "installed provider startup cannot be stopped before dependency replacement: " +
                    id);
            }
            affected_startup_receipts.emplace(id, receipt);
            affected_startup_dependencies.emplace(id, dependencies);
        }

        std::vector<std::string> quiescence_order;
        std::set<std::string> ordered_startup_components;
        while (quiescence_order.size() < affected_startup_receipts.size()) {
            bool selected = false;
            for (const auto& [id, receipt] : affected_startup_receipts) {
                if (ordered_startup_components.contains(id)) continue;
                bool has_unordered_dependent = false;
                for (const auto& other : affected_startup_receipts) {
                    const std::string& other_id = other.first;
                    if (id == other_id || ordered_startup_components.contains(other_id)) continue;
                    const auto& dependencies =
                        affected_startup_dependencies.at(other_id);
                    if (dependencies.contains(id)) {
                        has_unordered_dependent = true;
                        break;
                    }
                }
                if (has_unordered_dependent) continue;
                quiescence_order.push_back(id);
                ordered_startup_components.insert(id);
                selected = true;
                break;
            }
            if (!selected) {
                throw std::runtime_error(
                    "startup dependency cycle prevents transaction-wide quiescence");
            }
        }

        for (const std::string& id : quiescence_order) {
            const json& previous = affected_startup_receipts.at(id);
            const fs::path previous_root =
                previous.at("active_root").get<std::string>();
            const bool staged = staged_roots.contains(id);
            const fs::path control_root = staged ? staged_roots.at(id) : previous_root;
            const std::string control_version =
                staged ? staged_versions.at(id)
                       : previous.at("component_version").get<std::string>();
            const json control_manifest =
                validate_component_payload(control_root, id, control_version).manifest;
            bool replacement_lifecycle = false;
            if (control_manifest.contains("installer_integration") &&
                control_manifest.at("installer_integration")
                    .contains("startup_control")) {
                const auto control = parse_provider_lifecycle_control(
                    control_manifest.at("installer_integration")
                        .at("startup_control"));
                replacement_lifecycle =
                    control.operations.contains(
                        ProviderLifecycleOperation::pre_replace) &&
                    control.operations.contains(
                        ProviderLifecycleOperation::post_replace);
            }
            const fs::path replacement_root = staged
                                                  ? components_root / id / control_version
                                                  : previous_root;
            json transition{
                {"component_id", id},
                {"state", "transition_started"},
                {"mode", replacement_lifecycle ? "pre_replace" : "uninstall"},
                {"previous_receipt", previous},
                {"previous_root", previous_root.string()},
                {"replacement_root", replacement_root.string()},
                {"control_version", control_version},
            };
            if (staged) transition["staged_control_root"] = control_root.string();
            journal["startup_quiescence"].push_back(std::move(transition));
            write_json_atomic(journal_path, journal);
            const ProviderLifecycleResult lifecycle_result = replacement_lifecycle
                ? invoke_provider_lifecycle(
                      paths, previous, ProviderLifecycleOperation::pre_replace,
                      true, staged ? control_root : fs::path{},
                      staged ? control_version : std::string{}, previous_root,
                      replacement_root)
                : invoke_provider_lifecycle(
                      paths, previous, ProviderLifecycleOperation::uninstall, true);
            journal["startup_quiescence"].back()["lifecycle_result"] =
                provider_lifecycle_result_to_json(lifecycle_result);
            journal["startup_quiescence"].back()["state"] = "quiesced";
            quiesced_startup_components.insert(id);
            write_json_atomic(journal_path, journal);
        }

        for (const auto& action : plan.at("actions")) {
            const std::string action_kind = action.value("action", "install");
            if (action_kind == "keep") {
                results.push_back({{"component_id", action.at("component_id")},
                                   {"component_version", action.at("version")},
                                   {"state", "already_current"}});
                continue;
            }
            const std::string id = action.at("component_id").get<std::string>();
            const std::string version = action.at("version").get<std::string>();
            if (!valid_identifier(id) || !valid_identifier(version)) {
                throw std::runtime_error("plan contains an unsafe component identity");
            }
            const fs::path receipt_path = receipts_root / (id + ".json");
            if (action_kind == "refresh") {
                if (!fs::is_regular_file(receipt_path) ||
                    !action.contains("artifact") ||
                    !action.at("artifact").is_object()) {
                    throw std::runtime_error(
                        "exact-current refresh is missing receipt or artifact evidence: " +
                        id);
                }
                json receipt = read_json(receipt_path);
                const std::string expected_state =
                    action.value("enabled", true) ? "installed_managed"
                                                   : "installed_disabled";
                const std::string artifact_sha256 =
                    action.at("artifact").value("sha256", "");
                const fs::path target = components_root / id / version;
                if (receipt.value("schema", "") != "arena_install_receipt_v1" ||
                    receipt.value("component_id", "") != id ||
                    receipt.value("component_version", "") != version ||
                    receipt.value("state", "") != expected_state ||
                    artifact_sha256.empty() ||
                    receipt.value("artifact_sha256", "") != artifact_sha256 ||
                    !receipt.contains("active_root") ||
                    normalized_absolute(
                        receipt.at("active_root").get<std::string>()) !=
                        normalized_absolute(target)) {
                    throw std::runtime_error(
                        "exact-current refresh evidence no longer matches: " + id);
                }
                const ValidatedComponentPayload validated =
                    validate_component_payload(target, id, version);
                journal["actions"].push_back(
                    {{"action", "refresh"},
                     {"component_id", id},
                     {"version", version},
                     {"state", "refreshing"},
                     {"target", target.string()}});
                const fs::path receipt_backup =
                    receipts_root / (id + ".rollback-" + transaction_id + ".json");
                fs::copy_file(receipt_path, receipt_backup,
                              fs::copy_options::overwrite_existing);
                journal["actions"].back()["receipt_path"] = receipt_path.string();
                journal["actions"].back()["receipt_backup"] =
                    receipt_backup.string();
                write_json_atomic(journal_path, journal);
                if (validated.manifest.contains("installer_integration")) {
                    const fs::path workspace = provider_workspace(
                        paths, validated.manifest.at("installer_integration"));
                    arena::platform::grant_manager_workspace_access(
                        workspace, paths.manager_service_access);
                }
                receipt["dependencies"] =
                    action.value("dependencies", json::array());
                if (!paths.bundle_owner.empty()) {
                    receipt["bundle_owner"] = paths.bundle_owner;
                }
                receipt["refreshed_at"] = current_utc_iso();
                write_json_atomic(receipt_path, receipt);
                journal["actions"].back()["state"] = "refreshed_current";
                write_json_atomic(journal_path, journal);
                results.push_back(std::move(receipt));
                continue;
            }
            std::optional<json> previous_receipt;
            journal["actions"].push_back(
                {{"component_id", id}, {"version", version}, {"state", "staging"}});
            write_json_atomic(journal_path, journal);
            const fs::path stage = staged_roots.at(id);
            const ValidatedComponentPayload validated =
                validate_component_payload(stage, id, version);
            const json& manifest = validated.manifest;
            const fs::path component_root = components_root / id;
            const fs::path target = component_root / version;
            if (!within(components_root, target)) {
                throw std::runtime_error("component target escapes install root");
            }
            journal["actions"].back()["target"] = target.string();
            write_json_atomic(journal_path, journal);

            if (fs::is_regular_file(receipt_path)) {
                previous_receipt = read_json(receipt_path);
                const bool previous_startup_requires_quiescence =
                    requires_replacement_startup_quiescence(*previous_receipt);
                if (previous_startup_requires_quiescence &&
                    !quiesced_startup_components.contains(id)) {
                    throw std::runtime_error(
                        "installed provider startup escaped transaction-wide quiescence: " +
                        id);
                }
            }

            fs::create_directories(component_root);
            if (fs::exists(target)) {
                if (!repair) {
                    throw std::runtime_error(
                        "component version already exists; use repair or upgrade");
                }
                const fs::path replaced =
                    component_root / (version + ".rollback-" + transaction_id);
                rename_managed_component_tree_with_retry(target, replaced, id);
                journal["actions"].back()["rollback_path"] = replaced.string();
            }
            fs::rename(stage, target);
            journal["actions"].back()["state"] = "target_activated";
            write_json_atomic(journal_path, journal);

            json knowledge_activations =
                plan_knowledge_activation(target, manifest, paths, transaction_id, id);
            json activated_resources = json::array();
            json activated_files = json::array();
            if (!knowledge_activations.empty()) {
                journal["actions"].back()["knowledge_activations"] = knowledge_activations;
                write_json_atomic(journal_path, journal);
                apply_knowledge_activation(
                    knowledge_activations, [&](const json& checkpoint) {
                        journal["actions"].back()["knowledge_activations"] = checkpoint;
                        write_json_atomic(journal_path, journal);
                    });
                activated_resources = receipt_activated_resources(knowledge_activations);
                activated_files = flattened_activated_files(activated_resources);
                json created_locators = json::array();
                for (const auto& resource : activated_resources) {
                    const std::string locator_kind = resource.at("kind").get<std::string>();
                    const fs::path locator_path =
                        paths.data_root / "locators" / (locator_kind + ".json");
                    if (!fs::exists(locator_path)) {
                        const fs::path activation_path =
                            normalized_absolute(resource.at("path").get<std::string>());
                        const json validation =
                            validate_import_locator(locator_kind, activation_path);
                        json locator{
                            {"schema", "arena_saved_locator_v1"},
                            {"schema_version", 1},
                            {"kind", locator_kind},
                            {"mode", "in_place_reference"},
                            {"path", activation_path.string()},
                            {"validation", validation},
                            {"saved_at", current_utc_iso()},
                        };
                        if (locator_kind == "opening_book") {
                            locator["authority_mode"] = "immutable_pinned_snapshot";
                        }
                        write_json_atomic(locator_path, locator);
                        created_locators.push_back(locator_path.string());
                        journal["actions"].back()["created_knowledge_locators"] =
                            created_locators;
                        write_json_atomic(journal_path, journal);
                    }
                }
                journal["actions"].back()["state"] = "knowledge_activated";
                write_json_atomic(journal_path, journal);
            }

            if (fs::is_regular_file(receipt_path)) {
                const fs::path receipt_backup =
                    receipts_root / (id + ".rollback-" + transaction_id + ".json");
                fs::copy_file(receipt_path, receipt_backup, fs::copy_options::overwrite_existing);
                journal["actions"].back()["receipt_backup"] = receipt_backup.string();
            }
            journal["actions"].back()["receipt_path"] = receipt_path.string();
            json data_paths = json::array();
            json ownership = json::array();
            std::string configuration_path;
            if (validated.manifest.contains("installer_integration")) {
                const json& integration =
                    validated.manifest.at("installer_integration");
                const fs::path workspace = provider_workspace(paths, integration);
                if (previous_receipt.has_value()) {
                    const json previous_paths =
                        previous_receipt->value("data_paths", json::array());
                    if (previous_paths.is_array() && previous_paths.size() == 1 &&
                        previous_paths.at(0).is_string() &&
                        normalized_absolute(
                            previous_paths.at(0).get<std::string>()) != workspace) {
                        json& migration =
                            journal["actions"].back()["workspace_migration"];
                        migration = {{"state", "planning"}};
                        write_json_atomic(journal_path, journal);
                        migrate_provider_workspace(
                            paths, *previous_receipt, workspace, transaction_id,
                            migration, [&]() { write_json_atomic(journal_path, journal); });
                        for (const auto& locator :
                             migration.value("locator_writes", json::array())) {
                            migrated_locator_kinds.insert(
                                locator.at("kind").get<std::string>());
                        }
                    }
                }
                const bool enabled = action.value("enabled", true);
                if (enabled ||
                    integration.at("configure_when_disabled").get<bool>()) {
                    fs::create_directories(workspace);
                    if (contains_symbolic_link(workspace)) {
                        throw std::runtime_error(
                            "provider workspace contains a symbolic-link boundary");
                    }
                    arena::platform::grant_manager_workspace_access(
                        workspace, paths.manager_service_access);
                    const fs::path config_path = workspace / "manager-config.json";
                    journal["actions"].back()["configuration_path"] = config_path.string();
                    if (fs::is_regular_file(config_path)) {
                        if (fs::is_symlink(config_path)) {
                            throw std::runtime_error(
                                "provider configuration cannot be a symbolic link");
                        }
                        const fs::path backup =
                            workspace / ("manager-config.rollback-" + transaction_id + ".json");
                        fs::copy_file(config_path, backup, fs::copy_options::overwrite_existing);
                        journal["actions"].back()["configuration_backup"] = backup.string();
                    }
                    const ProviderInstallResult generated =
                        render_provider_installation(
                            paths, validated.manifest, target, workspace, enabled);
                    json generated_writes = json::array();
                    for (const auto& document : generated.generated_documents) {
                        const fs::path relative =
                            fs::path(document.at("path").get<std::string>())
                                .lexically_normal();
                        const fs::path document_path =
                            normalized_absolute(workspace / relative);
                        if (!within(workspace, document_path) ||
                            contains_symbolic_link(document_path.parent_path())) {
                            throw std::runtime_error(
                                "provider generated-document path escapes its workspace");
                        }
                        fs::create_directories(document_path.parent_path());
                        const bool existed = fs::is_regular_file(document_path);
                        json write{{"path", document_path.string()}, {"existed", existed}};
                        if (existed) {
                            if (fs::is_symlink(document_path)) {
                                throw std::runtime_error(
                                    "provider generated document cannot replace a link");
                            }
                            const fs::path backup =
                                document_path.string() + ".rollback-" + transaction_id;
                            fs::copy_file(document_path, backup,
                                          fs::copy_options::overwrite_existing);
                            write["backup"] = backup.string();
                        }
                        generated_writes.push_back(std::move(write));
                        journal["actions"].back()["generated_document_writes"] =
                            generated_writes;
                        write_json_atomic(journal_path, journal);
                        write_json_atomic(document_path, document.at("document"));
                    }
                    write_json_atomic(config_path, generated.configuration);
                    configuration_path = config_path.string();
                }
                data_paths.push_back(workspace.string());
                ownership.push_back({
                    {"path", workspace.string()},
                    {"ownership_class", "user_preserved"},
                    {"normal_uninstall", "preserve"},
                    {"purge", "explicit_recursive"},
                    {"kind", "directory_tree"},
                    {"source", "component_data_path"},
                });
            }
            for (const auto& activated : activated_files) {
                ownership.push_back({
                    {"path", activated.at("path")},
                    {"ownership_class", "generated"},
                    {"normal_uninstall", "preserve"},
                    {"purge", "exact_identity"},
                    {"kind", "file"},
                    {"size_bytes", activated.at("size_bytes")},
                    {"sha256", activated.at("sha256")},
                    {"source", "activated_managed_file"},
                });
            }
            json receipt{
                {"schema", "arena_install_receipt_v1"},
                {"schema_version", 1},
                {"component_id", id},
                {"component_version", version},
                {"state",
                 action.value("enabled", true) ? "installed_managed" : "installed_disabled"},
                {"active_root", target.string()},
                {"artifact_sha256", action.at("artifact").at("sha256")},
                {"installed_at", current_utc_iso()},
                {"dependencies", action.value("dependencies", json::array())},
                {"data_paths", std::move(data_paths)},
                {"ownership", std::move(ownership)},
                {"configuration_path", configuration_path},
                {"activated_files", activated_files},
                {"activated_resources", activated_resources},
            };
            if (!paths.bundle_owner.empty()) {
                receipt["bundle_owner"] = paths.bundle_owner;
            }
            if (validated.service_contract.has_value()) {
                receipt["service_contract"] = *validated.service_contract;
            }
            write_json_atomic(receipt_path, receipt);
            const bool credential_gated_startup =
                validated.service_contract.has_value() &&
                validated.service_contract->value("credential_gated_startup", false);
            if (!quiesced_startup_components.contains(id) &&
                action.value("enabled", true) && !credential_gated_startup &&
                supports_startup_capability(receipt, "startup_install")) {
                const json startup = provider_lifecycle_result_to_json(
                    invoke_provider_lifecycle(
                        paths, receipt, ProviderLifecycleOperation::install));
                journal["actions"].back()["startup_installed"] = true;
                journal["actions"].back()["lifecycle_result"] = startup;
                write_json_atomic(journal_path, journal);
                receipt["startup"] = startup;
                write_json_atomic(receipt_path, receipt);
            }
            journal["actions"].back()["state"] = "committed";
            write_json_atomic(journal_path, journal);
            results.push_back(receipt);
        }
        journal["refreshed_current_components"] = json::array();
        for (const std::string& id : refreshed_component_ids) {
            journal["refreshed_current_components"].push_back(id);
        }
        journal["refreshed_current_consumers"] = refresh_provider_consumers(
            paths,
            [&refreshed_component_ids](const json& receipt, const json&) {
                return refreshed_component_ids.contains(
                    receipt.at("component_id").get<std::string>());
            },
            transaction_id + "-current", &journal["consumer_refresh_writes"],
            [&]() { write_json_atomic(journal_path, journal); });
        write_json_atomic(journal_path, journal);
        journal["refreshed_dependency_consumers"] = refresh_dependency_consumers(
            paths, replaced_component_ids, refreshed_component_ids,
            transaction_id + "-dependencies", &journal["consumer_refresh_writes"],
            [&]() { write_json_atomic(journal_path, journal); });
        write_json_atomic(journal_path, journal);
        for (std::size_t offset = 0;
             offset < journal.at("startup_quiescence").size(); ++offset) {
            const std::size_t index =
                journal.at("startup_quiescence").size() - 1 - offset;
            json& transition = journal["startup_quiescence"][index];
            if (transition.value("state", "") != "quiesced") continue;
            const std::string id = transition.at("component_id").get<std::string>();
            const fs::path receipt_path = receipts_root / (id + ".json");
            if (!fs::is_regular_file(receipt_path)) {
                throw std::runtime_error(
                    "quiesced provider receipt is unavailable for restart: " + id);
            }
            json receipt = read_json(receipt_path);
            transition["state"] = "resume_started";
            write_json_atomic(journal_path, journal);
            ProviderLifecycleResult lifecycle_result;
            if (transition.at("mode") == "pre_replace") {
                lifecycle_result = invoke_provider_lifecycle(
                    paths, receipt, ProviderLifecycleOperation::post_replace,
                    true, {}, {},
                    transition.at("previous_root").get<std::string>(),
                    receipt.at("active_root").get<std::string>());
            } else {
                lifecycle_result = invoke_provider_lifecycle(
                    paths, receipt, ProviderLifecycleOperation::install, true);
            }
            const json startup =
                provider_lifecycle_result_to_json(lifecycle_result);
            receipt["startup"] = startup;
            write_json_atomic(receipt_path, receipt);
            transition["lifecycle_result"] = startup;
            transition["state"] =
                lifecycle_result.disposition == ProviderLifecycleDisposition::ready
                    ? "ready"
                    : "safe_inactive";
            write_json_atomic(journal_path, journal);
            for (auto& result : results) {
                if (result.value("component_id", "") == id) result["startup"] = startup;
            }
        }
        json refreshed_locator_consumers = json::object();
        for (const std::string& kind : migrated_locator_kinds) {
            refreshed_locator_consumers[kind] = refresh_import_consumers(
                paths, kind, transaction_id + "-" + kind,
                &journal["consumer_refresh_writes"],
                [&]() { write_json_atomic(journal_path, journal); });
            journal["refreshed_locator_consumers"] = refreshed_locator_consumers;
            write_json_atomic(journal_path, journal);
        }
        if (paths.restart_manager_service) {
#ifdef _WIN32
            constexpr const char* manager_service = "UCIArenaManager";
#else
            constexpr const char* manager_service = "uci-arena-manager.service";
#endif
            const int restart_result =
                arena::platform::perform_service_action(manager_service, "restart");
            if (restart_result != 0) {
                throw std::runtime_error("Manager service restart did not succeed");
            }
            journal["manager_service_restarted"] = true;
        }
        journal["state"] = "committed";
        write_json_atomic(journal_path, journal);
        try {
            superseded_component_cleanup = cleanup_superseded_component_roots(
                paths, [&](const json& checkpoint) {
                    journal["superseded_component_cleanup"] = checkpoint;
                    write_json_atomic(journal_path, journal);
                });
        } catch (const std::exception& cleanup_error) {
            superseded_component_cleanup["error"] = cleanup_error.what();
            journal["superseded_component_cleanup"] =
                superseded_component_cleanup;
            write_json_atomic(journal_path, journal);
        }
        std::error_code ignored;
        for (const auto& write :
             journal.value("consumer_refresh_writes", json::array())) {
            const std::string backup = write.value("backup", "");
            if (!backup.empty()) {
                fs::remove(backup, ignored);
                ignored.clear();
            }
        }
        for (const auto& action : journal.at("actions")) {
            if (action.contains("rollback_path")) {
                fs::remove_all(action.at("rollback_path").get<std::string>(), ignored);
                ignored.clear();
            }
            if (action.contains("receipt_backup")) {
                fs::remove(action.at("receipt_backup").get<std::string>(), ignored);
                ignored.clear();
            }
            for (const auto& activation :
                 action.value("knowledge_activations", json::array())) {
                for (const auto field : {"backup_path", "candidate_path"}) {
                    const std::string path = activation.value(field, "");
                    if (!path.empty()) {
                        fs::remove_all(path, ignored);
                        ignored.clear();
                    }
                }
            }
            if (action.contains("configuration_backup")) {
                fs::remove(action.at("configuration_backup").get<std::string>(), ignored);
                ignored.clear();
            }
            for (const auto& write :
                 action.value("generated_document_writes", json::array())) {
                const std::string backup = write.value("backup", "");
                if (!backup.empty()) {
                    fs::remove(backup, ignored);
                    ignored.clear();
                }
            }
            if (action.contains("workspace_migration")) {
                for (const auto& locator :
                     action.at("workspace_migration")
                         .value("locator_writes", json::array())) {
                    const std::string backup = locator.value("backup", "");
                    if (!backup.empty()) {
                        fs::remove(backup, ignored);
                        ignored.clear();
                    }
                }
            }
        }
        fs::remove_all(staging_root / transaction_id, ignored);
    } catch (const std::exception& error) {
        std::error_code rollback_error;
        std::string startup_rollback_failure;
        std::string knowledge_rollback_failure;
        std::string component_rollback_failure;
        std::string workspace_rollback_failure;
        const auto append_startup_rollback_failure =
            [&startup_rollback_failure](const std::string& failure) {
                if (!startup_rollback_failure.empty()) {
                    startup_rollback_failure += "; ";
                }
                startup_rollback_failure += failure;
            };
        for (auto& transition : journal["startup_quiescence"]) {
            const std::string transition_state = transition.value("state", "");
            const std::string id = transition.at("component_id").get<std::string>();
            const bool restart_may_be_live = transition_state == "ready" ||
                                             transition_state == "resume_started";
            const bool staged_v2 = transition.at("mode") == "pre_replace" &&
                                   staged_roots.contains(id);
            const fs::path replacement_root =
                transition.at("replacement_root").get<std::string>();
            const bool activated_staged_controller =
                staged_v2 && !fs::exists(staged_roots.at(id)) &&
                fs::exists(replacement_root);
            if (!restart_may_be_live && !activated_staged_controller) continue;
            try {
                if (restart_may_be_live) {
                    const fs::path receipt_path = receipts_root / (id + ".json");
                    const json current = read_json(receipt_path);
                    if (transition.at("mode") == "pre_replace") {
                        const fs::path current_root =
                            current.at("active_root").get<std::string>();
                        static_cast<void>(invoke_provider_lifecycle(
                            paths, current,
                            ProviderLifecycleOperation::pre_replace, true, {}, {},
                            current_root,
                            transition.at("previous_root").get<std::string>()));
                    } else {
                        static_cast<void>(invoke_provider_lifecycle(
                            paths, current,
                            ProviderLifecycleOperation::uninstall, true));
                    }
                }
                if (activated_staged_controller) {
                    const fs::path current_root =
                        transition.at("replacement_root").get<std::string>();
                    const fs::path rollback_control =
                        current_root.string() + ".startup-rollback-" + transaction_id;
                    rename_managed_component_tree_with_retry(
                        current_root, rollback_control, id);
                    transition["rollback_control_root"] =
                        rollback_control.string();
                }
                transition["state"] = "rollback_quiesced";
                write_json_atomic(journal_path, journal);
            } catch (const std::exception& startup_error) {
                transition["rollback_prepare_error"] = startup_error.what();
                transition["state"] = "rollback_failed";
                append_startup_rollback_failure(startup_error.what());
            }
        }
        const json consumer_refresh_writes =
            journal.value("consumer_refresh_writes", json::array());
        for (auto write = consumer_refresh_writes.rbegin();
             write != consumer_refresh_writes.rend(); ++write) {
            if (write->value("state", "") == "restored") continue;
            const fs::path path = write->at("path").get<std::string>();
            const fs::path backup = write->at("backup").get<std::string>();
            fs::remove(path, rollback_error);
            rollback_error.clear();
            if (write->value("existed", false) && fs::is_regular_file(backup)) {
                fs::rename(backup, path, rollback_error);
                if (rollback_error) {
                    workspace_rollback_failure +=
                        (workspace_rollback_failure.empty() ? "" : "; ") +
                        std::string("cannot restore refreshed consumer ") + path.string() +
                        ": " + rollback_error.message();
                    rollback_error.clear();
                }
            } else {
                fs::remove(backup, rollback_error);
                rollback_error.clear();
            }
        }
        for (auto action = journal["actions"].rbegin(); action != journal["actions"].rend();
             ++action) {
            const std::string state = action->value("state", "staging");
            bool preserve_action = false;
            bool action_rollback_failed = false;
            fs::path rollback_startup_control_root;
            bool staged_startup_rollback_prepared = false;
            if (action->value("previous_startup_control", "") ==
                    "staged_pre_replace" &&
                action->contains("previous_startup_receipt") &&
                action->contains("target") &&
                action->contains("staged_startup_control_version")) {
                try {
                    const json& previous = action->at("previous_startup_receipt");
                    const fs::path target = action->at("target").get<std::string>();
                    const fs::path staged =
                        action->at("staged_startup_control_root").get<std::string>();
                    fs::path control_root = fs::is_regular_file(
                                                target / "arena-component.json")
                                                ? target
                                                : staged;
                    static_cast<void>(invoke_provider_lifecycle(
                        paths, previous, ProviderLifecycleOperation::pre_replace,
                        true, control_root,
                        action->at("staged_startup_control_version").get<std::string>(),
                        target,
                        previous.at("active_root").get<std::string>()));
                    if (control_root == target && fs::exists(target)) {
                        rollback_startup_control_root =
                            target.string() + ".startup-rollback-" + transaction_id;
                        fs::remove_all(rollback_startup_control_root, rollback_error);
                        rollback_error.clear();
                        rename_managed_component_tree_with_retry(
                            target, rollback_startup_control_root,
                            action->value("component_id", "unknown"));
                    } else {
                        rollback_startup_control_root = control_root;
                    }
                    staged_startup_rollback_prepared = true;
                    (*action)["startup_rollback_prepared"] = true;
                } catch (const std::exception& startup_error) {
                    action_rollback_failed = true;
                    append_startup_rollback_failure(startup_error.what());
                    (*action)["startup_rollback_prepare_error"] = startup_error.what();
                    (*action)["state"] = "rollback_failed";
                }
            }
            if (action->value("startup_installed", false) && action->contains("receipt_path")) {
                try {
                    const fs::path receipt_path = action->at("receipt_path").get<std::string>();
                    if (fs::is_regular_file(receipt_path)) {
                        static_cast<void>(invoke_provider_lifecycle(
                            paths, read_json(receipt_path),
                            ProviderLifecycleOperation::uninstall));
                    }
                    (*action)["startup_installed"] = false;
                } catch (const std::exception& startup_error) {
                    preserve_action = true;
                    append_startup_rollback_failure(startup_error.what());
                    (*action)["startup_rollback_error"] = startup_error.what();
                    (*action)["state"] = "rollback_failed";
                }
            }
            if (preserve_action) {
                continue;
            }
            if (action->contains("knowledge_activations")) {
                const auto& activations = action->at("knowledge_activations");
                for (auto activation = activations.rbegin(); activation != activations.rend();
                     ++activation) {
                    const fs::path destination = activation->at("path").get<std::string>();
                    const fs::path candidate =
                        activation->at("candidate_path").get<std::string>();
                    const fs::path backup = activation->at("backup_path").get<std::string>();
                    const bool candidate_existed = fs::exists(candidate);
                    fs::remove_all(candidate, rollback_error);
                    if (rollback_error) {
                        knowledge_rollback_failure +=
                            (knowledge_rollback_failure.empty() ? "" : "; ") +
                            std::string("cannot remove candidate ") + candidate.string() + ": " +
                            rollback_error.message();
                        rollback_error.clear();
                    }
                    if (fs::exists(backup)) {
                        fs::remove_all(destination, rollback_error);
                        if (rollback_error) {
                            knowledge_rollback_failure +=
                                (knowledge_rollback_failure.empty() ? "" : "; ") +
                                std::string("cannot clear destination ") + destination.string() +
                                ": " + rollback_error.message();
                            rollback_error.clear();
                            continue;
                        }
                        fs::rename(backup, destination, rollback_error);
                        if (rollback_error) {
                            knowledge_rollback_failure +=
                                (knowledge_rollback_failure.empty() ? "" : "; ") +
                                std::string("cannot restore destination ") + destination.string() +
                                ": " + rollback_error.message();
                            rollback_error.clear();
                        }
                    } else if (!activation->value("had_existing", false) &&
                               (activation->value("state", "planned") == "activated" ||
                                (activation->value("state", "planned") == "prepared" &&
                                 !candidate_existed))) {
                        fs::remove_all(destination, rollback_error);
                        if (rollback_error) {
                            knowledge_rollback_failure +=
                                (knowledge_rollback_failure.empty() ? "" : "; ") +
                                std::string("cannot remove new destination ") +
                                destination.string() + ": " + rollback_error.message();
                            rollback_error.clear();
                        }
                    }
                }
            }
            const json created_locators =
                action->value("created_knowledge_locators", json::array());
            for (auto locator = created_locators.rbegin(); locator != created_locators.rend();
                 ++locator) {
                fs::remove(locator->get<std::string>(), rollback_error);
                rollback_error.clear();
            }
            if (state != "staging" && action->contains("target") &&
                action->value("action", "replace") != "refresh") {
                try {
                    remove_managed_component_tree_with_retry(
                        action->at("target").get<std::string>(),
                        action->value("component_id", "unknown"), "rollback");
                } catch (const std::exception& component_error) {
                    action_rollback_failed = true;
                    component_rollback_failure +=
                        (component_rollback_failure.empty() ? "" : "; ") +
                        std::string(component_error.what());
                    (*action)["component_rollback_error"] = component_error.what();
                }
            }
            if (action->contains("rollback_path") && action->contains("target")) {
                const fs::path rollback = action->at("rollback_path").get<std::string>();
                const fs::path target = action->at("target").get<std::string>();
                if (fs::exists(rollback) && !fs::exists(target)) {
                    try {
                        rename_managed_component_tree_with_retry(
                            rollback, target, action->value("component_id", "unknown"));
                    } catch (const std::exception& component_error) {
                        action_rollback_failed = true;
                        component_rollback_failure +=
                            (component_rollback_failure.empty() ? "" : "; ") +
                            std::string(component_error.what());
                        (*action)["component_rollback_error"] = component_error.what();
                    }
                }
            }
            if (action->contains("receipt_path")) {
                const fs::path receipt = action->at("receipt_path").get<std::string>();
                fs::remove(receipt, rollback_error);
                rollback_error.clear();
                if (action->contains("receipt_backup")) {
                    const fs::path backup = action->at("receipt_backup").get<std::string>();
                    if (fs::exists(backup)) {
                        fs::rename(backup, receipt, rollback_error);
                        rollback_error.clear();
                    }
                }
            }
            if (action->contains("configuration_path")) {
                const fs::path configuration = action->at("configuration_path").get<std::string>();
                fs::remove(configuration, rollback_error);
                rollback_error.clear();
                if (action->contains("configuration_backup")) {
                    const fs::path backup = action->at("configuration_backup").get<std::string>();
                    if (fs::exists(backup)) {
                        fs::rename(backup, configuration, rollback_error);
                        rollback_error.clear();
                    }
                }
            }
            if (action->contains("generated_document_writes")) {
                const auto& writes = action->at("generated_document_writes");
                for (auto write = writes.rbegin(); write != writes.rend(); ++write) {
                    const fs::path path = write->at("path").get<std::string>();
                    fs::remove(path, rollback_error);
                    rollback_error.clear();
                    const std::string backup_value = write->value("backup", "");
                    if (write->value("existed", false) && !backup_value.empty()) {
                        const fs::path backup = backup_value;
                        if (fs::is_regular_file(backup)) {
                            fs::rename(backup, path, rollback_error);
                            rollback_error.clear();
                        }
                    } else if (!backup_value.empty()) {
                        fs::remove(backup_value, rollback_error);
                        rollback_error.clear();
                    }
                }
            }
            if (action->contains("workspace_migration")) {
                const json& migration = action->at("workspace_migration");
                const json locator_writes =
                    migration.value("locator_writes", json::array());
                for (auto locator = locator_writes.rbegin();
                     locator != locator_writes.rend(); ++locator) {
                    const fs::path path = locator->at("path").get<std::string>();
                    const fs::path backup = locator->at("backup").get<std::string>();
                    fs::remove(path, rollback_error);
                    rollback_error.clear();
                    if (fs::is_regular_file(backup)) {
                        fs::rename(backup, path, rollback_error);
                    }
                    if (rollback_error) {
                        workspace_rollback_failure +=
                            (workspace_rollback_failure.empty() ? "" : "; ") +
                            std::string("cannot restore migrated locator ") + path.string() +
                            ": " + rollback_error.message();
                        rollback_error.clear();
                    }
                }
                const std::string source_value = migration.value("source", "");
                const std::string destination_value =
                    migration.value("destination", "");
                if (!source_value.empty() && !destination_value.empty()) {
                    const fs::path source = source_value;
                    const fs::path destination = destination_value;
                    if (!fs::exists(source) && fs::exists(destination)) {
                        fs::create_directories(source.parent_path());
                        fs::rename(destination, source, rollback_error);
                        if (rollback_error) {
                            workspace_rollback_failure +=
                                (workspace_rollback_failure.empty() ? "" : "; ") +
                                std::string("cannot restore provider workspace ") +
                                source.string() + ": " + rollback_error.message();
                            rollback_error.clear();
                        }
                    }
                }
            }
            (*action)["state"] = action_rollback_failed ? "rollback_failed" : "rolled_back";
            if (action->value("previous_startup_transition_started", false) &&
                action->contains("previous_startup_receipt")) {
                try {
                    if (staged_startup_rollback_prepared) {
                        const json& previous = action->at("previous_startup_receipt");
                        const fs::path restored_root =
                            previous.at("active_root").get<std::string>();
                        static_cast<void>(invoke_provider_lifecycle(
                            paths, previous, ProviderLifecycleOperation::post_replace,
                            true,
                            rollback_startup_control_root,
                            action->at("staged_startup_control_version").get<std::string>(),
                            restored_root, restored_root));
                        if (rollback_startup_control_root !=
                            fs::path(action->value("staged_startup_control_root", ""))) {
                            fs::remove_all(rollback_startup_control_root, rollback_error);
                            rollback_error.clear();
                        }
                    } else {
                        static_cast<void>(invoke_provider_lifecycle(
                            paths, action->at("previous_startup_receipt"),
                            ProviderLifecycleOperation::install, true));
                    }
                    (*action)["previous_startup_restored"] = true;
                } catch (const std::exception& startup_error) {
                    append_startup_rollback_failure(startup_error.what());
                    (*action)["previous_startup_restore_error"] = startup_error.what();
                    (*action)["state"] = "rollback_failed";
                }
            }
        }
        for (std::size_t offset = 0;
             offset < journal.at("startup_quiescence").size(); ++offset) {
            const std::size_t index =
                journal.at("startup_quiescence").size() - 1 - offset;
            json& transition = journal["startup_quiescence"][index];
            if (transition.value("state", "") == "restored") continue;
            try {
                const json& previous = transition.at("previous_receipt");
                const std::string id = transition.at("component_id").get<std::string>();
                ProviderLifecycleResult lifecycle_result;
                if (transition.at("mode") == "pre_replace") {
                    fs::path control_root;
                    if (transition.contains("rollback_control_root")) {
                        control_root = transition.at("rollback_control_root")
                                           .get<std::string>();
                    } else if (transition.contains("staged_control_root")) {
                        control_root = transition.at("staged_control_root")
                                           .get<std::string>();
                    }
                    const fs::path restored_root =
                        previous.at("active_root").get<std::string>();
                    lifecycle_result = invoke_provider_lifecycle(
                        paths, previous, ProviderLifecycleOperation::post_replace,
                        true, control_root,
                        control_root.empty()
                            ? std::string{}
                            : transition.at("control_version").get<std::string>(),
                        restored_root, restored_root);
                } else {
                    lifecycle_result = invoke_provider_lifecycle(
                        paths, previous, ProviderLifecycleOperation::install, true);
                }
                const json startup =
                    provider_lifecycle_result_to_json(lifecycle_result);
                json restored_receipt = previous;
                restored_receipt["startup"] = startup;
                write_json_atomic(receipts_root / (id + ".json"), restored_receipt);
                transition["lifecycle_result"] = startup;
                transition["state"] = "restored";
                if (transition.contains("rollback_control_root")) {
                    fs::remove_all(
                        transition.at("rollback_control_root").get<std::string>(),
                        rollback_error);
                    rollback_error.clear();
                }
                write_json_atomic(journal_path, journal);
            } catch (const std::exception& startup_error) {
                transition["restore_error"] = startup_error.what();
                transition["state"] = "rollback_failed";
                append_startup_rollback_failure(startup_error.what());
            }
        }
        fs::remove_all(staging_root / transaction_id, rollback_error);
        std::string rollback_failure;
        if (!startup_rollback_failure.empty()) {
            rollback_failure = "startup: " + startup_rollback_failure;
        }
        if (!knowledge_rollback_failure.empty()) {
            rollback_failure += (rollback_failure.empty() ? "" : "; ") +
                                std::string("knowledge activation: ") +
                                knowledge_rollback_failure;
        }
        if (!component_rollback_failure.empty()) {
            rollback_failure += (rollback_failure.empty() ? "" : "; ") +
                                std::string("component roots: ") +
                                component_rollback_failure;
        }
        if (!workspace_rollback_failure.empty()) {
            rollback_failure += (rollback_failure.empty() ? "" : "; ") +
                                std::string("provider workspace: ") +
                                workspace_rollback_failure;
        }
        journal["state"] = rollback_failure.empty() ? "rolled_back" : "rollback_failed";
        journal["error"] = rollback_failure.empty()
                               ? error.what()
                               : std::string(error.what()) + "; rollback failed: " +
                                     rollback_failure;
        write_json_atomic(journal_path, journal);
        if (!rollback_failure.empty()) {
            throw std::runtime_error(journal.at("error").get<std::string>());
        }
        throw;
    }
    return {
        {"schema", "arena_install_apply_result_v1"},
        {"transaction_id", transaction_id},
        {"state", "committed"},
        {"components", std::move(results)},
        {"superseded_component_cleanup",
         std::move(superseded_component_cleanup)},
    };
}

json SetupEngine::verify(const SetupPaths& paths) {
    json results = json::array();
    bool ok = true;
    for (const auto& receipt : receipt_rows(paths)) {
        if (!receipt.contains("component_id") || !receipt.contains("active_root")) {
            ok = false;
            results.push_back({{"component_id", receipt.value("component_id", "unknown")},
                               {"state", "broken"},
                               {"error", receipt.value("error", "receipt is incomplete")}});
            continue;
        }
        try {
            const ValidatedComponentPayload validated =
                validate_component_payload(
                    receipt.at("active_root").get<std::string>(),
                    receipt.at("component_id").get<std::string>(),
                    receipt.at("component_version").get<std::string>());
            if (validated.service_contract.has_value() != receipt.contains("service_contract") ||
                (validated.service_contract.has_value() &&
                 receipt.at("service_contract") != *validated.service_contract)) {
                throw std::runtime_error(
                    "installed service contract receipt evidence does not match the payload");
            }
            const json expected_resources = expected_receipt_knowledge_resources(
                receipt.at("active_root").get<std::string>(), validated.manifest, paths, receipt);
            const json expected_activated_files =
                flattened_activated_files(expected_resources);
            if (receipt.value("activated_resources", json::array()) != expected_resources ||
                receipt.value("activated_files", json::array()) !=
                    expected_activated_files) {
                throw std::runtime_error(
                    "installed knowledge activation receipt evidence does not match the payload");
            }
            for (const auto& activated : expected_activated_files) {
                const fs::path path = activated.at("path").get<std::string>();
                if (!within(paths.knowledge_root, path) || !fs::is_regular_file(path) ||
                    fs::file_size(path) != integer_field(activated, "size_bytes") ||
                    sha256_file(path) != activated.at("sha256").get<std::string>()) {
                    throw std::runtime_error("activated knowledge file verification failed: " +
                                             path.string());
                }
            }
            const std::string configuration_path = receipt.value("configuration_path", "");
            if (!configuration_path.empty()) {
                const fs::path configuration = normalized_absolute(configuration_path);
                if (!validated.manifest.contains("installer_integration")) {
                    throw std::runtime_error(
                        "generated component configuration has no owning integration contract");
                }
                const fs::path workspace = provider_workspace(
                    paths, validated.manifest.at("installer_integration"));
                if (configuration.parent_path() != workspace ||
                    !fs::is_regular_file(configuration) || contains_symbolic_link(configuration)) {
                    throw std::runtime_error("generated component configuration is unavailable");
                }
                const ProviderInstallResult expected = render_provider_installation(
                    paths, validated.manifest,
                    receipt.at("active_root").get<std::string>(),
                    configuration.parent_path(),
                    receipt.value("state", "") == "installed_managed");
                if (read_json(configuration) != expected.configuration) {
                    throw std::runtime_error("generated component configuration drifted from its "
                                             "verified installer inputs");
                }
                for (const auto& document : expected.generated_documents) {
                    const fs::path generated_path = normalized_absolute(
                        configuration.parent_path() /
                        fs::path(document.at("path").get<std::string>()).lexically_normal());
                    if (!within(configuration.parent_path(), generated_path) ||
                        !fs::is_regular_file(generated_path) ||
                        contains_symbolic_link(generated_path) ||
                        read_json(generated_path) != document.at("document")) {
                        throw std::runtime_error(
                            "provider generated document drifted from its verified inputs");
                    }
                }
            }
            const auto lifecycle = receipt_lifecycle_observation(receipt);
            if (lifecycle && lifecycle->active) {
                verify_provider_service_health(paths, receipt, validated);
            }
            results.push_back(
                {{"component_id", receipt.at("component_id")}, {"state", "verified"}});
        } catch (const std::exception& error) {
            ok = false;
            results.push_back({{"component_id", receipt.at("component_id")},
                               {"state", "broken"},
                               {"error", error.what()}});
        }
    }
    return {
        {"schema", "arena_install_verify_result_v1"},
        {"ok", ok},
        {"components", std::move(results)},
    };
}

json SetupEngine::plan_uninstall(
    const SetupPaths& paths, const std::vector<std::string>& component_ids,
    const bool all) {
    RemovalLifecycleHooks hooks;
    hooks.startup_cleanup_required = [](const json& receipt) {
        return requires_uninstall_startup(receipt);
    };
    return RemovalLifecycle::plan_uninstall(paths, component_ids, all, hooks);
}

json SetupEngine::uninstall(const SetupPaths& paths, const json& plan,
                            const bool confirmed) {
    RemovalLifecycleHooks hooks;
    hooks.startup_cleanup_required = [](const json& receipt) {
        return requires_uninstall_startup(receipt);
    };
    hooks.uninstall_startup = [&paths](const json& receipt) {
        return provider_lifecycle_result_to_json(invoke_provider_lifecycle(
            paths, receipt, ProviderLifecycleOperation::uninstall));
    };
    return RemovalLifecycle::uninstall(paths, plan, confirmed, hooks);
}

json SetupEngine::plan_purge(
    const SetupPaths& paths, const std::vector<std::string>& component_ids,
    const bool all) {
    return RemovalLifecycle::plan_purge(paths, component_ids, all);
}

json SetupEngine::purge(const SetupPaths& paths, const json& plan,
                        const bool confirmed) {
    return RemovalLifecycle::purge(paths, plan, confirmed);
}

json copy_managed_lichess_profile(const SetupPaths& paths, const fs::path& source,
                                  const json& validation) {
    const auto files = lichess_single_game_profile_files(source);
    json identity_rows = json::array();
    for (const auto& file : files) {
        identity_rows.push_back({{"path", file.relative.generic_string()},
                                 {"size_bytes", file.size},
                                 {"sha256", file.sha256}});
    }
    const std::string fingerprint = sha256_text(identity_rows.dump());
    const std::string component_id = "lichess_profile.import." + fingerprint.substr(0, 16);
    const std::string component_version = "1";
    const fs::path import_root = paths.data_root / "imports" / "lichess_profiles";
    const fs::path active_root = import_root / fingerprint.substr(0, 32);
    const fs::path staging_root = import_root / (".s-" + fingerprint.substr(0, 16));
    const fs::path active_api_root = platform::filesystem_api_path(active_root);
    const fs::path staging_api_root = platform::filesystem_api_path(staging_root);
    const bool active_root_existed = fs::exists(active_api_root);
    fs::create_directories(platform::filesystem_api_path(import_root));
    require_no_symbolic_link(import_root);

    std::error_code ignored;
    fs::remove_all(staging_api_root, ignored);
    if (!fs::exists(active_api_root)) {
        fs::create_directories(staging_api_root);
        try {
            for (const auto& file : files) {
                const fs::path target = staging_root / file.relative;
                if (!within(staging_root, target)) {
                    throw std::runtime_error("managed Lichess profile path escapes staging");
                }
                const fs::path api_target = platform::filesystem_api_path(target);
                fs::create_directories(api_target.parent_path());
                fs::copy_file(platform::filesystem_api_path(file.source), api_target,
                              fs::copy_options::overwrite_existing);
                if (fs::file_size(api_target) != file.size ||
                    sha256_file(api_target) != file.sha256) {
                    throw std::runtime_error(
                        "managed Lichess profile closure verification failed");
                }
            }
            const json manifest{
                {"schema", "arena_install_component_v1"},
                {"schema_version", 1},
                {"component_id", component_id},
                {"component_version", component_version},
                {"entrypoints",
                 {{"profile", source.filename().generic_string()}}},
                {"files", identity_rows},
                {"capabilities", {"qualified_lichess_profile"}},
                {"uninstall_policy", "preserve_with_manager_data"},
            };
            write_json_atomic(staging_root / "arena-component.json", manifest);
            fs::rename(staging_api_root, active_api_root);
        } catch (...) {
            fs::remove_all(staging_api_root, ignored);
            throw;
        }
    }
    const json active_manifest =
        validate_component_payload(active_root, component_id, component_version).manifest;
    if (active_manifest.value("files", json::array()) != identity_rows ||
        active_manifest.value("entrypoints", json::object()).value("profile", "") !=
            source.filename().generic_string()) {
        throw std::runtime_error(
            "managed Lichess profile root does not match its complete content identity");
    }
    const fs::path managed_profile = active_root / source.filename();
    const fs::path managed_api_profile = platform::filesystem_api_path(managed_profile);
    if (!fs::is_regular_file(managed_api_profile) ||
        sha256_file(managed_api_profile) != sha256_file(source)) {
        throw std::runtime_error("managed Lichess profile activation verification failed");
    }

    const fs::path receipt_path =
        paths.data_root / "receipts" / (component_id + ".json");
    const bool receipt_existed = fs::is_regular_file(receipt_path);
    json ownership = json::array();
    for (const auto& file : identity_rows) {
        const fs::path path =
            active_root / file.at("path").get<std::string>();
        ownership.push_back({
            {"path", path.string()},
            {"ownership_class", "generated"},
            {"normal_uninstall", "preserve"},
            {"purge", "exact_identity"},
            {"kind", "file"},
            {"size_bytes", file.at("size_bytes")},
            {"sha256", file.at("sha256")},
            {"source", "managed_copy"},
        });
    }
    const fs::path manifest_path = active_root / "arena-component.json";
    const fs::path manifest_api_path = platform::filesystem_api_path(manifest_path);
    ownership.push_back({
        {"path", manifest_path.string()},
        {"ownership_class", "generated"},
        {"normal_uninstall", "preserve"},
        {"purge", "exact_identity"},
        {"kind", "file"},
        {"size_bytes", fs::file_size(manifest_api_path)},
        {"sha256", sha256_file(manifest_api_path)},
        {"source", "managed_copy_manifest"},
    });
    ownership.push_back({
        {"path", active_root.string()},
        {"ownership_class", "generated"},
        {"normal_uninstall", "preserve"},
        {"purge", "empty_only"},
        {"kind", "directory_tree"},
        {"source", "managed_copy_root"},
    });
    const json receipt{
        {"schema", "arena_install_receipt_v1"},
        {"schema_version", 1},
        {"component_id", component_id},
        {"component_version", component_version},
        {"state", "installed_managed"},
        {"import_mode", "managed_copy"},
        {"active_root", active_root.string()},
        {"artifact_sha256", fingerprint},
        {"installed_at", current_utc_iso()},
        {"dependencies", json::array()},
        {"data_paths", json::array()},
        {"ownership", std::move(ownership)},
        {"configuration_path", ""},
        {"activated_files", json::array()},
    };
    write_json_atomic(receipt_path, receipt);
    return {
        {"schema", "arena_setup_import_result_v1"},
        {"schema_version", 1},
        {"kind", "lichess_single_game_profile"},
        {"mode", "managed_copy"},
        {"state", "installed_managed"},
        {"path", managed_profile.string()},
        {"validation", validation},
        {"receipt", receipt},
        {"_active_root_existed", active_root_existed},
        {"_receipt_existed", receipt_existed},
    };
}

} // namespace arena::application::installer
