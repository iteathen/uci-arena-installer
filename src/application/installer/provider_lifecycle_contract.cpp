#include "arena/application/installer/provider_lifecycle_contract.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <initializer_list>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace arena::application::installer {
namespace {

using nlohmann::json;
using arena::ports::ProviderLegacyResultEnvelope;
using arena::ports::ProviderLifecycleOperation;
using arena::ports::ProviderLifecycleWireProtocol;

[[noreturn]] void invalid_contract() {
    throw std::invalid_argument("provider lifecycle control contract is invalid");
}

bool exact_object(const json& value,
                  const std::initializer_list<std::string_view> keys) {
    if (!value.is_object() || value.size() != keys.size()) return false;
    return std::ranges::all_of(keys, [&](const std::string_view key) {
        return value.contains(std::string(key));
    });
}

bool valid_utf8(const std::string_view value) noexcept {
    std::size_t index = 0;
    while (index < value.size()) {
        const auto first = static_cast<unsigned char>(value[index]);
        if (first <= 0x7f) {
            ++index;
            continue;
        }
        std::size_t length{};
        std::uint32_t code_point{};
        if ((first & 0xe0) == 0xc0) {
            length = 2;
            code_point = first & 0x1f;
        } else if ((first & 0xf0) == 0xe0) {
            length = 3;
            code_point = first & 0x0f;
        } else if ((first & 0xf8) == 0xf0) {
            length = 4;
            code_point = first & 0x07;
        } else {
            return false;
        }
        if (index + length > value.size()) return false;
        for (std::size_t offset = 1; offset < length; ++offset) {
            const auto next = static_cast<unsigned char>(value[index + offset]);
            if ((next & 0xc0) != 0x80) return false;
            code_point = (code_point << 6) | (next & 0x3f);
        }
        if ((length == 2 && code_point < 0x80) ||
            (length == 3 && code_point < 0x800) ||
            (length == 4 && code_point < 0x10000) ||
            code_point > 0x10ffff ||
            (code_point >= 0xd800 && code_point <= 0xdfff)) {
            return false;
        }
        index += length;
    }
    return true;
}

bool ascii_identifier(const std::string_view value,
                      const std::optional<std::size_t> maximum = std::nullopt) {
    if (value.empty() || (maximum && value.size() > *maximum)) return false;
    return std::ranges::all_of(value, [](const unsigned char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '.' ||
               character == '_' || character == '-';
    });
}

bool legacy_flag(const std::string_view value) {
    return value.size() > 2 && value.starts_with("--") &&
           ascii_identifier(value.substr(2));
}

bool v3_flag(const std::string_view value) {
    if (value.size() < 3 || value.size() > 64 || !value.starts_with("--")) {
        return false;
    }
    const auto first = static_cast<unsigned char>(value[2]);
    if (!std::isalnum(first)) return false;
    return std::ranges::all_of(value.substr(3), [](const unsigned char character) {
        return std::isalnum(character) != 0 || character == '-';
    });
}

bool contains_forbidden_text_byte(const std::string_view value) {
    return value.find('\0') != std::string_view::npos ||
           value.find('\r') != std::string_view::npos ||
           value.find('\n') != std::string_view::npos;
}

bool legacy_relative_path(const std::string& value) {
    if (value == ".") return true;
    if (value.empty() || contains_forbidden_text_byte(value) || !valid_utf8(value)) {
        return false;
    }
    const std::filesystem::path path(value);
    if (path.is_absolute() || path.has_root_name()) return false;
    return std::ranges::none_of(path, [](const std::filesystem::path& part) {
        return part == "." || part == "..";
    });
}

bool v3_relative_path(const std::string& value) {
    if (value.empty() || value.size() > 512 || contains_forbidden_text_byte(value) ||
        !valid_utf8(value)) {
        return false;
    }
    if (value == ".") return true;
    if (value.front() == '/' || value.front() == '\\') return false;
    if (value.size() >= 2 &&
        std::isalpha(static_cast<unsigned char>(value[0])) != 0 && value[1] == ':') {
        return false;
    }
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto separator = value.find_first_of("/\\", start);
        const auto end = separator == std::string::npos ? value.size() : separator;
        if (value.substr(start, end - start) == "..") return false;
        if (separator == std::string::npos) break;
        start = separator + 1;
    }
    return true;
}

std::vector<std::string> legacy_arguments(const json& value) {
    if (!value.is_array() || value.empty() || value.size() > 8) invalid_contract();
    std::vector<std::string> arguments;
    arguments.reserve(value.size());
    for (const auto& argument : value) {
        if (!argument.is_string()) invalid_contract();
        const auto text = argument.get<std::string>();
        if (!ascii_identifier(text)) invalid_contract();
        arguments.push_back(text);
    }
    return arguments;
}

