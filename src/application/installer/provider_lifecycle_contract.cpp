#include "arena/application/installer/provider_lifecycle_contract.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <set>
#include <stdexcept>
#include <string_view>

namespace arena::application::installer {
namespace {
using arena::ports::ProviderLegacyResultEnvelope;
using arena::ports::ProviderLifecycleOperation;
using arena::ports::ProviderLifecycleWireProtocol;

constexpr std::size_t kEntrypointMaximumBytes = 128;
constexpr std::size_t kLegacyArgumentsMaximumCount = 8;
constexpr std::size_t kTypedArgumentsMaximumCount = 16;
constexpr std::size_t kTypedArgumentMaximumBytes = 128;
constexpr std::size_t kFlagMaximumBytes = 64;
constexpr std::size_t kWorkspaceRelativePathMaximumBytes = 512;

[[noreturn]] void invalid_control() {
    throw std::invalid_argument("provider lifecycle control is invalid");
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
            (length == 4 && code_point < 0x10000) || code_point > 0x10ffff ||
            (code_point >= 0xd800 && code_point <= 0xdfff)) {
            return false;
        }
        index += length;
    }
    return true;
}

bool valid_identifier(const std::string_view value, const std::size_t maximum) {
    return !value.empty() && value.size() <= maximum &&
           std::ranges::all_of(value, [](const unsigned char character) {
               return (character >= 'a' && character <= 'z') ||
                      (character >= 'A' && character <= 'Z') ||
                      (character >= '0' && character <= '9') ||
                      character == '.' || character == '_' || character == '-';
           });
}

bool valid_flag(const std::string_view value, const bool typed) {
    if (value.size() < 3 || value.size() > kFlagMaximumBytes ||
        !value.starts_with("--")) {
        return false;
    }
    const auto body = value.substr(2);
    if (!typed) return valid_identifier(body, kFlagMaximumBytes - 2);
    if (!((body.front() >= 'a' && body.front() <= 'z') ||
          (body.front() >= 'A' && body.front() <= 'Z') ||
          (body.front() >= '0' && body.front() <= '9'))) {
        return false;
    }
    return std::ranges::all_of(body.substr(1), [](const unsigned char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '-';
    });
}

bool exact_object(const nlohmann::json& value,
                  const std::initializer_list<std::string_view> keys) {
    if (!value.is_object() || value.size() != keys.size()) return false;
    return std::ranges::all_of(keys, [&](const std::string_view key) {
        return value.contains(std::string{key});
    });
}

bool object_has_only(const nlohmann::json& value,
                     const std::initializer_list<std::string_view> keys) {
    if (!value.is_object()) return false;
    for (auto field = value.begin(); field != value.end(); ++field) {
        if (std::ranges::none_of(keys, [&](const std::string_view key) {
                return field.key() == key;
            })) {
            return false;
        }
    }
    return true;
}

const std::string& required_string(const nlohmann::json& object,
                                   const std::string_view field) {
    const auto key = std::string{field};
    if (!object.contains(key) || !object.at(key).is_string()) invalid_control();
    return object.at(key).get_ref<const std::string&>();
}

bool required_boolean(const nlohmann::json& object, const std::string_view field) {
    const auto key = std::string{field};
    if (!object.contains(key) || !object.at(key).is_boolean()) invalid_control();
    return object.at(key).get<bool>();
}

