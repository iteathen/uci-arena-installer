#include "arena/application/installer/removal_lifecycle.hpp"
#include "arena/platform/filesystem_path.hpp"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <cwchar>
#endif

namespace arena::application::installer {
namespace {

using nlohmann::json;
namespace fs = std::filesystem;

bool contains_symbolic_link(const fs::path& value);

json read_json(const fs::path& path) {
    std::ifstream input(platform::filesystem_api_path(path), std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot read JSON file: " + path.string());
    }
    try {
        return json::parse(input);
    } catch (const json::exception& error) {
        throw std::runtime_error("invalid JSON in " + path.string() + ": " +
                                 error.what());
    }
}

void write_json_atomic(const fs::path& path, const json& value) {
    if (contains_symbolic_link(path.parent_path())) {
        throw std::runtime_error("atomic JSON parent contains a symbolic link: " +
                                 path.parent_path().string());
    }
    fs::create_directories(platform::filesystem_api_path(path.parent_path()));
    const fs::path temporary = path.string() + ".tmp";
    if (contains_symbolic_link(temporary)) {
        throw std::runtime_error("atomic JSON staging path contains a symbolic link: " +
                                 temporary.string());
    }
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
            crypto_hash_sha256_update(
                &state, buffer.data(), static_cast<unsigned long long>(count));
        }
    }
    std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
    crypto_hash_sha256_final(&state, digest.data());
    std::array<char, crypto_hash_sha256_BYTES * 2 + 1> encoded{};
    sodium_bin2hex(encoded.data(), encoded.size(), digest.data(), digest.size());
    return encoded.data();
}

bool valid_identifier(const std::string& value) {
    return !value.empty() &&
           std::all_of(value.begin(), value.end(), [](const unsigned char character) {
               return std::isalnum(character) != 0 || character == '.' ||
                      character == '_' || character == '-';
           });
}

bool valid_bundle_owner(const std::string& value) {
    return !value.empty() && value.size() <= 128 &&
           std::all_of(value.begin(), value.end(), [](const unsigned char character) {
               return std::isalnum(character) != 0 || character == '.' ||
                      character == '_' || character == '-' || character == '{' ||
                      character == '}';
           });
}

fs::path normalized_absolute(const fs::path& value) {
    return fs::absolute(value).lexically_normal();
}

bool path_component_equal(const fs::path& left, const fs::path& right) {
#ifdef _WIN32
    return ::_wcsicmp(left.native().c_str(), right.native().c_str()) == 0;
#else
    return left == right;
#endif
}

bool at_or_within(const fs::path& root, const fs::path& candidate) {
    const fs::path normalized_root = normalized_absolute(root);
    const fs::path normalized_candidate = normalized_absolute(candidate);
    auto root_it = normalized_root.begin();
    auto candidate_it = normalized_candidate.begin();
    for (; root_it != normalized_root.end(); ++root_it, ++candidate_it) {
        if (candidate_it == normalized_candidate.end() ||
            !path_component_equal(*root_it, *candidate_it)) {
            return false;
        }
    }
    return true;
}

bool strictly_within(const fs::path& root, const fs::path& candidate) {
    return at_or_within(root, candidate) && !at_or_within(candidate, root);
}

bool same_path(const fs::path& left, const fs::path& right) {
    return at_or_within(left, right) && at_or_within(right, left);
}

bool paths_overlap(const fs::path& left, const fs::path& right) {
    return at_or_within(left, right) || at_or_within(right, left);
}

bool contains_symbolic_link(const fs::path& value) {
    const fs::path absolute = normalized_absolute(value);
    fs::path current = absolute.root_path();
    std::error_code error;
    for (const auto& part : absolute.relative_path()) {
        current /= part;
        const fs::path api_current = platform::filesystem_api_path(current);
        const fs::file_status status = fs::symlink_status(api_current, error);
        if (error) {
            error.clear();
            continue;
        }
        if (fs::is_symlink(status)) {
            return true;
        }
#ifdef _WIN32
        const DWORD attributes = ::GetFileAttributesW(api_current.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            return true;
        }
#endif
    }
    return false;
}

