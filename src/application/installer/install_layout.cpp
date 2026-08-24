#include "arena/application/installer/install_layout.hpp"

#include "arena/application/installer/setup.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace arena::application::installer {
namespace {

namespace fs = std::filesystem;
namespace ports = arena::ports;
using json = nlohmann::json;

constexpr std::array<std::string_view, 2> scope_names{
    "per-user", "per-machine"};
constexpr std::array<std::string_view, 10> root_names{
    "integration", "data", "install", "cache", "workspace",
    "knowledge", "release", "receipts", "transactions", "preservation"};
constexpr std::array<std::string_view, 6> stable_path_names{
    "policy", "descriptor", "scope_marker", "setup_launcher",
    "installer_runtime", "path_directory"};
constexpr std::array<std::string_view, 10> registration_value_names{
    "schema", "scope", "installation_id", "layout_generation",
    "integration_root", "policy_path", "policy_sha256", "descriptor_path",
    "descriptor_sha256", "setup_launcher_path"};

[[noreturn]] void fail(const std::string_view field,
                       const std::string_view reason) {
    constexpr std::size_t maximum_field_bytes = 160;
    std::string message{"invalid install layout "};
    message.append(field.substr(0, maximum_field_bytes));
    message.append(": ");
    message.append(reason);
    throw std::invalid_argument(std::move(message));
}

template <std::size_t Size>
void exact_keys(const json& value,
                const std::array<std::string_view, Size>& expected,
                const std::string_view field) {
    if (!value.is_object()) fail(field, "expected object");
    for (const auto name : expected) {
        if (!value.contains(name)) {
            fail(std::string{field} + "." + std::string{name}, "missing field");
        }
    }
    if (value.size() != expected.size()) fail(field, "unknown field");
}

[[nodiscard]] bool decode_utf8_code_point(const std::string_view text,
                                          std::size_t& offset,
                                          std::uint32_t& code_point) noexcept {
    const auto first = static_cast<unsigned char>(text[offset]);
    std::size_t continuation_count{};
    std::uint32_t minimum{};
    if (first <= 0x7fU) {
        code_point = first;
        ++offset;
        return true;
    }
    if (first >= 0xc2U && first <= 0xdfU) {
        code_point = first & 0x1fU;
        continuation_count = 1;
        minimum = 0x80U;
    } else if (first >= 0xe0U && first <= 0xefU) {
        code_point = first & 0x0fU;
        continuation_count = 2;
        minimum = 0x800U;
    } else if (first >= 0xf0U && first <= 0xf4U) {
        code_point = first & 0x07U;
        continuation_count = 3;
        minimum = 0x10000U;
    } else {
        return false;
    }
    if (continuation_count > text.size() - offset - 1) return false;
    for (std::size_t index = 0; index < continuation_count; ++index) {
        const auto byte = static_cast<unsigned char>(text[offset + index + 1]);
        if ((byte & 0xc0U) != 0x80U) return false;
        code_point = (code_point << 6U) | (byte & 0x3fU);
    }
    offset += continuation_count + 1;
    return code_point >= minimum && code_point <= 0x10ffffU &&
           !(code_point >= 0xd800U && code_point <= 0xdfffU);
}

[[nodiscard]] std::size_t validate_utf8_text(const std::string_view value,
                                             const std::string_view field) {
    std::size_t offset{};
    std::size_t count{};
    while (offset < value.size()) {
        std::uint32_t code_point{};
        if (!decode_utf8_code_point(value, offset, code_point)) {
            fail(field, "invalid UTF-8");
        }
        if (code_point <= 0x1fU ||
            (code_point >= 0x7fU && code_point <= 0x9fU)) {
            fail(field, "control character");
        }
        ++count;
    }
    return count;
}

[[nodiscard]] const std::string& bounded_text(const json& value,
                                              const std::string_view field,
                                              const std::size_t maximum) {
    if (!value.is_string()) fail(field, "expected string");
    const auto& result = value.get_ref<const std::string&>();
    if (result.empty() || validate_utf8_text(result, field) > maximum) {
        fail(field, "expected bounded nonempty string");
    }
    return result;
}

[[nodiscard]] bool ascii_identifier(const std::string_view value) noexcept {
    return std::all_of(value.begin(), value.end(), [](const unsigned char byte) {
        return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
               (byte >= '0' && byte <= '9') || byte == '.' || byte == '_' ||
               byte == '-';
    });
}

[[nodiscard]] bool ascii_alpha(const unsigned char byte) noexcept {
    return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z');
}

[[nodiscard]] bool ascii_alnum(const unsigned char byte) noexcept {
    return ascii_alpha(byte) || (byte >= '0' && byte <= '9');
}

[[nodiscard]] bool registry_value_name(const std::string_view value) noexcept {
    if (value.empty()) return false;
    const auto first = static_cast<unsigned char>(value.front());
    if (!ascii_alpha(first)) return false;
    return std::all_of(value.begin() + 1, value.end(), [](const unsigned char byte) {
        return ascii_alnum(byte) || byte == '_';
    });
}

[[nodiscard]] std::string ascii_lower(std::string value) {
    for (char& byte : value) {
        const auto character = static_cast<unsigned char>(byte);
        if (character >= 'A' && character <= 'Z') {
            byte = static_cast<char>(character + ('a' - 'A'));
        }
    }
    return value;
}

[[nodiscard]] bool reserved_windows_name(const std::string_view value) {
    const auto dot = value.find('.');
    const auto base = ascii_lower(std::string{value.substr(0, dot)});
    if (base == "con" || base == "prn" || base == "aux" || base == "nul") {
        return true;
    }
    return base.size() == 4 &&
           (base.starts_with("com") || base.starts_with("lpt")) &&
           base[3] >= '1' && base[3] <= '9';
}

void validate_windows_component(const std::string_view value,
                                const std::string_view field,
                                const std::size_t maximum) {
    if (value.empty() || validate_utf8_text(value, field) > maximum) {
        fail(field, "invalid Windows path component");
    }
    if (value == "." || value == ".." ||
        value.find_first_of("<>:\"/\\|?*") != std::string_view::npos) {
        fail(field, "unsafe Windows path component");
    }
    if (value.back() == '.' || value.back() == ' ' || reserved_windows_name(value)) {
        fail(field, "reserved Windows path component");
    }
}

void validate_segment(const json& value, const std::string_view field) {
    const auto& segment = bounded_text(value, field, 128);
    validate_windows_component(segment, field, 128);
}

void validate_segments(const json& value, const std::string_view field) {
    if (!value.is_array() || value.size() > 16) fail(field, "invalid segments");
    for (std::size_t index = 0; index < value.size(); ++index) {
        validate_segment(value[index], std::string{field} + "[" +
                                           std::to_string(index) + "]");
    }
}

[[nodiscard]] std::vector<std::string>
relative_path_parts(const json& value, const std::string_view field) {
    const auto& text = bounded_text(value, field, 512);
    if (text.front() == '/' || text.find('\\') != std::string::npos ||
        text.find("//") != std::string::npos ||
        (text.size() >= 2 && ascii_alpha(static_cast<unsigned char>(text[0])) &&
         text[1] == ':')) {
        fail(field, "invalid relative path");
    }
    if (!std::all_of(text.begin(), text.end(), [](const unsigned char byte) {
            return ascii_alnum(byte) || byte == '.' || byte == '_' || byte == '-' ||
                   byte == '/';
        })) {
        fail(field, "invalid relative path");
    }
    std::vector<std::string> result;
    std::size_t start{};
    while (start <= text.size()) {
        const auto end = text.find('/', start);
        const auto part = text.substr(start, end == std::string::npos
                                                ? std::string::npos
                                                : end - start);
        validate_windows_component(part, field, 128);
        result.emplace_back(part);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return result;
}

void validate_registry_subkey(const json& value) {
    constexpr std::string_view field = "registration.subkey";
    const auto& text = bounded_text(value, field, 256);
    if (text != "Software\\UCI Arena\\Installer") {
        fail(field, "unsupported value");
    }
    std::size_t start{};
    while (start <= text.size()) {
        const auto end = text.find('\\', start);
        const auto part = text.substr(start, end == std::string::npos
                                                ? std::string::npos
                                                : end - start);
        if (part.empty() || part.back() == '.' || part.back() == ' ' ||
            !std::all_of(part.begin(), part.end(), [](const unsigned char byte) {
                return ascii_alnum(byte) || byte == ' ' || byte == '.' ||
                       byte == '_' || byte == '-';
            })) {
            fail(field, "invalid registry subkey");
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
}

[[nodiscard]] std::uint64_t positive_integer(const json& value,
                                             const std::string_view field) {
    if (!value.is_number_integer()) fail(field, "expected positive integer");
    if (value.is_number_unsigned()) {
        const auto result = value.get<std::uint64_t>();
        if (result == 0) fail(field, "expected positive integer");
        return result;
    }
    const auto result = value.get<std::int64_t>();
    if (result <= 0) fail(field, "expected positive integer");
    return static_cast<std::uint64_t>(result);
}

void require_integer(const json& value, const std::uint64_t expected,
                     const std::string_view field) {
    if (positive_integer(value, field) != expected) fail(field, "unsupported value");
}

void require_string(const json& value, const std::string_view expected,
                    const std::string_view field) {
    if (!value.is_string() || value.get_ref<const std::string&>() != expected) {
        fail(field, "unsupported value");
    }
}

[[nodiscard]] bool valid_sha256(const std::string_view value) noexcept {
    return value.size() == 64 &&
           std::all_of(value.begin(), value.end(), [](const char byte) {
               return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
           });
}

void require_sha256(const std::string_view value, const std::string_view field) {
    if (!valid_sha256(value)) fail(field, "invalid SHA-256");
}

struct WindowsPath {
    std::string text;
    std::string root_key;
    std::vector<std::string> components;
};

[[nodiscard]] std::string path_text(const fs::path& value) {
    const auto encoded = value.u8string();
    return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

[[nodiscard]] fs::path path_value(const std::string_view value) {
    const auto begin = reinterpret_cast<const char8_t*>(value.data());
    return fs::path{std::u8string{begin, begin + value.size()}};
}

[[nodiscard]] std::vector<std::string>
split_windows_components(const std::string_view value,
                         const std::string_view field) {
    std::vector<std::string> result;
    std::size_t start{};
    while (start <= value.size()) {
        const auto end = value.find('\\', start);
        const auto part = value.substr(start, end == std::string::npos
                                                 ? std::string::npos
                                                 : end - start);
        validate_windows_component(part, field, 32767);
        result.emplace_back(part);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return result;
}

[[nodiscard]] WindowsPath parse_windows_text(const std::string_view value,
                                             const std::string_view field) {
    if (value.empty() || validate_utf8_text(value, field) > 32767 ||
        value.find('/') != std::string_view::npos) {
        fail(field, "expected absolute normalized Windows path");
    }
    WindowsPath result;
    result.text = std::string{value};
    if (value.starts_with("\\\\")) {
        if (value.starts_with("\\\\?\\") || value.starts_with("\\\\.\\")) {
            fail(field, "device namespace is forbidden");
        }
        auto all_components = split_windows_components(value.substr(2), field);
        if (all_components.size() < 3) fail(field, "UNC share root is forbidden");
        result.root_key = "\\\\" + ascii_lower(all_components[0]) + "\\" +
                          ascii_lower(all_components[1]);
        result.components.assign(all_components.begin() + 2, all_components.end());
        return result;
    }
    if (value.size() < 4 ||
        !ascii_alpha(static_cast<unsigned char>(value[0])) || value[1] != ':' ||
        value[2] != '\\') {
        fail(field, "expected absolute drive or UNC path");
    }
    result.root_key = ascii_lower(std::string{value.substr(0, 2)});
    result.components = split_windows_components(value.substr(3), field);
    return result;
}

[[nodiscard]] WindowsPath parse_windows_path(const fs::path& value,
                                             const std::string_view field) {
    std::string encoded;
    try {
        encoded = path_text(value);
    } catch (const std::bad_alloc&) {
        throw;
    } catch (...) {
        fail(field, "invalid path encoding");
    }
    return parse_windows_text(encoded, field);
}

[[nodiscard]] std::string windows_case_key(const std::string_view value) {
    return ascii_lower(std::string{value});
}

[[nodiscard]] bool windows_equal(const WindowsPath& left,
                                 const WindowsPath& right) {
    if (left.root_key != right.root_key ||
        left.components.size() != right.components.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.components.size(); ++index) {
        if (windows_case_key(left.components[index]) !=
            windows_case_key(right.components[index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool windows_descendant(const WindowsPath& parent,
                                      const WindowsPath& candidate) {
    if (parent.root_key != candidate.root_key ||
        parent.components.size() >= candidate.components.size()) {
        return false;
    }
    for (std::size_t index = 0; index < parent.components.size(); ++index) {
        if (windows_case_key(parent.components[index]) !=
            windows_case_key(candidate.components[index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] fs::path join_windows_path(const WindowsPath& parent,
                                         const std::vector<std::string>& parts,
                                         const std::string_view field) {
    if (parts.empty()) fail(field, "path must be a strict descendant");
    std::string joined = parent.text;
    for (const auto& part : parts) {
        joined.push_back('\\');
        joined.append(part);
    }
    const auto parsed = parse_windows_text(joined, field);
    if (!windows_descendant(parent, parsed)) fail(field, "path escapes parent");
    return path_value(parsed.text);
}

[[nodiscard]] std::vector<std::string> json_segments(const json& value) {
    std::vector<std::string> result;
    result.reserve(value.size());
    for (const auto& item : value) result.push_back(item.get<std::string>());
    return result;
}

[[nodiscard]] ports::LayoutAnchor anchor_from_name(const std::string_view name,
                                                   const std::string_view field) {
    if (name == "user_local_app_data") return ports::LayoutAnchor::user_local_app_data;
    if (name == "machine_program_files") return ports::LayoutAnchor::machine_program_files;
    if (name == "machine_program_data") return ports::LayoutAnchor::machine_program_data;
    fail(field, "unknown anchor");
}

[[nodiscard]] std::string_view scope_name(const ports::InstallScope scope) {
    switch (scope) {
    case ports::InstallScope::per_user:
        return "per-user";
    case ports::InstallScope::per_machine:
        return "per-machine";
    }
    fail("scope", "unsupported value");
}

[[nodiscard]] ports::InstallScope scope_from_name(const std::string_view name) {
    if (name == "per-user") return ports::InstallScope::per_user;
    if (name == "per-machine") return ports::InstallScope::per_machine;
    fail("scope", "unsupported value");
}

[[nodiscard]] bool anchor_allowed(const std::string_view scope,
                                  const std::string_view anchor) noexcept {
    if (scope == "per-user") return anchor == "user_local_app_data";
    return anchor == "machine_program_files" || anchor == "machine_program_data";
}

void validate_root_definition(const json& value, const std::string_view field,
                              const bool anchored) {
    if (anchored) {
        exact_keys(value, std::array<std::string_view, 2>{"anchor", "segments"}, field);
        const auto& anchor = bounded_text(value.at("anchor"),
                                          std::string{field} + ".anchor", 64);
        (void)anchor_from_name(anchor, std::string{field} + ".anchor");
    } else {
        exact_keys(value,
                   std::array<std::string_view, 2>{"relative_to", "segments"},
                   field);
        const auto& owner = bounded_text(value.at("relative_to"),
                                         std::string{field} + ".relative_to", 16);
        if (owner != "integration" && owner != "data") {
            fail(std::string{field} + ".relative_to", "unknown root owner");
        }
    }
    validate_segments(value.at("segments"), std::string{field} + ".segments");
}

[[nodiscard]] std::string logical_path_key(const json& roots,
                                           const std::string_view name) {
    const auto& root = roots.at(name);
    std::string result;
    if (name == "integration" || name == "data") {
        result = "@" + root.at("anchor").get<std::string>();
    } else {
        result = logical_path_key(roots, root.at("relative_to").get<std::string>());
    }
    for (const auto& part : root.at("segments")) {
        result.push_back('\\');
        result.append(part.get_ref<const std::string&>());
    }
    return windows_case_key(result);
}

void require_unique(const std::vector<std::string>& values,
                    const std::string_view field) {
    std::vector<std::string> seen;
    seen.reserve(values.size());
    for (const auto& value : values) {
        const auto key = windows_case_key(value);
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) {
            fail(field, "case-insensitive collision");
        }
        seen.push_back(key);
    }
}

void validate_scope_semantics(const json& policy, const std::string_view scope) {
    const auto field = std::string{"scopes."} + std::string{scope};
    const auto& definition = policy.at("scopes").at(scope);
    exact_keys(definition, std::array<std::string_view, 2>{"privilege", "roots"}, field);
    require_string(definition.at("privilege"),
                   scope == "per-user" ? "user" : "machine",
                   field + ".privilege");
    const auto& roots = definition.at("roots");
    exact_keys(roots, root_names, field + ".roots");
    for (const auto name : root_names) {
        validate_root_definition(roots.at(name), field + ".roots." + std::string{name},
                                 name == "integration" || name == "data");
    }
    for (const auto name : {std::string_view{"integration"}, std::string_view{"data"}}) {
        const auto& anchor = roots.at(name).at("anchor").get_ref<const std::string&>();
        if (!anchor_allowed(scope, anchor)) {
            fail(field + ".roots." + std::string{name} + ".anchor", "scope mismatch");
        }
    }

    std::vector<std::string> logical_paths;
    logical_paths.reserve(root_names.size() + stable_path_names.size());
    for (const auto name : root_names) logical_paths.push_back(logical_path_key(roots, name));
    const auto integration_key = logical_path_key(roots, "integration");
    const auto data_key = logical_path_key(roots, "data");
    const auto is_logical_descendant = [](const std::string_view parent,
                                          const std::string_view candidate) {
        return candidate.size() > parent.size() && candidate.starts_with(parent) &&
               candidate[parent.size()] == '\\';
    };
    if (is_logical_descendant(integration_key, data_key) ||
        is_logical_descendant(data_key, integration_key)) {
        fail(field + ".roots", "integration and data overlap");
    }
    const auto integration_prefix = integration_key;
    for (const auto name : stable_path_names) {
        std::string path = integration_prefix;
        for (const auto& part : relative_path_parts(
                 policy.at("stable_paths").at(name),
                 std::string{"stable_paths."} + std::string{name})) {
            path.push_back('\\');
            path.append(part);
        }
        logical_paths.push_back(std::move(path));
    }
    require_unique(logical_paths, field + ".paths");

    if (policy.at("policy_version").get<std::uint64_t>() == 2) {
        const auto& install = roots.at("install");
        if (install.at("relative_to") != "integration" ||
            install.at("segments").size() != 1 ||
            install.at("segments")[0] != "installed") {
            fail(field + ".roots.install", "v2 invariant");
        }
        std::vector<std::string> mutable_leaves;
        for (std::size_t index = 3; index < root_names.size(); ++index) {
            const auto name = root_names[index];
            const auto& root = roots.at(name);
            if (root.at("relative_to") != "data" || root.at("segments").size() != 1) {
                fail(field + ".roots." + std::string{name},
                     "v2 mutable-root invariant");
            }
            mutable_leaves.push_back(root.at("segments")[0].get<std::string>());
        }
        require_unique(mutable_leaves, field + ".roots");
    }
}

[[nodiscard]] json validate_policy(const json& candidate) {
    exact_keys(candidate,
               std::array<std::string_view, 8>{
                   "schema", "schema_version", "policy_id", "policy_version",
                   "platform", "registration", "stable_paths", "scopes"},
               "policy");
    require_string(candidate.at("schema"), "arena_install_layout_policy_v1", "schema");
    require_integer(candidate.at("schema_version"), 1, "schema_version");
    const auto& policy_id = bounded_text(candidate.at("policy_id"), "policy_id", 128);
    if (!ascii_identifier(policy_id)) fail("policy_id", "invalid identifier");
    const auto policy_version = positive_integer(candidate.at("policy_version"),
                                                 "policy_version");
    require_string(candidate.at("platform"), "windows-x86_64", "platform");

    exact_keys(candidate.at("registration"),
               std::array<std::string_view, 2>{"subkey", "values"}, "registration");
    validate_registry_subkey(candidate.at("registration").at("subkey"));
    const auto& registration_values = candidate.at("registration").at("values");
    exact_keys(registration_values, registration_value_names, "registration.values");
    std::vector<std::string> value_names;
    for (const auto name : registration_value_names) {
        const auto field = std::string{"registration.values."} + std::string{name};
        const auto& value = bounded_text(registration_values.at(name), field, 128);
        if (!registry_value_name(value)) fail(field, "invalid value name");
        value_names.push_back(value);
    }
    require_unique(value_names, "registration.values");

    const auto& stable_paths = candidate.at("stable_paths");
    exact_keys(stable_paths, stable_path_names, "stable_paths");
    std::vector<std::string> stable_values;
    for (const auto name : stable_path_names) {
        const auto& value = stable_paths.at(name);
        (void)relative_path_parts(value, std::string{"stable_paths."} + std::string{name});
        stable_values.push_back(value.get<std::string>());
    }
    require_unique(stable_values, "stable_paths");
    const auto expected_policy = "share/uci-arena/layout-policies/" + policy_id +
                                 "-v" + std::to_string(policy_version) + ".json";
    if (stable_paths.at("policy") != expected_policy) {
        fail("stable_paths.policy", "identity mismatch");
    }

    exact_keys(candidate.at("scopes"), scope_names, "scopes");
    for (const auto scope : scope_names) validate_scope_semantics(candidate, scope);
    return candidate;
}

[[nodiscard]] fs::path resolve_anchor(
    const ports::InstallLayoutAnchorResolver& resolver,
    const ports::LayoutAnchor anchor,
    const std::string_view field) {
    fs::path path;
    try {
        path = resolver.resolve(anchor);
    } catch (const std::bad_alloc&) {
        throw;
    } catch (...) {
        fail(field, "anchor resolution failed");
    }
    return path_value(parse_windows_path(path, field).text);
}

[[nodiscard]] fs::path& root_by_name(InstallLayoutRoots& roots,
                                    const std::string_view name) {
    if (name == "integration") return roots.integration;
    if (name == "data") return roots.data;
    if (name == "install") return roots.install;
    if (name == "cache") return roots.cache;
    if (name == "workspace") return roots.workspace;
    if (name == "knowledge") return roots.knowledge;
    if (name == "release") return roots.release;
    if (name == "receipts") return roots.receipts;
    if (name == "transactions") return roots.transactions;
    return roots.preservation;
}

[[nodiscard]] const fs::path& root_by_name(const InstallLayoutRoots& roots,
                                          const std::string_view name) {
    if (name == "integration") return roots.integration;
    if (name == "data") return roots.data;
    if (name == "install") return roots.install;
    if (name == "cache") return roots.cache;
    if (name == "workspace") return roots.workspace;
    if (name == "knowledge") return roots.knowledge;
    if (name == "release") return roots.release;
    if (name == "receipts") return roots.receipts;
    if (name == "transactions") return roots.transactions;
    return roots.preservation;
}

[[nodiscard]] fs::path& stable_by_name(InstallLayoutStablePaths& paths,
                                      const std::string_view name) {
    if (name == "policy") return paths.policy;
    if (name == "descriptor") return paths.descriptor;
    if (name == "scope_marker") return paths.scope_marker;
    if (name == "setup_launcher") return paths.setup_launcher;
    if (name == "installer_runtime") return paths.installer_runtime;
    return paths.path_directory;
}

[[nodiscard]] const fs::path& stable_by_name(const InstallLayoutStablePaths& paths,
                                            const std::string_view name) {
    if (name == "policy") return paths.policy;
    if (name == "descriptor") return paths.descriptor;
    if (name == "scope_marker") return paths.scope_marker;
    if (name == "setup_launcher") return paths.setup_launcher;
    if (name == "installer_runtime") return paths.installer_runtime;
    return paths.path_directory;
}

[[nodiscard]] ResolvedInstallLayout resolve_validated(
    const json& policy, const ports::InstallScope scope,
    const ports::InstallLayoutAnchorResolver& anchors) {
    const auto selected_scope = scope_name(scope);
    const auto& definition = policy.at("scopes").at(selected_scope);
    const auto& definitions = definition.at("roots");

    std::map<ports::LayoutAnchor, fs::path> resolved_anchors;
    for (const auto name : {std::string_view{"integration"}, std::string_view{"data"}}) {
        const auto& anchor_name = definitions.at(name).at("anchor").get_ref<const std::string&>();
        const auto anchor = anchor_from_name(anchor_name, "roots.anchor");
        if (!resolved_anchors.contains(anchor)) {
            resolved_anchors.emplace(
                anchor, resolve_anchor(anchors, anchor,
                                       std::string{"anchors."} + anchor_name));
        }
    }

    ResolvedInstallLayout result;
    result.policy_id = policy.at("policy_id").get<std::string>();
    result.policy_version = policy.at("policy_version").get<std::uint64_t>();
    result.scope = scope;
    for (const auto name : {std::string_view{"integration"}, std::string_view{"data"}}) {
        const auto& definition_root = definitions.at(name);
        const auto anchor = anchor_from_name(
            definition_root.at("anchor").get_ref<const std::string&>(), "roots.anchor");
        root_by_name(result.roots, name) = join_windows_path(
            parse_windows_path(resolved_anchors.at(anchor), "anchor"),
            json_segments(definition_root.at("segments")),
            std::string{"roots."} + std::string{name});
    }
    const auto integration = parse_windows_path(result.roots.integration, "roots.integration");
    const auto data = parse_windows_path(result.roots.data, "roots.data");
    if (windows_equal(integration, data) || windows_descendant(integration, data) ||
        windows_descendant(data, integration)) {
        fail("roots", "integration and data overlap");
    }

    for (std::size_t index = 2; index < root_names.size(); ++index) {
        const auto name = root_names[index];
        const auto& definition_root = definitions.at(name);
        const auto owner = definition_root.at("relative_to").get<std::string>();
        root_by_name(result.roots, name) = join_windows_path(
            parse_windows_path(root_by_name(result.roots, owner), "root owner"),
            json_segments(definition_root.at("segments")),
            std::string{"roots."} + std::string{name});
    }
    for (const auto name : stable_path_names) {
        stable_by_name(result.stable_paths, name) = join_windows_path(
            integration,
            relative_path_parts(policy.at("stable_paths").at(name),
                                std::string{"stable_paths."} + std::string{name}),
            std::string{"stable_paths."} + std::string{name});
    }

    std::vector<std::string> all_paths;
    for (const auto name : root_names) all_paths.push_back(path_text(root_by_name(result.roots, name)));
    for (const auto name : stable_path_names) all_paths.push_back(path_text(stable_by_name(result.stable_paths, name)));
    require_unique(all_paths, "resolved paths");
    return result;
}

[[nodiscard]] bool valid_uuid(const std::string_view value) noexcept {
    if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
        value[18] != '-' || value[23] != '-') {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) continue;
        const char byte = value[index];
        if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f'))) return false;
    }
    return value[14] >= '1' && value[14] <= '8' &&
           (value[19] == '8' || value[19] == '9' || value[19] == 'a' ||
            value[19] == 'b');
}

[[nodiscard]] WindowsPath descriptor_path(const json& value,
                                          const std::string_view field) {
    const auto& text = bounded_text(value, field, 32767);
    return parse_windows_text(text, field);
}

void require_matching_path(const json& value, const fs::path& expected,
                           const std::string_view field) {
    if (!windows_equal(descriptor_path(value, field),
                       parse_windows_path(expected, field))) {
        fail(field, "resolved path mismatch");
    }
}

}  // namespace

json validate_install_layout_policy(const json& candidate) {
    return validate_policy(candidate);
}

ResolvedInstallLayout resolve_install_layout(
    const json& validated_policy, const ports::InstallScope scope,
    const ports::InstallLayoutAnchorResolver& anchors) {
    return resolve_validated(validate_policy(validated_policy), scope, anchors);
}

InstalledLayout validate_installed_layout(
    const json& validated_policy, const std::string_view policy_sha256,
    const json& descriptor, const ports::InstallLayoutAnchorResolver& anchors) {
    const auto policy = validate_policy(validated_policy);
    require_sha256(policy_sha256, "policy_sha256");
    exact_keys(descriptor,
               std::array<std::string_view, 10>{
                   "schema", "schema_version", "installation_id", "layout_generation",
                   "platform", "scope", "policy", "integration_root", "roots",
                   "stable_paths"},
               "descriptor");
    require_string(descriptor.at("schema"), "arena_installed_layout_v1",
                   "descriptor.schema");
    require_integer(descriptor.at("schema_version"), 1,
                    "descriptor.schema_version");
    const auto& installation_id = bounded_text(
        descriptor.at("installation_id"), "descriptor.installation_id", 36);
    if (!valid_uuid(installation_id)) {
        fail("descriptor.installation_id", "invalid UUID");
    }
    const auto generation = positive_integer(descriptor.at("layout_generation"),
                                             "descriptor.layout_generation");
    require_string(descriptor.at("platform"), "windows-x86_64",
                   "descriptor.platform");
    const auto& descriptor_scope = bounded_text(descriptor.at("scope"),
                                                "descriptor.scope", 11);
    const auto scope = scope_from_name(descriptor_scope);
    const auto resolved = resolve_validated(policy, scope, anchors);

    const auto& descriptor_policy = descriptor.at("policy");
    exact_keys(descriptor_policy,
               std::array<std::string_view, 4>{"policy_id", "policy_version", "path", "sha256"},
               "descriptor.policy");
    require_string(descriptor_policy.at("policy_id"), resolved.policy_id,
                   "descriptor.policy.policy_id");
    if (positive_integer(descriptor_policy.at("policy_version"),
                         "descriptor.policy.policy_version") != resolved.policy_version) {
        fail("descriptor.policy.policy_version", "policy mismatch");
    }
    const auto& descriptor_digest = bounded_text(descriptor_policy.at("sha256"),
                                                 "descriptor.policy.sha256", 64);
    require_sha256(descriptor_digest, "descriptor.policy.sha256");
    if (descriptor_digest != policy_sha256) {
        fail("descriptor.policy.sha256", "policy mismatch");
    }
    require_matching_path(descriptor_policy.at("path"), resolved.stable_paths.policy,
                          "descriptor.policy.path");
    require_matching_path(descriptor.at("integration_root"), resolved.roots.integration,
                          "descriptor.integration_root");

    constexpr std::array<std::string_view, 9> descriptor_root_names{
        "data", "install", "cache", "workspace", "knowledge", "release",
        "receipts", "transactions", "preservation"};
    exact_keys(descriptor.at("roots"), descriptor_root_names, "descriptor.roots");
    for (const auto name : descriptor_root_names) {
        require_matching_path(descriptor.at("roots").at(name),
                              root_by_name(resolved.roots, name),
                              std::string{"descriptor.roots."} + std::string{name});
    }
    constexpr std::array<std::string_view, 5> descriptor_stable_names{
        "descriptor", "scope_marker", "setup_launcher", "installer_runtime",
        "path_directory"};
    exact_keys(descriptor.at("stable_paths"), descriptor_stable_names,
               "descriptor.stable_paths");
    for (const auto name : descriptor_stable_names) {
        require_matching_path(descriptor.at("stable_paths").at(name),
                              stable_by_name(resolved.stable_paths, name),
                              std::string{"descriptor.stable_paths."} + std::string{name});
    }

    return {installation_id, generation, std::string{policy_sha256}, resolved};
}

ports::InstallLayoutLocatorRecord locator_from_installed_layout(
    const InstalledLayout& layout, const std::string_view descriptor_sha256) {
    require_sha256(descriptor_sha256, "descriptor_sha256");
    return {
        "arena_install_layout_locator_v1",
        layout.resolved.scope,
        layout.installation_id,
        layout.layout_generation,
        layout.resolved.roots.integration,
        layout.resolved.stable_paths.policy,
        layout.policy_sha256,
        layout.resolved.stable_paths.descriptor,
        std::string{descriptor_sha256},
        layout.resolved.stable_paths.setup_launcher,
    };
}

SetupPaths setup_paths_from_layout(const InstalledLayout& layout,
                                   std::string bundle_owner,
                                   const bool restart_manager_service,
                                   const bool manager_service_access) {
    return {
        layout.resolved.roots.install,
        layout.resolved.roots.data,
        layout.resolved.roots.cache,
        layout.resolved.roots.workspace,
        layout.resolved.roots.knowledge,
        std::move(bundle_owner),
        restart_manager_service,
        manager_service_access,
    };
}

}  // namespace arena::application::installer