std::vector<std::string> parse_arguments(const nlohmann::json& value,
                                         const bool typed) {
    const std::size_t maximum_count =
        typed ? kTypedArgumentsMaximumCount : kLegacyArgumentsMaximumCount;
    if (!value.is_array() || value.empty() || value.size() > maximum_count) {
        invalid_control();
    }
    std::vector<std::string> arguments;
    arguments.reserve(value.size());
    std::set<std::string> unique;
    for (const auto& item : value) {
        if (!item.is_string()) invalid_control();
        const auto& argument = item.get_ref<const std::string&>();
        const bool valid = typed
                               ? !argument.empty() &&
                                     argument.size() <= kTypedArgumentMaximumBytes &&
                                     valid_utf8(argument) &&
                                     argument.find('\0') == std::string::npos &&
                                     argument.find('\r') == std::string::npos &&
                                     argument.find('\n') == std::string::npos
                               : valid_identifier(
                                     argument,
                                     arena::ports::provider_lifecycle_argument_max_utf8_bytes);
        if (!valid || !unique.insert(argument).second) invalid_control();
        arguments.push_back(argument);
    }
    return arguments;
}

std::filesystem::path parse_workspace_relative_path(const std::string& value) {
    if (value.empty() || value.size() > kWorkspaceRelativePathMaximumBytes ||
        !valid_utf8(value) || value.find('\0') != std::string::npos ||
        value.find('\r') != std::string::npos ||
        value.find('\n') != std::string::npos ||
        value.front() == '/' || value.front() == '\\' ||
        (value.size() >= 2 &&
         ((value.front() >= 'A' && value.front() <= 'Z') ||
          (value.front() >= 'a' && value.front() <= 'z')) &&
         value[1] == ':')) {
        invalid_control();
    }
    std::string portable = value;
    std::ranges::replace(portable, '\\', '/');
    std::size_t start = 0;
    while (start <= portable.size()) {
        const auto separator = portable.find('/', start);
        const auto component = portable.substr(
            start, separator == std::string::npos ? std::string::npos : separator - start);
        if (component == "..") invalid_control();
        if (separator == std::string::npos) break;
        start = separator + 1;
    }
    const std::u8string encoded{
        reinterpret_cast<const char8_t*>(portable.data()), portable.size()};
    return std::filesystem::path{encoded}.lexically_normal();
}

ProviderLegacyResultEnvelope parse_legacy_envelope(const nlohmann::json& result) {
    const auto& envelope = required_string(result, "envelope");
    if (envelope == "root") return ProviderLegacyResultEnvelope::root;
    if (envelope == "startup") return ProviderLegacyResultEnvelope::startup;
    invalid_control();
}