void require_no_symbolic_link(const fs::path& value) {
    if (contains_symbolic_link(value)) {
        throw std::runtime_error("symbolic-link traversal is forbidden: " +
                                 value.string());
    }
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

json receipt_rows(const SetupPaths& paths) {
    json rows = json::array();
    const fs::path directory = paths.data_root / "receipts";
    if (!fs::is_directory(directory) || contains_symbolic_link(directory)) {
        return rows;
    }
    for (const auto& entry : fs::directory_iterator(directory)) {
        if (entry.path().extension() != ".json") {
            continue;
        }
        if (!entry.is_regular_file() || entry.is_symlink()) {
            throw std::runtime_error("component receipt is not a regular file: " +
                                     entry.path().string());
        }
        const json receipt = read_json(entry.path());
        if (!receipt.is_object() ||
            receipt.value("schema", "") != "arena_install_receipt_v1" ||
            receipt.value("schema_version", 0) != 1 ||
            !receipt.contains("component_id") ||
            !receipt.at("component_id").is_string() ||
            !valid_identifier(receipt.at("component_id").get<std::string>()) ||
            !receipt.contains("component_version") ||
            !receipt.at("component_version").is_string() ||
            !receipt.contains("active_root") ||
            !receipt.at("active_root").is_string()) {
            throw std::runtime_error("component receipt identity is invalid: " +
                                     entry.path().string());
        }
        const std::string id = receipt.at("component_id").get<std::string>();
        if (entry.path().filename() != fs::path(id + ".json")) {
            throw std::runtime_error("component receipt filename does not match identity: " +
                                     entry.path().string());
        }
        const json dependencies = receipt.value("dependencies", json::array());
        if (!dependencies.is_array() ||
            std::any_of(dependencies.begin(), dependencies.end(), [](const json& value) {
                return !value.is_string() ||
                       !valid_identifier(value.get<std::string>());
            })) {
            throw std::runtime_error("component receipt dependencies are invalid: " + id);
        }
        if (receipt.contains("bundle_owner") &&
            (!receipt.at("bundle_owner").is_string() ||
             !valid_bundle_owner(receipt.at("bundle_owner").get<std::string>()))) {
            throw std::runtime_error("component receipt bundle owner is invalid: " + id);
        }
        rows.push_back(receipt);
    }
    return rows;
}

void remove_managed_component_tree(const fs::path& active_root,
                                   const std::string& component_id) {
    constexpr int attempts = 100;
    constexpr auto retry_delay = std::chrono::milliseconds(100);
    std::string failure = "path remains present";
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (contains_symbolic_link(active_root)) {
            throw std::runtime_error(
                "managed component root became a symbolic link: " +
                component_id);
        }
        std::error_code remove_error;
        fs::remove_all(platform::filesystem_api_path(active_root), remove_error);
        std::error_code exists_error;
        const bool remains = fs::exists(platform::filesystem_api_path(active_root),
                                        exists_error);
        if (!remove_error && !exists_error && !remains) {
            return;
        }
        failure = remove_error ? remove_error.message()
                               : (exists_error ? exists_error.message() : failure);
        if (attempt + 1 < attempts) {
            std::this_thread::sleep_for(retry_delay);
        }
    }
    throw std::runtime_error("cannot remove managed component " + component_id +
                             " after shutdown: " + failure);
}

bool within_purge_root(const SetupPaths& paths, const fs::path& candidate) {
    if (same_path(paths.data_root, candidate) ||
        same_path(paths.workspace_root, candidate) ||
        same_path(paths.knowledge_root, candidate) ||
        same_path(paths.cache_root, candidate)) {
        return false;
    }
    return strictly_within(paths.data_root, candidate) ||
           strictly_within(paths.workspace_root, candidate) ||
           strictly_within(paths.knowledge_root, candidate) ||
           strictly_within(paths.cache_root, candidate);
}

bool protected_lifecycle_path(const SetupPaths& paths, const fs::path& candidate) {
    return paths_overlap(paths.data_root / "receipts", candidate) ||
           paths_overlap(paths.data_root / "transactions" /
                             "uninstall-preservation",
                         candidate);
}

bool tree_contains_symbolic_link(const fs::path& root) {
    std::error_code error;
    fs::recursive_directory_iterator iterator(
        platform::filesystem_api_path(root), error);
    const fs::recursive_directory_iterator end;
    if (error) {
        throw std::runtime_error("cannot inspect purge directory: " +
                                 root.string() + ": " + error.message());
    }
    while (iterator != end) {
        if (iterator->is_symlink(error) ||
            contains_symbolic_link(iterator->path())) {
            if (error) {
                throw std::runtime_error("cannot inspect purge directory entry: " +
                                         iterator->path().string() + ": " +
                                         error.message());
            }
            return true;
        }
        iterator.increment(error);
        if (error) {
            throw std::runtime_error("cannot inspect purge directory: " +
                                     root.string() + ": " + error.message());
        }
    }
    return false;
}

std::string lifecycle_plan_id(const std::string_view mode) {
    return std::string(mode) + "-" +
           std::to_string(
               std::chrono::system_clock::now().time_since_epoch().count());
}

bool valid_digest(const std::string& value) {
    return value.size() == 64 &&
           std::all_of(value.begin(), value.end(), [](const unsigned char value) {
               return std::isdigit(value) != 0 || (value >= 'a' && value <= 'f');
           });
}