std::vector<std::string> v3_arguments(const json& value) {
    if (!value.is_array() || value.empty() || value.size() > 16) invalid_contract();
    std::vector<std::string> arguments;
    std::set<std::string> unique;
    arguments.reserve(value.size());
    for (const auto& argument : value) {
        if (!argument.is_string()) invalid_contract();
        const auto text = argument.get<std::string>();
        if (text.empty() || text.size() > 128 || contains_forbidden_text_byte(text) ||
            !valid_utf8(text) || !unique.insert(text).second) {
            invalid_contract();
        }
        arguments.push_back(text);
    }
    return arguments;
}

ProviderLegacyResultEnvelope legacy_envelope(const json& result) {
    if (!result.at("envelope").is_string()) invalid_contract();
    const auto envelope = result.at("envelope").get<std::string>();
    if (envelope == "root") return ProviderLegacyResultEnvelope::root;
    if (envelope == "startup") return ProviderLegacyResultEnvelope::startup;
    invalid_contract();
}

struct LegacyCommon {
    std::string entrypoint;
    std::string workspace_flag;
    std::filesystem::path workspace_relative_path;
    std::string configuration_flag;
    ProviderLegacyResultEnvelope result_envelope;
    bool require_configuration_match;
    bool require_live_on_install;
};

LegacyCommon parse_legacy_common(const json& startup_control) {
    if (!startup_control.at("entrypoint").is_string() ||
        !startup_control.at("workspace_argument").is_object() ||
        !startup_control.at("configuration_argument").is_string() ||
        !startup_control.at("result").is_object()) {
        invalid_contract();
    }
    const auto entrypoint = startup_control.at("entrypoint").get<std::string>();
    const auto& workspace = startup_control.at("workspace_argument");
    if (!ascii_identifier(entrypoint) || !exact_object(workspace, {"flag", "relative_path"}) ||
        !workspace.at("flag").is_string() || !workspace.at("relative_path").is_string()) {
        invalid_contract();
    }
    const auto workspace_flag = workspace.at("flag").get<std::string>();
    const auto workspace_relative = workspace.at("relative_path").get<std::string>();
    const auto configuration_flag =
        startup_control.at("configuration_argument").get<std::string>();
    const auto& result = startup_control.at("result");
    if (!legacy_flag(workspace_flag) || !legacy_relative_path(workspace_relative) ||
        !legacy_flag(configuration_flag) ||
        !result.at("require_configuration_match_on_install").is_boolean() ||
        !result.at("require_live_on_install").is_boolean()) {
        invalid_contract();
    }
    return {entrypoint,
            workspace_flag,
            std::filesystem::path(workspace_relative).lexically_normal(),
            configuration_flag,
            legacy_envelope(result),
            result.at("require_configuration_match_on_install").get<bool>(),
            result.at("require_live_on_install").get<bool>()};
}

ProviderLifecycleControl parse_v1(const json& startup_control) {
    if (!exact_object(startup_control,
                      {"schema", "schema_version", "entrypoint", "actions",
                       "workspace_argument", "configuration_argument", "result"}) ||
        !startup_control.at("schema").is_string() ||
        startup_control.at("schema") != "arena_provider_startup_control_v1" ||
        !startup_control.at("schema_version").is_number_integer() ||
        startup_control.at("schema_version") != 1 ||
        !exact_object(startup_control.at("actions"), {"install", "uninstall"}) ||
        !exact_object(startup_control.at("result"),
                      {"envelope", "require_configuration_match_on_install",
                       "require_live_on_install"})) {
        invalid_contract();
    }

    const auto common = parse_legacy_common(startup_control);
    std::map<ProviderLifecycleOperation, ProviderLifecycleOperationControl> operations;
    operations.emplace(
        ProviderLifecycleOperation::install,
        ProviderLifecycleOperationControl{
            legacy_arguments(startup_control.at("actions").at("install")),
            !common.require_live_on_install});
    operations.emplace(
        ProviderLifecycleOperation::uninstall,
        ProviderLifecycleOperationControl{
            legacy_arguments(startup_control.at("actions").at("uninstall")), false});

    return {ProviderLifecycleWireProtocol::legacy_v1,
            common.entrypoint,
            std::move(operations),
            common.workspace_flag,
            common.workspace_relative_path,
            common.configuration_flag,
            std::nullopt,
            std::nullopt,
            common.result_envelope,
            common.require_configuration_match};
}