ProviderLifecycleControl parse_legacy(const nlohmann::json& control,
                                      const bool version_two) {
    const auto root_keys = version_two
                               ? std::initializer_list<std::string_view>{
                                     "schema", "schema_version", "entrypoint", "actions",
                                     "workspace_argument", "configuration_argument",
                                     "replacement_arguments", "result"}
                               : std::initializer_list<std::string_view>{
                                     "schema", "schema_version", "entrypoint", "actions",
                                     "workspace_argument", "configuration_argument", "result"};
    if (!exact_object(control, root_keys)) invalid_control();

    const auto& entrypoint = required_string(control, "entrypoint");
    if (!valid_identifier(entrypoint, kEntrypointMaximumBytes)) invalid_control();
    const auto& actions = control.at("actions");
    const bool exact_actions = version_two
                                   ? exact_object(actions, {"install", "uninstall",
                                                            "pre_replace", "post_replace"})
                                   : exact_object(actions, {"install", "uninstall"});
    if (!exact_actions) invalid_control();

    ProviderLifecycleControl parsed{
        version_two ? ProviderLifecycleWireProtocol::legacy_v2
                    : ProviderLifecycleWireProtocol::legacy_v1,
        entrypoint,
        {},
        {},
        {},
        {},
        std::nullopt,
        std::nullopt,
        ProviderLegacyResultEnvelope::root,
        false};

    const std::array required_operations{
        ProviderLifecycleOperation::install, ProviderLifecycleOperation::uninstall};
    const std::array required_names{"install", "uninstall"};
    for (std::size_t index = 0; index < required_operations.size(); ++index) {
        parsed.operations.emplace(
            required_operations[index],
            ProviderLifecycleOperationControl{
                parse_arguments(actions.at(required_names[index]), false), false});
    }
    if (version_two) {
        parsed.operations.emplace(
            ProviderLifecycleOperation::pre_replace,
            ProviderLifecycleOperationControl{
                parse_arguments(actions.at("pre_replace"), false), false});
        parsed.operations.emplace(
            ProviderLifecycleOperation::post_replace,
            ProviderLifecycleOperationControl{
                parse_arguments(actions.at("post_replace"), false), false});
    }

    const auto& workspace = control.at("workspace_argument");
    if (!exact_object(workspace, {"flag", "relative_path"})) invalid_control();
    parsed.workspace_flag = required_string(workspace, "flag");
    parsed.workspace_relative_path =
        parse_workspace_relative_path(required_string(workspace, "relative_path"));
    parsed.configuration_flag = required_string(control, "configuration_argument");
    if (!valid_flag(parsed.workspace_flag, false) ||
        !valid_flag(parsed.configuration_flag, false)) {
        invalid_control();
    }

    const auto& result = control.at("result");
    if (version_two) {
        if (!object_has_only(result,
                             {"envelope", "require_configuration_match_on_install",
                              "require_live_on_install", "require_live_on_post_replace",
                              "allow_deferred_requalification_on_post_replace"}) ||
            result.size() < 4 || result.size() > 5) {
            invalid_control();
        }
    } else if (!exact_object(
                   result, {"envelope", "require_configuration_match_on_install",
                            "require_live_on_install"})) {
        invalid_control();
    }
    parsed.legacy_envelope = parse_legacy_envelope(result);
    parsed.legacy_require_configuration_match =
        required_boolean(result, "require_configuration_match_on_install");
    parsed.operations.at(ProviderLifecycleOperation::install).allow_safe_inactive =
        !required_boolean(result, "require_live_on_install");

    if (version_two) {
        const auto& replacement = control.at("replacement_arguments");
        if (!exact_object(replacement, {"previous_root", "replacement_root"})) {
            invalid_control();
        }
        parsed.previous_root_flag = required_string(replacement, "previous_root");
        parsed.replacement_root_flag = required_string(replacement, "replacement_root");
        if (!valid_flag(*parsed.previous_root_flag, false) ||
            !valid_flag(*parsed.replacement_root_flag, false)) {
            invalid_control();
        }

        const bool require_live =
            required_boolean(result, "require_live_on_post_replace");
        bool allow_deferred = false;
        if (result.contains("allow_deferred_requalification_on_post_replace")) {
            allow_deferred = required_boolean(
                result, "allow_deferred_requalification_on_post_replace");
        }
        if (require_live == allow_deferred) invalid_control();
        parsed.operations.at(ProviderLifecycleOperation::post_replace)
            .allow_safe_inactive = allow_deferred;
    }
    return parsed;
}

bool parse_success_dispositions(const nlohmann::json& value,
                                const bool safe_inactive_permitted) {
    if (!value.is_array() || value.empty() || value.size() > 2 ||
        !value.front().is_string() || value.front() != "ready") {
        invalid_control();
    }
    if (value.size() == 1) return false;
    if (!safe_inactive_permitted || !value.at(1).is_string() ||
        value.at(1) != "safe_inactive") {
        invalid_control();
    }
    return true;
}

ProviderLifecycleOperationControl parse_typed_operation(
    const nlohmann::json& value, const bool safe_inactive_permitted) {
    if (!exact_object(value, {"arguments", "success_dispositions"})) {
        invalid_control();
    }
    return {parse_arguments(value.at("arguments"), true),
            parse_success_dispositions(value.at("success_dispositions"),
                                       safe_inactive_permitted)};
}