json ownership_entries_from_receipt(const json& receipt) {
    json entries = json::array();
    std::set<std::string, std::less<>> seen;
    auto append = [&](json entry) {
        if (!entry.is_object() || !entry.contains("path") ||
            !entry.at("path").is_string() ||
            entry.value("normal_uninstall", "") != "preserve" ||
            !entry.contains("ownership_class") ||
            !entry.at("ownership_class").is_string() ||
            !entry.contains("purge") || !entry.at("purge").is_string() ||
            !entry.contains("kind") || !entry.at("kind").is_string()) {
            throw std::runtime_error("receipt ownership entry is invalid");
        }
        const std::string ownership_class =
            entry.at("ownership_class").get<std::string>();
        const bool known_ownership =
            ownership_class == "application_replaceable" ||
            ownership_class == "user_preserved" ||
            ownership_class == "generated" || ownership_class == "migrated" ||
            ownership_class == "cache_disposable" ||
            ownership_class == "installer_temporary" ||
            ownership_class == "external_dependency" ||
            ownership_class == "unknown_policy_controlled";
        const std::string purge_policy = entry.at("purge").get<std::string>();
        const std::string kind = entry.at("kind").get<std::string>();
        const bool exact_identity = purge_policy == "exact_identity";
        const bool directory_policy = purge_policy == "explicit_recursive" ||
                                      purge_policy == "empty_only";
        const bool exact_contract =
            !exact_identity ||
            (kind == "file" && entry.contains("size_bytes") &&
             entry.at("size_bytes").is_number_unsigned() &&
             entry.contains("sha256") && entry.at("sha256").is_string() &&
             valid_digest(entry.at("sha256").get<std::string>()));
        if (!known_ownership ||
            (kind != "file" && kind != "directory_tree") ||
            (purge_policy != "forbidden" && !exact_identity &&
             !directory_policy) ||
            !exact_contract || (directory_policy && kind != "directory_tree") ||
            ((ownership_class == "external_dependency" ||
              ownership_class == "unknown_policy_controlled") &&
             purge_policy != "forbidden")) {
            throw std::runtime_error("receipt ownership policy is invalid");
        }
        const fs::path path =
            normalized_absolute(entry.at("path").get<std::string>());
        entry["path"] = path.string();
        const std::string key = path.generic_string() + "\n" + purge_policy +
                                "\n" + kind;
        if (seen.insert(key).second) {
            entries.push_back(std::move(entry));
        }
    };

    if (receipt.contains("ownership")) {
        if (!receipt.at("ownership").is_array()) {
            throw std::runtime_error("receipt ownership contract is invalid");
        }
        for (const auto& entry : receipt.at("ownership")) {
            if (entry.value("normal_uninstall", "") == "preserve") {
                append(entry);
            }
        }
    }
    for (const auto& value : receipt.value("data_paths", json::array())) {
        if (!value.is_string()) {
            throw std::runtime_error("receipt data path is invalid");
        }
        append({{"path", value},
                {"ownership_class", "user_preserved"},
                {"normal_uninstall", "preserve"},
                {"purge", "explicit_recursive"},
                {"kind", "directory_tree"},
                {"source", "component_data_path"}});
    }
    for (const auto& activated :
         receipt.value("activated_files", json::array())) {
        if (!activated.is_object() || !activated.contains("path") ||
            !activated.contains("sha256") ||
            !activated.contains("size_bytes")) {
            throw std::runtime_error("receipt activated-file identity is invalid");
        }
        append({{"path", activated.at("path")},
                {"ownership_class", "generated"},
                {"normal_uninstall", "preserve"},
                {"purge", "exact_identity"},
                {"kind", "file"},
                {"size_bytes", activated.at("size_bytes")},
                {"sha256", activated.at("sha256")},
                {"source", "activated_managed_file"}});
    }
    return entries;
}

fs::path preservation_root(const SetupPaths& paths) {
    return paths.data_root / "transactions" / "uninstall-preservation";
}

fs::path preservation_record_path(const SetupPaths& paths,
                                  const std::string& component_id,
                                  const std::string& receipt_sha256) {
    return preservation_root(paths) /
           (component_id + "-" + receipt_sha256.substr(0, 16) + ".json");
}

json preservation_record(const json& receipt, const std::string& receipt_sha256,
                         const std::string& state) {
    json record{{"schema", "arena_install_uninstall_preservation_v1"},
                {"schema_version", 1},
                {"component_id", receipt.at("component_id")},
                {"component_version", receipt.at("component_version")},
                {"receipt_sha256", receipt_sha256},
                {"authority", "non_authoritative_preservation_evidence"},
                {"state", state},
                {"prepared_at", current_utc_iso()},
                {"entries", ownership_entries_from_receipt(receipt)}};
    if (receipt.contains("bundle_owner")) {
        record["bundle_owner"] = receipt.at("bundle_owner");
    }
    return record;
}

std::map<std::string, std::pair<fs::path, json>, std::less<>>
latest_uninstall_preservation(const SetupPaths& paths) {
    std::map<std::string, std::pair<fs::path, json>, std::less<>> records;
    const fs::path root = preservation_root(paths);
    if (!fs::is_directory(root)) {
        return records;
    }
    require_no_symbolic_link(root);
    for (const auto& entry : fs::directory_iterator(root)) {
        if (!entry.is_regular_file() || entry.is_symlink() ||
            entry.path().extension() != ".json") {
            continue;
        }
        const json record = read_json(entry.path());
        const std::string state = record.value("state", "");
        if (record.value("schema", "") !=
                "arena_install_uninstall_preservation_v1" ||
            record.value("authority", "") !=
                "non_authoritative_preservation_evidence" ||
            !record.contains("component_id") ||
            !record.at("component_id").is_string() ||
            !record.contains("receipt_sha256") ||
            !record.at("receipt_sha256").is_string() ||
            !valid_digest(record.at("receipt_sha256").get<std::string>()) ||
            !record.contains("entries") || !record.at("entries").is_array() ||
            (state != "prepared" && state != "uninstalled" &&
             state != "purged" && state != "purged_with_preserved_files" &&
             state != "purge_failed")) {
            throw std::runtime_error("uninstall preservation evidence is invalid");
        }
        if (state == "prepared") {
            continue;
        }
        const std::string id = record.at("component_id").get<std::string>();
        const fs::path expected_path = preservation_record_path(
            paths, id, record.at("receipt_sha256").get<std::string>());
        if (normalized_absolute(expected_path) !=
            normalized_absolute(entry.path())) {
            throw std::runtime_error(
                "uninstall preservation filename does not match its evidence");
        }
        const auto current = records.find(id);
        if (current == records.end() ||
            record.value("uninstalled_at", "") >=
                current->second.second.value("uninstalled_at", "")) {
            records[id] = {entry.path(), record};
        }
    }
    return records;
}

} // namespace