ProviderLifecycleControl parse_v2(const json& startup_control) {
    if (!exact_object(startup_control,
                      {"schema", "schema_version", "entrypoint", "actions",
                       "workspace_argument", "configuration_argument", "replacement_arguments",
                       "result"}) ||
        !startup_control.at("schema").is_string() ||
        startup_control.at("schema") != "arena_provider_startup_control_v2" ||
        !startup_control.at("schema_version").is_number_integer() ||
        startup_control.at("schema_version") != 2 ||
        !exact_object(startup_control.at("actions"),
                      {"install", "uninstall", "pre_replace", "post_replace"}) ||
        !exact_object(startup_control.at("replacement_arguments"),
                      {"previous_root", "replacement_root"})) {
        invalid_contract();
    }
    const auto& result = startup_control.at("result");
    const bool four_fields =
        exact_object(result,
                     {"envelope", "require_configuration_match_on_install",
                      "require_live_on_install", "require_live_on_post_replace"});
    const bool five_fields =
        exact_object(result,
                     {"envelope", "require_configuration_match_on_install",
                      "require_live_on_install", "require_live_on_post_replace",
                      "allow_deferred_requalification_on_post_replace"});
    if (!four_fields && !five_fields) invalid_contract();

    const auto common = parse_legacy_common(startup_control);
    const auto& replacement = startup_control.at("replacement_arguments");
    if (!replacement.at("previous_root").is_string() ||
        !replacement.at("replacement_root").is_string() ||
        !result.at("require_live_on_post_replace").is_boolean()) {
        invalid_contract();
    }
    const auto previous_root_flag = replacement.at("previous_root").get<std::string>();
    const auto replacement_root_flag = replacement.at("replacement_root").get<std::string>();
    const bool require_live_on_post_replace =
        result.at("require_live_on_post_replace").get<bool>();
    bool allow_deferred = false;
    if (result.contains("allow_deferred_requalification_on_post_replace")) {
        if (!result.at("allow_deferred_requalification_on_post_replace").is_boolean()) {
            invalid_contract();
        }
        allow_deferred =
            result.at("allow_deferred_requalification_on_post_replace").get<bool>();
    }
    if (!legacy_flag(previous_root_flag) || !legacy_flag(replacement_root_flag) ||
        (require_live_on_post_replace && allow_deferred) ||
        (!require_live_on_post_replace && !allow_deferred)) {
        invalid_contract();
    }

    std::map<ProviderLifecycleOperation, ProviderLifecycleOperationControl> operations;
    operations.emplace(
        ProviderLifecycleOperation::install,
        ProviderLifecycleOperationControl{
            legacy_arguments(startup_control.at("actions").at("install")),
            !common.require_live_on_install});
    operations.emplace(
        ProviderLifecycleOperation::uninstall,
        ProviderLifecycleOperationControl{
            legacy_arguments(startup_control.at("actions").at("uninstall")), false});
    operations.emplace(
        ProviderLifecycleOperation::pre_replace,
        ProviderLifecycleOperationControl{
            legacy_arguments(startup_control.at("actions").at("pre_replace")), false});
    operations.emplace(
        ProviderLifecycleOperation::post_replace,
        ProviderLifecycleOperationControl{
            legacy_arguments(startup_control.at("actions").at("post_replace")),
            allow_deferred});

    return {ProviderLifecycleWireProtocol::legacy_v2,
            common.entrypoint,
            std::move(operations),
            common.workspace_flag,
            common.workspace_relative_path,
            common.configuration_flag,
            previous_root_flag,
            replacement_root_flag,
            common.result_envelope,
            common.require_configuration_match};
}

ProviderLifecycleOperationControl parse_v3_operation(const json& operation,
                                                     const bool safe_inactive_allowed) {
    if (!exact_object(operation, {"arguments", "success_dispositions"})) {
        invalid_contract();
    }
    const auto arguments = v3_arguments(operation.at("arguments"));
    const auto& dispositions = operation.at("success_dispositions");
    if (!dispositions.is_array()) invalid_contract();
    bool allow_safe_inactive = false;
    if (dispositions.size() == 1 && dispositions.at(0).is_string() &&
        dispositions.at(0) == "ready") {
        allow_safe_inactive = false;
    } else if (safe_inactive_allowed && dispositions.size() == 2 &&
               dispositions.at(0).is_string() && dispositions.at(0) == "ready" &&
               dispositions.at(1).is_string() && dispositions.at(1) == "safe_inactive") {
        allow_safe_inactive = true;
    } else {
        invalid_contract();
    }
    return {arguments, allow_safe_inactive};
}