ProviderLifecycleControl parse_typed(const nlohmann::json& control) {
    if (!object_has_only(control,
                         {"schema", "schema_version", "entrypoint", "operations",
                          "workspace_argument", "configuration_argument",
                          "replacement_arguments"}) ||
        control.size() < 6 || control.size() > 7 ||
        !control.contains("entrypoint") || !control.contains("operations") ||
        !control.contains("workspace_argument") ||
        !control.contains("configuration_argument")) {
        invalid_control();
    }
    const auto& entrypoint = required_string(control, "entrypoint");
    if (!valid_identifier(entrypoint, kEntrypointMaximumBytes)) invalid_control();
    const auto& operations = control.at("operations");
    if (!object_has_only(
            operations, {"install", "uninstall", "pre_replace", "post_replace"}) ||
        !operations.contains("install") || !operations.contains("uninstall")) {
        invalid_control();
    }
    const bool has_pre_replace = operations.contains("pre_replace");
    const bool has_post_replace = operations.contains("post_replace");
    if (has_pre_replace != has_post_replace ||
        operations.size() != (has_pre_replace ? 4 : 2) ||
        control.contains("replacement_arguments") != has_pre_replace) {
        invalid_control();
    }

    ProviderLifecycleControl parsed{
        ProviderLifecycleWireProtocol::typed_v3,
        entrypoint,
        {},
        {},
        {},
        {},
        std::nullopt,
        std::nullopt,
        ProviderLegacyResultEnvelope::root,
        true};
    parsed.operations.emplace(
        ProviderLifecycleOperation::install,
        parse_typed_operation(operations.at("install"), true));
    parsed.operations.emplace(
        ProviderLifecycleOperation::uninstall,
        parse_typed_operation(operations.at("uninstall"), false));
    if (has_pre_replace) {
        parsed.operations.emplace(
            ProviderLifecycleOperation::pre_replace,
            parse_typed_operation(operations.at("pre_replace"), false));
        parsed.operations.emplace(
            ProviderLifecycleOperation::post_replace,
            parse_typed_operation(operations.at("post_replace"), true));
    }

    const auto& workspace = control.at("workspace_argument");
    if (!exact_object(workspace, {"flag", "relative_path"})) invalid_control();
    parsed.workspace_flag = required_string(workspace, "flag");
    parsed.workspace_relative_path =
        parse_workspace_relative_path(required_string(workspace, "relative_path"));
    parsed.configuration_flag = required_string(control, "configuration_argument");
    if (!valid_flag(parsed.workspace_flag, true) ||
        !valid_flag(parsed.configuration_flag, true)) {
        invalid_control();
    }

    if (has_pre_replace) {
        const auto& replacement = control.at("replacement_arguments");
        if (!exact_object(replacement, {"previous_root", "replacement_root"})) {
            invalid_control();
        }
        parsed.previous_root_flag = required_string(replacement, "previous_root");
        parsed.replacement_root_flag = required_string(replacement, "replacement_root");
        if (!valid_flag(*parsed.previous_root_flag, true) ||
            !valid_flag(*parsed.replacement_root_flag, true)) {
            invalid_control();
        }
    }
    return parsed;
}

} // namespace

ProviderLifecycleControl parse_provider_lifecycle_control(
    const nlohmann::json& startup_control) {
    if (!startup_control.is_object() || !startup_control.contains("schema") ||
        !startup_control.at("schema").is_string() ||
        !startup_control.contains("schema_version") ||
        !startup_control.at("schema_version").is_number_integer()) {
        invalid_control();
    }
    const auto& schema = startup_control.at("schema").get_ref<const std::string&>();
    const auto& version = startup_control.at("schema_version");
    if (schema == "arena_provider_startup_control_v1" && version == 1) {
        return parse_legacy(startup_control, false);
    }
    if (schema == "arena_provider_startup_control_v2" && version == 2) {
        return parse_legacy(startup_control, true);
    }
    if (schema == "arena_provider_startup_control_v3" && version == 3) {
        return parse_typed(startup_control);
    }
    invalid_control();
}

} // namespace arena::application::installer