json RemovalLifecycle::plan_uninstall(
    const SetupPaths& paths, const std::vector<std::string>& component_ids,
    const bool all, const RemovalLifecycleHooks& hooks) {
    if (!hooks.startup_cleanup_required) {
        throw std::runtime_error("uninstall startup classifier is unavailable");
    }
    if (!paths.bundle_owner.empty() && !valid_bundle_owner(paths.bundle_owner)) {
        throw std::runtime_error("bundle owner is invalid");
    }
    std::map<std::string, json, std::less<>> installed;
    for (const auto& receipt : receipt_rows(paths)) {
        if (receipt.value("schema", "") == "arena_install_receipt_v1" &&
            receipt.contains("component_id") &&
            receipt.at("component_id").is_string()) {
            const std::string id =
                receipt.at("component_id").get<std::string>();
            if (!installed.emplace(id, receipt).second) {
                throw std::runtime_error(
                    "duplicate installed component receipt identity: " + id);
            }
        }
    }
    std::vector<std::string> requested = component_ids;
    std::size_t skipped_foreign_receipts = 0;
    if (all) {
        requested.clear();
        for (const auto& [component_id, receipt] : installed) {
            if (!paths.bundle_owner.empty() &&
                receipt.value("bundle_owner", "") != paths.bundle_owner) {
                ++skipped_foreign_receipts;
                continue;
            }
            requested.push_back(component_id);
        }
    }
    if (!paths.bundle_owner.empty() && !all) {
        std::erase_if(
            requested,
            [&installed, &paths,
             &skipped_foreign_receipts](const std::string& component_id) {
                const auto receipt = installed.find(component_id);
                if (receipt == installed.end() ||
                    receipt->second.value("bundle_owner", "") ==
                        paths.bundle_owner) {
                    return false;
                }
                ++skipped_foreign_receipts;
                return true;
            });
    }
    if (requested.empty() && !all && paths.bundle_owner.empty()) {
        throw std::runtime_error(
            "uninstall requires at least one component or --all");
    }
    std::set<std::string, std::less<>> selected;
    std::set<std::string, std::less<>> selected_installed;
    std::vector<std::string> unique;
    for (const auto& id : requested) {
        if (!valid_identifier(id)) {
            throw std::runtime_error("invalid component id " + id);
        }
        if (selected.insert(id).second) {
            unique.push_back(id);
            if (installed.contains(id)) {
                selected_installed.insert(id);
            }
        }
    }
    for (const auto& [consumer_id, receipt] : installed) {
        if (selected.contains(consumer_id)) {
            continue;
        }
        const json dependencies = receipt.value("dependencies", json::array());
        if (!dependencies.is_array()) {
            throw std::runtime_error(
                "installed component dependencies are invalid: " + consumer_id);
        }
        for (const auto& dependency : dependencies) {
            if (dependency.is_string() &&
                selected_installed.contains(dependency.get<std::string>())) {
                throw std::runtime_error("cannot uninstall dependency " +
                                         dependency.get<std::string>() + " while " +
                                         consumer_id + " remains installed");
            }
        }
    }
    std::map<std::string, int, std::less<>> visits;
    std::vector<std::string> ordered;
    std::function<void(const std::string&)> add = [&](const std::string& id) {
        if (visits[id] == 1) {
            throw std::runtime_error("installed component dependency cycle at " + id);
        }
        if (visits[id] == 2) {
            return;
        }
        visits[id] = 1;
        for (const auto& [candidate_id, receipt] : installed) {
            if (!selected.contains(candidate_id)) {
                continue;
            }
            const json dependencies = receipt.value("dependencies", json::array());
            if (std::find(dependencies.begin(), dependencies.end(), id) !=
                dependencies.end()) {
                add(candidate_id);
            }
        }
        visits[id] = 2;
        ordered.push_back(id);
    };
    for (const auto& id : unique) {
        add(id);
    }

    json components = json::array();
    for (const auto& id : ordered) {
        const fs::path receipt_path =
            paths.data_root / "receipts" / (id + ".json");
        if (!fs::is_regular_file(platform::filesystem_api_path(receipt_path))) {
            continue;
        }
        if (contains_symbolic_link(receipt_path)) {
            throw std::runtime_error("receipt path contains a symbolic link: " + id);
        }
        const json receipt = read_json(receipt_path);
        if (receipt.value("component_id", "") != id ||
            !receipt.contains("active_root") ||
            !receipt.at("active_root").is_string()) {
            throw std::runtime_error("installed receipt identity is invalid: " + id);
        }
        static_cast<void>(ownership_entries_from_receipt(receipt));
        const fs::path active_root = normalized_absolute(
            receipt.at("active_root").get<std::string>());
        if (contains_symbolic_link(active_root)) {
            throw std::runtime_error(
                "receipt active root contains a symbolic link: " + id);
        }
        const bool managed_copy =
            receipt.value("import_mode", "") == "managed_copy";
        const bool remove_active_root = strictly_within(
            paths.install_root / "components", active_root);
        if (!remove_active_root &&
            !(managed_copy && within_purge_root(paths, active_root))) {
            throw std::runtime_error(
                "receipt active root is outside an owned component or managed-copy root: " +
                id);
        }
        const std::string receipt_sha256 = sha256_file(receipt_path);
        components.push_back(
            {{"component_id", id},
             {"receipt_path", normalized_absolute(receipt_path).string()},
             {"receipt_sha256", receipt_sha256},
             {"active_root", active_root.string()},
             {"remove_active_root", remove_active_root},
             {"startup_cleanup_required",
              hooks.startup_cleanup_required(receipt)},
             {"preservation_record",
              normalized_absolute(
                  preservation_record_path(paths, id, receipt_sha256))
                  .string()}});
    }
    json plan{{"schema", "arena_install_uninstall_plan_v1"},
              {"schema_version", 1},
              {"plan_id", lifecycle_plan_id("uninstall")},
              {"mode", "uninstall"},
              {"all", all},
              {"skipped_foreign_receipts", skipped_foreign_receipts},
              {"components", std::move(components)},
              {"blockers", json::array()}};
    if (!paths.bundle_owner.empty()) {
        plan["bundle_owner"] = paths.bundle_owner;
    }
    return plan;
}