ProviderLifecycleControl parse_v3(const json& startup_control) {
    if (!startup_control.is_object() ||
        !startup_control.contains("schema") || !startup_control.at("schema").is_string() ||
        startup_control.at("schema") != "arena_provider_startup_control_v3" ||
        !startup_control.contains("schema_version") ||
        !startup_control.at("schema_version").is_number_integer() ||
        startup_control.at("schema_version") != 3 ||
        !startup_control.contains("entrypoint") ||
        !startup_control.at("entrypoint").is_string() ||
        !startup_control.contains("operations") ||
        !startup_control.at("operations").is_object() ||
        !startup_control.contains("workspace_argument") ||
        !startup_control.contains("configuration_argument")) {
        invalid_contract();
    }

    const auto& operations_json = startup_control.at("operations");
    const bool has_pre_replace = operations_json.contains("pre_replace");
    const bool has_post_replace = operations_json.contains("post_replace");
    if (has_pre_replace != has_post_replace ||
        (has_pre_replace && !exact_object(operations_json,
                                         {"install", "uninstall", "pre_replace",
                                          "post_replace"})) ||
        (!has_pre_replace && !exact_object(operations_json, {"install", "uninstall"}))) {
        invalid_contract();
    }
    const bool has_replacement_arguments = startup_control.contains("replacement_arguments");
    if (has_replacement_arguments != has_pre_replace ||
        (has_replacement_arguments &&
         !exact_object(startup_control,
                       {"schema", "schema_version", "entrypoint", "operations",
                        "workspace_argument", "configuration_argument",
                        "replacement_arguments"})) ||
        (!has_replacement_arguments &&
         !exact_object(startup_control,
                       {"schema", "schema_version", "entrypoint", "operations",
                        "workspace_argument", "configuration_argument"}))) {
        invalid_contract();
    }

    const auto entrypoint = startup_control.at("entrypoint").get<std::string>();
    const auto& workspace = startup_control.at("workspace_argument");
    if (!ascii_identifier(entrypoint, 128) ||
        !exact_object(workspace, {"flag", "relative_path"}) ||
        !workspace.at("flag").is_string() || !workspace.at("relative_path").is_string() ||
        !startup_control.at("configuration_argument").is_string()) {
        invalid_contract();
    }
    const auto workspace_flag = workspace.at("flag").get<std::string>();
    const auto workspace_relative = workspace.at("relative_path").get<std::string>();
    const auto configuration_flag =
        startup_control.at("configuration_argument").get<std::string>();
    if (!v3_flag(workspace_flag) || !v3_relative_path(workspace_relative) ||
        !v3_flag(configuration_flag)) {
        invalid_contract();
    }

    std::map<ProviderLifecycleOperation, ProviderLifecycleOperationControl> operations;
    operations.emplace(ProviderLifecycleOperation::install,
                       parse_v3_operation(operations_json.at("install"), true));
    operations.emplace(ProviderLifecycleOperation::uninstall,
                       parse_v3_operation(operations_json.at("uninstall"), false));

    std::optional<std::string> previous_root_flag;
    std::optional<std::string> replacement_root_flag;
    if (has_pre_replace) {
        operations.emplace(ProviderLifecycleOperation::pre_replace,
                           parse_v3_operation(operations_json.at("pre_replace"), false));
        operations.emplace(ProviderLifecycleOperation::post_replace,
                           parse_v3_operation(operations_json.at("post_replace"), true));
        const auto& replacement = startup_control.at("replacement_arguments");
        if (!exact_object(replacement, {"previous_root", "replacement_root"}) ||
            !replacement.at("previous_root").is_string() ||
            !replacement.at("replacement_root").is_string()) {
            invalid_contract();
        }
        previous_root_flag = replacement.at("previous_root").get<std::string>();
        replacement_root_flag = replacement.at("replacement_root").get<std::string>();
        if (!v3_flag(*previous_root_flag) || !v3_flag(*replacement_root_flag)) {
            invalid_contract();
        }
    }

    return {ProviderLifecycleWireProtocol::typed_v3,
            entrypoint,
            std::move(operations),
            workspace_flag,
            std::filesystem::path(workspace_relative).lexically_normal(),
            configuration_flag,
            std::move(previous_root_flag),
            std::move(replacement_root_flag),
            ProviderLegacyResultEnvelope::root,
            true};
}

} // namespace

ProviderLifecycleControl
parse_provider_lifecycle_control(const nlohmann::json& startup_control) {
    if (!startup_control.is_object() || !startup_control.contains("schema") ||
        !startup_control.at("schema").is_string() ||
        !startup_control.contains("schema_version") ||
        !startup_control.at("schema_version").is_number_integer()) {
        invalid_contract();
    }
    const auto schema = startup_control.at("schema").get<std::string>();
    const auto version = startup_control.at("schema_version").get<int>();
    if (schema == "arena_provider_startup_control_v1" && version == 1) {
        return parse_v1(startup_control);
    }
    if (schema == "arena_provider_startup_control_v2" && version == 2) {
        return parse_v2(startup_control);
    }
    if (schema == "arena_provider_startup_control_v3" && version == 3) {
        return parse_v3(startup_control);
    }
    invalid_contract();
}

} // namespace arena::application::installer