json RemovalLifecycle::uninstall(const SetupPaths& paths, const json& plan,
                                 const bool confirmed,
                                 const RemovalLifecycleHooks& hooks) {
    if (!confirmed) {
        throw std::runtime_error(
            "uninstall requires explicit confirmation of a reviewed plan");
    }
    if (!hooks.startup_cleanup_required || !hooks.uninstall_startup) {
        throw std::runtime_error("uninstall startup adapter is unavailable");
    }
    if (!plan.is_object() ||
        plan.value("schema", "") != "arena_install_uninstall_plan_v1" ||
        plan.value("mode", "") != "uninstall" ||
        !plan.contains("components") || !plan.at("components").is_array() ||
        !plan.contains("blockers") || !plan.at("blockers").is_array()) {
        throw std::runtime_error("uninstall plan contract is invalid");
    }
    if (!plan.at("blockers").empty()) {
        throw std::runtime_error("uninstall plan contains blockers");
    }
    if (plan.value("bundle_owner", "") != paths.bundle_owner) {
        throw std::runtime_error(
            "uninstall plan bundle owner does not match this invocation");
    }

    json results = json::array();
    for (const auto& component : plan.at("components")) {
        if (!component.is_object() || !component.contains("component_id") ||
            !component.at("component_id").is_string() ||
            !component.contains("receipt_path") ||
            !component.at("receipt_path").is_string() ||
            !component.contains("receipt_sha256") ||
            !component.at("receipt_sha256").is_string() ||
            !component.contains("active_root") ||
            !component.at("active_root").is_string() ||
            !component.contains("remove_active_root") ||
            !component.at("remove_active_root").is_boolean() ||
            !component.contains("startup_cleanup_required") ||
            !component.at("startup_cleanup_required").is_boolean() ||
            !component.contains("preservation_record") ||
            !component.at("preservation_record").is_string()) {
            throw std::runtime_error("uninstall plan component is invalid");
        }
        const std::string id = component.at("component_id").get<std::string>();
        if (!valid_identifier(id)) {
            throw std::runtime_error("uninstall plan component id is invalid");
        }
        const fs::path expected_receipt = normalized_absolute(
            paths.data_root / "receipts" / (id + ".json"));
        const fs::path receipt_path = normalized_absolute(
            component.at("receipt_path").get<std::string>());
        if (receipt_path != expected_receipt ||
            contains_symbolic_link(receipt_path)) {
            throw std::runtime_error(
                "uninstall plan receipt path is outside its contract: " + id);
        }
        if (!fs::is_regular_file(platform::filesystem_api_path(receipt_path))) {
            results.push_back(
                {{"component_id", id}, {"state", "not_installed"}});
            continue;
        }
        if (sha256_file(receipt_path) !=
            component.at("receipt_sha256").get<std::string>()) {
            throw std::runtime_error("uninstall plan is stale for component " + id);
        }
        const json receipt = read_json(receipt_path);
        if (receipt.value("component_id", "") != id) {
            throw std::runtime_error(
                "uninstall receipt identity changed for component " + id);
        }
        const fs::path active_root = normalized_absolute(
            component.at("active_root").get<std::string>());
        if (active_root != normalized_absolute(
                               receipt.at("active_root").get<std::string>())) {
            throw std::runtime_error(
                "uninstall active root changed for component " + id);
        }
        if (contains_symbolic_link(active_root)) {
            throw std::runtime_error(
                "uninstall active root contains a symbolic link for component " + id);
        }
        const bool expected_remove_active_root = strictly_within(
            paths.install_root / "components", active_root);
        const bool managed_copy =
            receipt.value("import_mode", "") == "managed_copy";
        if ((!expected_remove_active_root &&
             !(managed_copy && within_purge_root(paths, active_root))) ||
            component.at("remove_active_root").get<bool>() !=
                expected_remove_active_root ||
            component.at("startup_cleanup_required").get<bool>() !=
                hooks.startup_cleanup_required(receipt)) {
            throw std::runtime_error(
                "uninstall plan decisions do not match receipt ownership for " + id);
        }
        const std::string receipt_sha256 =
            component.at("receipt_sha256").get<std::string>();
        const fs::path evidence_path = normalized_absolute(
            component.at("preservation_record").get<std::string>());
        const fs::path expected_evidence = normalized_absolute(
            preservation_record_path(paths, id, receipt_sha256));
        if (evidence_path != expected_evidence ||
            !strictly_within(preservation_root(paths), evidence_path) ||
            contains_symbolic_link(evidence_path)) {
            throw std::runtime_error(
                "uninstall preservation path is unsafe for component " + id);
        }

        json evidence =
            preservation_record(receipt, receipt_sha256, "prepared");
        write_json_atomic(evidence_path, evidence);
        json startup_cleanup;
        if (component.at("startup_cleanup_required").get<bool>()) {
            startup_cleanup = hooks.uninstall_startup(receipt);
        }
        if (component.at("remove_active_root").get<bool>()) {
            if (!strictly_within(paths.install_root / "components", active_root)) {
                throw std::runtime_error(
                    "uninstall component root is outside installer authority");
            }
            remove_managed_component_tree(active_root, id);
            const fs::path component_directory = active_root.parent_path();
            std::error_code error;
            if (strictly_within(paths.install_root / "components",
                                component_directory) &&
                fs::is_directory(platform::filesystem_api_path(component_directory),
                                 error) &&
                !error &&
                fs::is_empty(platform::filesystem_api_path(component_directory),
                             error) &&
                !error) {
                fs::remove(platform::filesystem_api_path(component_directory), error);
            }
            if (error) {
                throw std::runtime_error(
                    "cannot remove empty component directory for " + id);
            }
        }
        evidence["state"] = "uninstalled";
        evidence["uninstalled_at"] = current_utc_iso();
        write_json_atomic(evidence_path, evidence);
        std::error_code receipt_error;
        if (!fs::remove(platform::filesystem_api_path(receipt_path), receipt_error) ||
            receipt_error) {
            throw std::runtime_error("cannot remove component receipt for " + id);
        }
        json result{{"component_id", id},
                    {"state", "uninstalled"},
                    {"data_preserved", true},
                    {"preservation_record", evidence_path.string()}};
        if (!startup_cleanup.empty()) {
            result["startup_cleanup"] = std::move(startup_cleanup);
        }
        results.push_back(std::move(result));
    }
    json result{{"schema", "arena_install_uninstall_result_v1"},
                {"schema_version", 1},
                {"plan_id", plan.value("plan_id", "")},
                {"all", plan.value("all", false)},
                {"skipped_foreign_receipts",
                 plan.value("skipped_foreign_receipts", std::size_t{0})},
                {"components", std::move(results)}};
    if (!paths.bundle_owner.empty()) {
        result["bundle_owner"] = paths.bundle_owner;
    }
    return result;
}

json RemovalLifecycle::plan_purge(
    const SetupPaths& paths, const std::vector<std::string>& component_ids,
    const bool all) {
    if (!paths.bundle_owner.empty() && !valid_bundle_owner(paths.bundle_owner)) {
        throw std::runtime_error("bundle owner is invalid");
    }
    const auto records = latest_uninstall_preservation(paths);
    std::vector<std::string> requested = component_ids;
    if (all) {
        requested.clear();
        for (const auto& [id, record] : records) {
            if (paths.bundle_owner.empty() ||
                record.second.value("bundle_owner", "") == paths.bundle_owner) {
                requested.push_back(id);
            }
        }
    }
    if (!all && requested.empty()) {
        throw std::runtime_error(
            "purge plan requires at least one component or --all");
    }
    std::set<std::string, std::less<>> selected;
    json components = json::array();
    json blockers = json::array();
    for (const auto& id : requested) {
        if (!valid_identifier(id)) {
            throw std::runtime_error("invalid component id " + id);
        }
        if (!selected.insert(id).second) {
            continue;
        }
        const auto found = records.find(id);
        if (found == records.end()) {
            blockers.push_back(
                {{"component_id", id},
                 {"reason", "no_uninstall_preservation_evidence"}});
            continue;
        }
        const fs::path receipt_path =
            paths.data_root / "receipts" / (id + ".json");
        if (fs::exists(platform::filesystem_api_path(receipt_path))) {
            blockers.push_back(
                {{"component_id", id}, {"reason", "component_is_installed"}});
            continue;
        }
        const fs::path evidence_path = normalized_absolute(found->second.first);
        const json& evidence = found->second.second;
        if (!strictly_within(preservation_root(paths), evidence_path) ||
            contains_symbolic_link(evidence_path)) {
            blockers.push_back(
                {{"component_id", id},
                 {"reason", "unsafe_preservation_evidence_path"}});
            continue;
        }
        if (!paths.bundle_owner.empty() &&
            evidence.value("bundle_owner", "") != paths.bundle_owner) {
            blockers.push_back(
                {{"component_id", id}, {"reason", "foreign_bundle_owner"}});
            continue;
        }
        if (!evidence.contains("entries") || !evidence.at("entries").is_array()) {
            blockers.push_back(
                {{"component_id", id}, {"reason", "invalid_ownership_entries"}});
            continue;
        }
        json operations = json::array();
        json preserved = json::array();
        bool invalid_entry = false;
        for (const auto& entry : evidence.at("entries")) {
            if (!entry.is_object() || !entry.contains("path") ||
                !entry.at("path").is_string()) {
                invalid_entry = true;
                break;
            }
            const std::string purge_policy = entry.value("purge", "forbidden");
            const std::string ownership_class = entry.value(
                "ownership_class", "unknown_policy_controlled");
            if ((ownership_class == "external_dependency" ||
                 ownership_class == "unknown_policy_controlled") &&
                purge_policy != "forbidden") {
                invalid_entry = true;
                break;
            }
            if (purge_policy == "forbidden") {
                json retained = entry;
                retained["reason"] = "ownership_policy_forbids_purge";
                preserved.push_back(std::move(retained));
                continue;
            }
            const fs::path candidate = normalized_absolute(
                entry.at("path").get<std::string>());
            if (!within_purge_root(paths, candidate) ||
                protected_lifecycle_path(paths, candidate) ||
                contains_symbolic_link(candidate)) {
                invalid_entry = true;
                break;
            }
            if (purge_policy != "explicit_recursive" &&
                purge_policy != "exact_identity" &&
                purge_policy != "empty_only") {
                invalid_entry = true;
                break;
            }
            const std::string kind = entry.value("kind", "");
            if ((purge_policy == "exact_identity" &&
                 (kind != "file" || !entry.contains("size_bytes") ||
                  !entry.at("size_bytes").is_number_unsigned() ||
                  !entry.contains("sha256") ||
                  !entry.at("sha256").is_string() ||
                  !valid_digest(entry.at("sha256").get<std::string>()))) ||
                ((purge_policy == "explicit_recursive" ||
                  purge_policy == "empty_only") &&
                 kind != "directory_tree")) {
                invalid_entry = true;
                break;
            }
            if ((purge_policy == "explicit_recursive" ||
                 purge_policy == "empty_only") &&
                fs::is_directory(platform::filesystem_api_path(candidate)) &&
                tree_contains_symbolic_link(candidate)) {
                invalid_entry = true;
                break;
            }
            json operation = entry;
            operation["path"] = candidate.string();
            operations.push_back(std::move(operation));
        }
        if (!invalid_entry) {
            for (const auto& operation : operations) {
                for (const auto& retained : preserved) {
                    if (retained.contains("path") &&
                        retained.at("path").is_string() &&
                        paths_overlap(
                            operation.at("path").get<std::string>(),
                            retained.at("path").get<std::string>())) {
                        invalid_entry = true;
                        break;
                    }
                }
                if (invalid_entry) {
                    break;
                }
            }
        }
        if (invalid_entry) {
            blockers.push_back(
                {{"component_id", id},
                 {"reason", "invalid_or_unbounded_ownership_entry"}});
            continue;
        }
        components.push_back(
            {{"component_id", id},
             {"evidence_path", evidence_path.string()},
             {"evidence_sha256", sha256_file(evidence_path)},
             {"operations", std::move(operations)},
             {"preserved", std::move(preserved)}});
    }
    json plan{{"schema", "arena_install_purge_plan_v1"},
              {"schema_version", 1},
              {"plan_id", lifecycle_plan_id("purge")},
              {"mode", "purge"},
              {"all", all},
              {"components", std::move(components)},
              {"blockers", std::move(blockers)}};
    if (!paths.bundle_owner.empty()) {
        plan["bundle_owner"] = paths.bundle_owner;
    }
    return plan;
}

json RemovalLifecycle::purge(const SetupPaths& paths, const json& plan,
                             const bool confirmed) {
    if (!confirmed) {
        throw std::runtime_error(
            "purge requires explicit confirmation of a reviewed plan");
    }
    if (!plan.is_object() ||
        plan.value("schema", "") != "arena_install_purge_plan_v1" ||
        plan.value("mode", "") != "purge" ||
        !plan.contains("components") || !plan.at("components").is_array() ||
        !plan.contains("blockers") || !plan.at("blockers").is_array()) {
        throw std::runtime_error("purge plan contract is invalid");
    }
    if (!plan.at("blockers").empty()) {
        throw std::runtime_error("purge plan contains blockers");
    }
    if (plan.value("bundle_owner", "") != paths.bundle_owner) {
        throw std::runtime_error(
            "purge plan bundle owner does not match this invocation");
    }

    std::vector<std::string> planned_ids;
    for (const auto& component : plan.at("components")) {
        if (!component.is_object() || !component.contains("component_id") ||
            !component.at("component_id").is_string()) {
            throw std::runtime_error("purge plan component is invalid");
        }
        planned_ids.push_back(component.at("component_id").get<std::string>());
    }
    if (!planned_ids.empty()) {
        const json current_plan = plan_purge(paths, planned_ids, false);
        if (!current_plan.at("blockers").empty() ||
            current_plan.at("components") != plan.at("components")) {
            throw std::runtime_error(
                "purge plan is stale or its ownership projection was modified");
        }
    }

    bool ok = true;
    json component_results = json::array();
    for (const auto& component : plan.at("components")) {
        const std::string id =
            component.at("component_id").get<std::string>();
        const fs::path installed_receipt =
            paths.data_root / "receipts" / (id + ".json");
        if (fs::exists(platform::filesystem_api_path(installed_receipt))) {
            throw std::runtime_error("cannot purge an installed component: " + id);
        }
        const fs::path evidence_path = normalized_absolute(
            component.at("evidence_path").get<std::string>());
        if (!strictly_within(preservation_root(paths), evidence_path) ||
            !fs::is_regular_file(platform::filesystem_api_path(evidence_path)) ||
            contains_symbolic_link(evidence_path) ||
            sha256_file(evidence_path) !=
                component.at("evidence_sha256").get<std::string>()) {
            throw std::runtime_error(
                "purge plan evidence is stale or unsafe for " + id);
        }
        json evidence = read_json(evidence_path);
        if (evidence.value("component_id", "") != id ||
            evidence.value("authority", "") !=
                "non_authoritative_preservation_evidence") {
            throw std::runtime_error(
                "purge evidence identity is invalid for " + id);
        }
        json operations = json::array();
        bool component_ok = true;
        bool preserved_file = false;
        for (const auto& operation : component.at("operations")) {
            json result{{"path", operation.at("path")},
                        {"purge", operation.at("purge")}};
            try {
                const fs::path candidate = normalized_absolute(
                    operation.at("path").get<std::string>());
                if (!within_purge_root(paths, candidate) ||
                    protected_lifecycle_path(paths, candidate) ||
                    contains_symbolic_link(candidate)) {
                    throw std::runtime_error(
                        "purge target is outside bounded ownership or uses a symbolic link");
                }
                const std::string policy =
                    operation.at("purge").get<std::string>();
                std::error_code error;
                const fs::path api_candidate =
                    platform::filesystem_api_path(candidate);
                if (!fs::exists(api_candidate, error)) {
                    if (error) {
                        throw std::runtime_error(
                            "cannot inspect purge target: " + error.message());
                    }
                    result["state"] = "already_absent";
                } else if (policy == "exact_identity") {
                    const bool regular_file = fs::is_regular_file(api_candidate, error);
                    if (error) {
                        throw std::runtime_error(
                            "cannot inspect exact-identity purge target: " +
                            error.message());
                    }
                    if (!regular_file) {
                        result["state"] = "preserved_modified";
                        result["reason"] =
                            "current_type_no_longer_matches_receipt";
                        preserved_file = true;
                    } else if (!operation.contains("size_bytes") ||
                               !operation.contains("sha256")) {
                        throw std::runtime_error(
                            "exact-identity purge target is not a declared regular file");
                    } else {
                        const auto expected_size =
                            operation.at("size_bytes").get<std::uint64_t>();
                        const auto current_size = fs::file_size(api_candidate, error);
                        if (error) {
                            throw std::runtime_error(
                                "cannot inspect exact-identity purge target size: " +
                                error.message());
                        }
                        if (current_size != expected_size ||
                            sha256_file(candidate) !=
                                operation.at("sha256").get<std::string>()) {
                            result["state"] = "preserved_modified";
                            result["reason"] =
                                "current_identity_no_longer_matches_receipt";
                            preserved_file = true;
                        } else if (!fs::remove(api_candidate, error) || error) {
                            throw std::runtime_error(
                                "cannot remove exact-identity purge target");
                        } else {
                            result["state"] = "removed";
                        }
                    }
                } else if (policy == "explicit_recursive") {
                    const bool directory = fs::is_directory(api_candidate, error);
                    if (error) {
                        throw std::runtime_error(
                            "cannot inspect recursive purge target: " +
                            error.message());
                    } else if (!directory) {
                        result["state"] = "preserved_modified";
                        result["reason"] =
                            "current_type_no_longer_matches_receipt";
                        preserved_file = true;
                    } else if (tree_contains_symbolic_link(candidate)) {
                        throw std::runtime_error(
                            "recursive purge target contains a symbolic link");
                    } else {
                        fs::remove_all(api_candidate, error);
                        if (error) {
                            throw std::runtime_error(
                                "cannot remove recursive purge target: " +
                                error.message());
                        }
                        result["state"] = "removed";
                    }
                } else if (policy == "empty_only") {
                    const bool directory = fs::is_directory(api_candidate, error);
                    if (error) {
                        throw std::runtime_error(
                            "cannot inspect empty-only purge target: " +
                            error.message());
                    }
                    if (!directory) {
                        result["state"] = "preserved_modified";
                        result["reason"] =
                            "current_type_no_longer_matches_receipt";
                        preserved_file = true;
                    } else {
                        const bool empty = fs::is_empty(api_candidate, error);
                        if (error) {
                            throw std::runtime_error(
                                "cannot inspect empty-only purge target contents: " +
                                error.message());
                        }
                        if (!empty) {
                            result["state"] = "preserved_nonempty";
                            result["reason"] = "directory_is_not_empty";
                            preserved_file = true;
                        } else if (!fs::remove(api_candidate, error) || error) {
                            throw std::runtime_error(
                                "cannot remove empty purge target");
                        } else {
                            result["state"] = "removed";
                        }
                    }
                } else {
                    throw std::runtime_error(
                        "purge plan contains an unsupported policy");
                }
            } catch (const std::exception& error) {
                component_ok = false;
                ok = false;
                result["state"] = "failed";
                result["error"] = error.what();
            }
            operations.push_back(std::move(result));
        }
        for (const auto& retained :
             component.value("preserved", json::array())) {
            json result = retained;
            result["state"] = "preserved_by_policy";
            operations.push_back(std::move(result));
            preserved_file = true;
        }
        evidence["state"] =
            !component_ok
                ? "purge_failed"
                : (preserved_file ? "purged_with_preserved_files" : "purged");
        evidence["purged_at"] = current_utc_iso();
        evidence["purge_results"] = operations;
        write_json_atomic(evidence_path, evidence);
        component_results.push_back(
            {{"component_id", id},
             {"state", evidence.at("state")},
             {"operations", std::move(operations)},
             {"preservation_record", evidence_path.string()}});
    }
    return {{"schema", "arena_install_purge_result_v1"},
            {"schema_version", 1},
            {"plan_id", plan.value("plan_id", "")},
            {"ok", ok},
            {"components", std::move(component_results)}};
}

} // namespace arena::application::installer
