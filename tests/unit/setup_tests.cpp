#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <sodium.h>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "arena/application/installer/credential_recovery.hpp"
#include "arena/application/installer/removal_lifecycle.hpp"
#include "arena/application/installer/setup.hpp"
#include "arena/domain/uuid.hpp"
#include "arena/ports/asset_search_index.hpp"

namespace {

namespace fs = std::filesystem;
using arena::application::installer::SetupEngine;
using arena::application::installer::SetupPaths;
using arena::application::installer::RemovalLifecycle;
using arena::application::installer::RemovalLifecycleHooks;
using arena::application::installer::assess_book_forge_credential_recovery;
using arena::application::installer::recover_book_forge_credential;
using nlohmann::json;

class FakeAssetSearchIndex final : public arena::ports::AssetSearchIndex {
  public:
    explicit FakeAssetSearchIndex(arena::ports::IndexedAssetSearchResult result)
        : result_(std::move(result)) {}

    [[nodiscard]] arena::ports::IndexedAssetSearchResult
    search(const arena::ports::IndexedAssetQuery& query) override {
        last_query = query;
        return result_;
    }

    arena::ports::IndexedAssetQuery last_query{};

  private:
    arena::ports::IndexedAssetSearchResult result_;
};

fs::path temporary_directory() {
    const fs::path directory =
        fs::temp_directory_path() / ("arena-setup-" + arena::domain::uuid_v7());
    fs::create_directories(directory);
    return directory;
}

json component(const std::string& id, const std::vector<std::string>& dependencies = {}) {
    return {
        {"id", id},
        {"version", "1.0.0"},
        {"dependencies", dependencies},
        {"installed_bytes", 1},
        {"staging_bytes", 1},
        {"backup_bytes", 0},
        {"workspace_reserve_bytes", 0},
        {"artifacts",
         {{"any",
           {{"source", id + ".tar.gz"}, {"sha256", std::string(64, '0')}, {"size_bytes", 1}}}}},
    };
}

std::string sha256_text(const std::string_view value) {
    std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
    if (crypto_hash_sha256(
            digest.data(),
            reinterpret_cast<const unsigned char*>(value.data()),
            static_cast<unsigned long long>(value.size())) != 0) {
        throw std::runtime_error("test SHA-256 calculation failed");
    }
    std::array<char, crypto_hash_sha256_BYTES * 2 + 1> encoded{};
    sodium_bin2hex(encoded.data(), encoded.size(), digest.data(), digest.size());
    return encoded.data();
}

std::string sha256_path(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    const std::string contents{
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    return sha256_text(contents);
}

void write_mock_renderer_runtime(const SetupPaths& paths) {
    const fs::path source = ARENA_MOCK_SERVICE_PATH;
    const fs::path active_root =
        paths.install_root / "components" / "runtime.host" / "1.0.0";
    const fs::path relative = fs::path("bin") / source.filename();
    const fs::path executable = active_root / relative;
    fs::create_directories(executable.parent_path());
    fs::create_directories(paths.data_root / "receipts");
    fs::copy_file(source, executable, fs::copy_options::overwrite_existing);
    const auto size = fs::file_size(executable);
    const std::string digest = sha256_path(executable);
    std::ofstream(active_root / "arena-component.json", std::ios::binary)
        << json{
               {"schema", "arena_install_component_v1"},
               {"schema_version", 1},
               {"component_id", "runtime.host"},
               {"component_version", "1.0.0"},
               {"entrypoints", {{"execute", relative.generic_string()}}},
               {"dependencies", json::array()},
               {"files",
                json::array({{{"path", relative.generic_string()},
                              {"size_bytes", size},
                              {"sha256", digest}}})},
           }
               .dump(2);
    std::ofstream(paths.data_root / "receipts" / "runtime.host.json", std::ios::binary)
        << json{
               {"schema", "arena_install_receipt_v1"},
               {"schema_version", 1},
               {"component_id", "runtime.host"},
               {"component_version", "1.0.0"},
               {"state", "installed_managed"},
               {"active_root", active_root.string()},
               {"artifact_sha256", std::string(64, 'a')},
               {"dependencies", json::array()},
               {"data_paths", json::array()},
               {"configuration_path", ""},
               {"activated_files", json::array()},
           }
               .dump(2);
}

fs::path write_mock_renderer_consumer(const SetupPaths& paths,
                                      const std::string& component_id,
                                      const std::string& locator_kind) {
    write_mock_renderer_runtime(paths);
    const fs::path active_root =
        paths.install_root / "components" / component_id / "1.0.0";
    const fs::path renderer = active_root / "installer-integration.fixture";
    const fs::path workspace = paths.workspace_root / component_id;
    const fs::path configuration = workspace / "manager-config.json";
    fs::create_directories(active_root);
    fs::create_directories(workspace);
    std::ofstream(renderer, std::ios::binary) << "renderer";
    const json integration{
        {"schema", "arena_provider_installer_integration_v1"},
        {"schema_version", 1},
        {"entrypoint", "installer_integration"},
        {"invocation",
         {{"kind", "dependency_runtime"},
          {"runtime_component_id", "runtime.host"},
          {"runtime_entrypoint", "execute"}}},
        {"arguments", json::array({"--context"})},
        {"workspace_name", component_id},
        {"configure_when_disabled", true},
        {"dependency_bindings", json::array()},
        {"locator_bindings",
         json::array({{{"name", "selected"},
                       {"kind", locator_kind},
                       {"required", false}}})},
        {"startup_control",
         {{"schema", "arena_provider_startup_control_v1"},
          {"schema_version", 1},
          {"entrypoint", "installer_integration"},
          {"actions",
           {{"install", json::array({"lifecycle", "enable"})},
            {"uninstall", json::array({"lifecycle", "disable"})}}},
          {"workspace_argument", {{"flag", "--state-root"}, {"relative_path", "runtime"}}},
          {"configuration_argument", "--settings"},
          {"result",
           {{"envelope", "root"},
            {"require_configuration_match_on_install", true},
            {"require_live_on_install", true}}}}},
    };
    std::ofstream(active_root / "arena-component.json", std::ios::binary)
        << json{
               {"schema", "arena_install_component_v1"},
               {"schema_version", 1},
               {"component_id", component_id},
               {"component_version", "1.0.0"},
               {"entrypoints", {{"installer_integration", "installer-integration.fixture"}}},
               {"capabilities", json::array({"startup_install", "startup_uninstall"})},
               {"dependencies", json::array({"runtime.host"})},
               {"workspace_name", component_id},
               {"installer_integration", integration},
               {"files",
                json::array({{{"path", "installer-integration.fixture"},
                              {"size_bytes", 8},
                              {"sha256", sha256_text("renderer")}}})},
           }
               .dump(2);
    std::ofstream(configuration, std::ios::binary) << R"({"schema":"old"})";
    std::ofstream(paths.data_root / "receipts" / (component_id + ".json"),
                  std::ios::binary)
        << json{
               {"schema", "arena_install_receipt_v1"},
               {"schema_version", 1},
               {"component_id", component_id},
               {"component_version", "1.0.0"},
               {"state", "installed_disabled"},
               {"active_root", active_root.string()},
               {"artifact_sha256", std::string(64, 'a')},
               {"dependencies", json::array({"runtime.host"})},
               {"data_paths", json::array({workspace.string()})},
               {"configuration_path", configuration.string()},
               {"activated_files", json::array()},
           }
               .dump(2);
    return configuration;
}

json manager_service_api(
    const std::initializer_list<const char*> additional_operations = {}) {
    json operations = json::array(
        {"describe", "status", "outputs_list", "outputs_inspect", "config_validate"});
    for (const char* operation : additional_operations) {
        operations.push_back(operation);
    }
    return {
        {"schema", "uci_arena_manager_service_api_v1"},
        {"version", "1.0"},
        {"transport", "cli_json"},
        {"entrypoint", "cli"},
        {"arguments", {"api"}},
        {"operations", std::move(operations)},
        {"output_schema", "uci_arena_service_output_v1"},
    };
}

SetupPaths paths_under(const fs::path& root) {
    fs::create_directories(root / "workspace");
    return {
        root / "install", root / "data", root / "cache", root / "workspace", root / "knowledge",
    };
}

fs::path write_lichess_single_profile(const fs::path& root,
                                      const std::string& filename =
                                          "uci_arena_single_game_profile_v1.json") {
    const fs::path evidence_root = root / "arena" / "results";
    fs::create_directories(evidence_root);
    std::ofstream(evidence_root / "diagnostic.json", std::ios::binary)
        << "diagnostic";
    std::ofstream(evidence_root / "complete-game.json", std::ios::binary)
        << "complete-game";
    const json diagnostic{
        {"passed", true},
        {"artifact", "arena/results/diagnostic.json"},
        {"artifact_sha256",
         "sha256:5a695eea5b00a31f8aef7dbb89c8f798fab371246ac1549afe84b16420707b99"},
    };
    const json complete{
        {"passed", true},
        {"artifact", "arena/results/complete-game.json"},
        {"artifact_sha256",
         "sha256:405758a289456712858d5ce6161c65529a244312e4ec2c92117cacd26cc60f87"},
    };
    const fs::path profile_path = root / filename;
    std::ofstream(profile_path)
        << json{
               {"schema", "uci_arena_single_game_profile_v1"},
               {"qualified", true},
               {"evidence",
                {{"cursor_correctness", complete},
                 {"provider_parity", diagnostic},
                 {"deadline", diagnostic},
                 {"topology", complete},
                 {"complete_game", complete}}},
           }
               .dump(2);
    return profile_path;
}

void write_book_forge_service_install(
    const SetupPaths& paths,
    const std::optional<std::pair<std::string_view, std::string_view>>& contract) {
    const fs::path active_root =
        paths.install_root / "components" / "book_forge.service" / "0.2.0";
    const fs::path cli = active_root / "services" / "book-forge" / "src" / "cli.mjs";
    const fs::path contract_path =
        active_root / "services" / "book-forge" / "contracts" / "service-contract.json";
    fs::create_directories(cli.parent_path());
    fs::create_directories(paths.data_root / "receipts");
    std::ofstream(cli, std::ios::binary) << "book-forge";
    json files = json::array(
        {{{"path", "services/book-forge/src/cli.mjs"},
          {"size_bytes", 10},
          {"sha256", "f7628bf86d7b79dab3847e8f5101b3f82224d7bdf694a9817cc0bfc710d553ad"}}});
    json manifest{
        {"schema", "arena_install_component_v1"},
        {"schema_version", 1},
        {"component_id", "book_forge.service"},
        {"component_version", "0.2.0"},
        {"entrypoints", {{"cli", "services/book-forge/src/cli.mjs"}}},
        {"capabilities", json::array({"status", "start"})},
        {"data_paths", json::array({"workspace"})},
        {"startup",
         {{"windows", "per_user_task_scheduler"}, {"linux", "per_user_systemd"}}},
    };
    if (contract.has_value()) {
        fs::create_directories(contract_path.parent_path());
        std::ofstream(contract_path, std::ios::binary) << contract->first;
        files.push_back(
            {{"path", "services/book-forge/contracts/service-contract.json"},
             {"size_bytes", contract->first.size()},
             {"sha256", std::string(contract->second)}});
        manifest["contract_manifest"] =
            "services/book-forge/contracts/service-contract.json";
    }
    manifest["files"] = std::move(files);
    std::ofstream(active_root / "arena-component.json", std::ios::binary)
        << manifest.dump(2);

    json receipt{
        {"schema", "arena_install_receipt_v1"},
        {"schema_version", 1},
        {"component_id", "book_forge.service"},
        {"component_version", "0.2.0"},
        {"state", "installed_managed"},
        {"active_root", active_root.string()},
        {"artifact_sha256", std::string(64, 'a')},
        {"dependencies", json::array()},
        {"data_paths", json::array({(paths.workspace_root / "book-forge").string()})},
        {"configuration_path", ""},
        {"activated_files", json::array()},
    };
    if (contract.has_value()) {
        receipt["service_contract"] = {
            {"schema", "uci_arena_standalone_service_contract_v1"},
            {"path", "services/book-forge/contracts/service-contract.json"},
            {"sha256", std::string(contract->second)},
            {"configuration_schema", "uci_arena_book_forge_config_v1"},
            {"status_schema", "uci_arena_book_forge_status_v2"},
        };
    }
    std::ofstream(paths.data_root / "receipts" / "book_forge.service.json",
                  std::ios::binary)
        << receipt.dump(2);
}

void write_manager_api_service_install(const SetupPaths& paths, const json& manager_api) {
    constexpr std::string_view component_id = "uci_arena_evidence.service";
    constexpr std::string_view component_version = "0.3.0";
    constexpr std::string_view cli_relative = "services/evidence-service/src/cli.mjs";
    constexpr std::string_view contract_relative =
        "services/evidence-service/contracts/service-contract.json";
    constexpr std::string_view cli_contents = "evidence";
    const fs::path active_root =
        paths.install_root / "components" / component_id / component_version;
    const fs::path cli = active_root / cli_relative;
    const fs::path contract_path = active_root / contract_relative;
    fs::create_directories(cli.parent_path());
    fs::create_directories(contract_path.parent_path());
    fs::create_directories(paths.data_root / "receipts");
    std::ofstream(cli, std::ios::binary) << cli_contents;

    const json startup{
        {"windows", "per_user_task_scheduler"},
        {"linux", "per_user_systemd"},
    };
    const json contract{
        {"schema", "uci_arena_standalone_service_contract_v1"},
        {"component_id", component_id},
        {"component_version", component_version},
        {"cli_entrypoint", cli_relative},
        {"configuration_schema", "uci_arena_evidence_service_config_v1"},
        {"status_schema", "uci_arena_evidence_status_v1"},
        {"capabilities", json::array({"status", "start"})},
        {"data_paths", json::array({"workspace"})},
        {"startup", startup},
        {"health_smoke",
         {{"command", "status"}, {"success", "exit_zero_and_status_schema"}}},
        {"manager_integration_api", manager_api},
        {"outputs", json::object()},
        {"persistence",
         {{"upgrade", "preserve_and_migrate"},
          {"repair", "preserve"},
          {"uninstall_default", "preserve"}}},
    };
    const std::string contract_contents = contract.dump();
    std::ofstream(contract_path, std::ios::binary) << contract_contents;
    const std::string contract_sha256 = sha256_text(contract_contents);

    std::ofstream(active_root / "arena-component.json", std::ios::binary)
        << json{
               {"schema", "arena_install_component_v1"},
               {"schema_version", 1},
               {"component_id", component_id},
               {"component_version", component_version},
               {"entrypoints", {{"cli", cli_relative}}},
               {"contract_manifest", contract_relative},
               {"manager_integration_api", manager_api},
               {"capabilities", json::array({"status", "start"})},
               {"data_paths", json::array({"workspace"})},
               {"startup", startup},
               {"files",
                json::array(
                    {{{"path", cli_relative},
                      {"size_bytes", cli_contents.size()},
                      {"sha256", sha256_text(cli_contents)}},
                     {{"path", contract_relative},
                      {"size_bytes", contract_contents.size()},
                      {"sha256", contract_sha256}}})},
           }
               .dump(2);

    std::ofstream(
        paths.data_root / "receipts" / "uci_arena_evidence.service.json",
        std::ios::binary)
        << json{
               {"schema", "arena_install_receipt_v1"},
               {"schema_version", 1},
               {"component_id", component_id},
               {"component_version", component_version},
               {"state", "installed_managed"},
               {"active_root", active_root.string()},
               {"artifact_sha256", std::string(64, 'a')},
               {"dependencies", json::array()},
               {"data_paths", json::array()},
               {"configuration_path", ""},
               {"activated_files", json::array()},
               {"service_contract",
                {{"schema", "uci_arena_standalone_service_contract_v1"},
                 {"path", contract_relative},
                 {"sha256", contract_sha256},
                 {"configuration_schema", "uci_arena_evidence_service_config_v1"},
                 {"status_schema", "uci_arena_evidence_status_v1"},
                 {"manager_integration_api", manager_api}}},
           }
               .dump(2);
}

} // namespace

TEST_CASE("setup plan resolves only explicitly composed components") {
    const fs::path root = temporary_directory();
    const json catalog{
        {"schema", "arena_install_catalog_v1"},
        {"schema_version", 1},
        {"suite_version", "1.0.0"},
        {"presets", {{"manager_only", json::array({"manager.core"})}}},
        {"components", json::array({component("manager.core"), component("node_runtime.private"),
                                    component("book_forge.service", {"node_runtime.private"})})},
    };
    const auto plan =
        SetupEngine::plan(catalog, paths_under(root), "custom",
                          {"book_forge.service", "node_runtime.private", "manager.core"},
                          true, false);
    REQUIRE(plan.at("schema") == "arena_install_plan_v1");
    REQUIRE(plan.at("actions").size() == 3);
    REQUIRE(plan.at("actions").at(0).at("component_id") == "node_runtime.private");
    REQUIRE(plan.at("actions").at(1).at("component_id") == "book_forge.service");
    REQUIRE(plan.at("actions").at(2).at("component_id") == "manager.core");
    fs::remove_all(root);
}

void write_provider_service_install(const SetupPaths& paths,
                                    const std::string_view contract_version = "1.0") {
    constexpr std::string_view component_id = "replaceable.provider";
    constexpr std::string_view component_version = "1.0.0";
    constexpr std::string_view cli_relative = "provider.mjs";
    constexpr std::string_view cli_contents = "provider";
    constexpr std::string_view renderer_relative = "installer-integration.mjs";
    constexpr std::string_view renderer_contents = "renderer";
    constexpr std::string_view contract_relative = "contracts/service-contract.json";
    constexpr std::string_view operations_relative = "contracts/manager-operations.json";
    constexpr std::string_view configuration_relative = "contracts/configuration-schema.json";
    constexpr std::string_view configuration_update_relative =
        "contracts/configuration-update-schema.json";
    constexpr std::string_view status_relative = "contracts/status-schema.json";
    const fs::path active_root =
        paths.install_root / "components" / component_id / component_version;
    fs::create_directories(active_root / "contracts");
    fs::create_directories(paths.data_root / "receipts");
    std::ofstream(active_root / cli_relative, std::ios::binary) << cli_contents;
    std::ofstream(active_root / renderer_relative, std::ios::binary) << renderer_contents;

    const json closed_schema{
        {"$schema", "https://json-schema.org/draft/2020-12/schema"},
        {"$id", "https://uci-arena.example/test/provider.json"},
        {"type", "object"},
        {"required", json::array({"schema"})},
        {"properties", {{"schema", {{"type", "string"}}}}},
        {"additionalProperties", false},
    };
    const json operations{
        {"schema", "replaceable_provider_manager_api_v1"},
        {"version", "1.0"},
        {"transport",
         {{"kind", "cli_json"},
          {"entrypoint", cli_relative},
          {"stdout", "one_bounded_json_document"},
          {"stderr", "sanitized_diagnostics"},
          {"shell", false}}},
        {"operations",
         {{"provider_status",
           {{"arguments", json::array({"status"})},
            {"mutates", false},
            {"credential", "forbidden"}}}}},
    };
    const json startup{
        {"windows", "per_user_task_scheduler"},
        {"linux", "per_user_systemd"},
        {"credential_gated", true},
    };
    const json contract{
        {"schema", "uci_arena_service_contract_v1"},
        {"service",
         {{"component_id", component_id},
          {"component_version", component_version},
          {"product_id", "replaceable.provider.product"},
          {"contract_version", contract_version},
          {"transport", "cli_json"},
          {"entrypoint", cli_relative}}},
        {"invocation", {{"kind", "direct"}}},
        {"provided_capabilities", json::array({"provider.status"})},
        {"required_capabilities", json::array({"runtime.process"})},
        {"credential_contract",
         {{"input", "external_absolute_file_path"},
          {"contents_visible_to", "provider_transport_only"},
          {"manager_projection", "path_and_redacted_status_only"},
          {"installer_behavior", "never_package_copy_or_inspect"}}},
        {"startup", startup},
        {"persistence",
         {{"upgrade", "preserve_and_migrate"},
          {"repair", "preserve"},
          {"uninstall_default", "preserve"}}},
        {"manager_operations", operations_relative},
        {"configuration_schema", configuration_relative},
        {"configuration_update_schema", configuration_update_relative},
        {"status_schema", status_relative},
        {"error_contract",
         {{"format", "bounded_typed_json"},
          {"secrets", "redacted"},
          {"unknown_operation", "fail_closed"},
          {"unknown_major_version", "incompatible"}}},
    };
    const std::vector<std::pair<fs::path, std::string>> documents{
        {contract_relative, contract.dump()},
        {operations_relative, operations.dump()},
        {configuration_relative, closed_schema.dump()},
        {configuration_update_relative, closed_schema.dump()},
        {status_relative, closed_schema.dump()},
    };
    json files = json::array(
        {{{"path", cli_relative},
          {"size_bytes", cli_contents.size()},
          {"sha256", sha256_text(cli_contents)}},
         {{"path", renderer_relative},
          {"size_bytes", renderer_contents.size()},
          {"sha256", sha256_text(renderer_contents)}}});
    for (const auto& [relative, contents] : documents) {
        std::ofstream(active_root / relative, std::ios::binary) << contents;
        files.push_back({{"path", relative.generic_string()},
                         {"size_bytes", contents.size()},
                         {"sha256", sha256_text(contents)}});
    }
    const json installer_integration{
        {"schema", "arena_provider_installer_integration_v1"},
        {"schema_version", 1},
        {"entrypoint", "installer_integration"},
        {"invocation",
         {{"kind", "dependency_runtime"},
          {"runtime_component_id", "runtime.host"},
          {"runtime_entrypoint", "execute"}}},
        {"arguments", json::array({"--context"})},
        {"workspace_name", "replaceable-provider"},
        {"configure_when_disabled", false},
        {"dependency_bindings", json::array()},
        {"locator_bindings",
         json::array({{{"name", "timing_profile"},
                       {"kind", "timing_profile"},
                       {"required", false}}})},
    };
    std::ofstream(active_root / "arena-component.json", std::ios::binary)
        << json{
               {"schema", "arena_install_component_v1"},
               {"schema_version", 1},
               {"component_id", component_id},
               {"component_version", component_version},
               {"entrypoints",
                {{"cli", cli_relative}, {"installer_integration", renderer_relative}}},
               {"contract_manifest", contract_relative},
               {"capabilities", json::array({"provider_status"})},
               {"dependencies", json::array({"runtime.host"})},
               {"workspace_name", "replaceable-provider"},
               {"installer_integration", installer_integration},
               {"startup", startup},
               {"data_paths", json::array({"workspace"})},
               {"files", files},
           }
               .dump(2);

    const auto digest_for = [&](const std::string_view relative) {
        return sha256_text(
            std::find_if(documents.begin(), documents.end(), [&](const auto& item) {
                return item.first.generic_string() == relative;
            })->second);
    };
    std::ofstream(paths.data_root / "receipts" / "replaceable.provider.json",
                  std::ios::binary)
        << json{
               {"schema", "arena_install_receipt_v1"},
               {"schema_version", 1},
               {"component_id", component_id},
               {"component_version", component_version},
               {"state", "installed_disabled"},
               {"active_root", active_root.string()},
               {"artifact_sha256", std::string(64, 'a')},
               {"dependencies", json::array({"runtime.host"})},
               {"data_paths", json::array()},
               {"configuration_path", ""},
               {"activated_files", json::array()},
               {"service_contract",
                {{"schema", "uci_arena_service_contract_v1"},
                 {"path", contract_relative},
                 {"sha256", digest_for(contract_relative)},
                 {"configuration_schema_path", configuration_relative},
                 {"configuration_schema_sha256", digest_for(configuration_relative)},
                 {"configuration_update_schema_path", configuration_update_relative},
                 {"configuration_update_schema_sha256",
                  digest_for(configuration_update_relative)},
                 {"status_schema_path", status_relative},
                 {"status_schema_sha256", digest_for(status_relative)},
                 {"manager_operations_path", operations_relative},
                 {"manager_operations_sha256", digest_for(operations_relative)},
                 {"credential_gated_startup", true}}},
           }
               .dump(2);
}

TEST_CASE("setup planner applies catalog roles without product identity knowledge") {
    const fs::path root = temporary_directory();
    json producer = component("renamed.producer");
    producer["default_enabled"] = false;
    producer["disable_when_workspace_capacity_unavailable"] = true;
    producer["workspace_reserve_bytes"] = 1;
    json knowledge = component("renamed.asset", {"renamed.producer"});
    knowledge["activation_role"] = "knowledge";
    knowledge["installed_bytes"] = 7;
    const json catalog{
        {"schema", "arena_install_catalog_v1"},
        {"schema_version", 1},
        {"suite_version", "1.0.0"},
        {"presets", {{"empty", json::array()}}},
        {"optional_component_groups",
         {{"knowledge", json::array({"renamed.asset"})},
          {"books", json::array({"renamed.asset"})},
          {"tablebases", json::array({"renamed.asset"})}}},
        {"components", json::array({producer, knowledge})},
    };

    const json plan =
        SetupEngine::plan(catalog, paths_under(root), "empty", {}, true, true);

    REQUIRE(plan.at("actions").size() == 2);
    REQUIRE(plan.at("actions").at(0).at("component_id") == "renamed.producer");
    REQUIRE(plan.at("actions").at(0).at("enabled") == false);
    REQUIRE(plan.at("actions").at(1).at("component_id") == "renamed.asset");
    REQUIRE(plan.at("space").at("knowledge_activation_bytes") == 7);
    fs::remove_all(root);
}

TEST_CASE("feature reconciliation is catalog-owned and identity agnostic") {
    const fs::path root = temporary_directory();
    const json catalog{
        {"schema", "arena_install_catalog_v1"},
        {"schema_version", 1},
        {"suite_version", "1.0.0"},
        {"presets", {{"base", json::array({"shell.core"})}}},
        {"feature_selection",
         {{"base_preset", "base"},
          {"components", json::array({"feature.alpha", "feature.beta"})},
          {"default_selected", json::array({"feature.alpha", "feature.beta"})}}},
        {"components",
         json::array({component("shell.core"), component("shared.runtime"),
                      component("feature.alpha", {"shared.runtime"}),
                      component("feature.beta", {"shared.runtime"})})},
    };

    const json selection = SetupEngine::resolve_feature_selection(
        catalog, {"feature.alpha=0", "feature.beta=1"});

    REQUIRE(selection.at("selected_roots") ==
            json::array({"shell.core", "feature.beta"}));
    REQUIRE(selection.at("selected_closure") ==
            json::array({"shell.core", "shared.runtime", "feature.beta"}));
    REQUIRE(selection.at("removal_candidates") ==
            json::array({"feature.alpha"}));
    REQUIRE_THROWS(SetupEngine::resolve_feature_selection(
        catalog, {"unknown.product=1"}));
    fs::remove_all(root);
}

TEST_CASE("repair plan restages components that are already current") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const fs::path active_root = paths.install_root / "components" / "manager.core" / "1.0.0";
    fs::create_directories(active_root);
    std::ofstream(active_root / "payload", std::ios::binary) << "payload";
    std::ofstream(active_root / "arena-component.json")
        << json{
               {"schema", "arena_install_component_v1"},
               {"schema_version", 1},
               {"component_id", "manager.core"},
               {"component_version", "1.0.0"},
               {"files",
                json::array(
                    {{{"path", "payload"},
                      {"size_bytes", 7},
                      {"sha256",
                       "239f59ed55e737c77147cf55ad0c1b030b6d7ee748a7426952f9b852d5a935e5"}}})},
           }
               .dump(2);
    fs::create_directories(paths.data_root / "receipts");
    std::ofstream(
        paths.data_root / "receipts" /
        "manager.core.json")
        << json{
               {"schema", "arena_install_receipt_v1"},
               {"schema_version", 1},
               {"component_id", "manager.core"},
               {"component_version", "1.0.0"},
               {"state", "installed_managed"},
               {"active_root", active_root.string()},
           }
               .dump(2);
    const json catalog{
        {"schema", "arena_install_catalog_v1"},
        {"schema_version", 1},
        {"suite_version", "1.0.0"},
        {"presets", {{"manager_only", json::array({"manager.core"})}}},
        {"optional_component_groups",
         {{"knowledge", json::array({"opening_book.standard", "syzygy.wdl.3-4-5"})},
          {"books", json::array({"opening_book.standard"})},
          {"tablebases", json::array({"syzygy.wdl.3-4-5"})}}},
        {"components", json::array({component("manager.core")})},
    };

    const json keep = SetupEngine::plan(catalog, paths, "manager_only", {}, true, false);
    REQUIRE(keep.at("actions").at(0).at("action") == "keep");

    const json repair =
        SetupEngine::plan(catalog, paths, "manager_only", {}, true, false, false, false, true);
    REQUIRE(repair.at("repair") == true);
    REQUIRE(repair.at("actions").at(0).at("action") == "repair");
    REQUIRE(repair.at("actions").at(0).contains("artifact"));
    fs::remove_all(root);
}

TEST_CASE("standalone service contracts are identity-bound and receipt-verified") {
    constexpr std::string_view valid_contract =
        R"({"schema":"uci_arena_standalone_service_contract_v1","component_id":"book_forge.service","component_version":"0.2.0","cli_entrypoint":"services/book-forge/src/cli.mjs","configuration_schema":"uci_arena_book_forge_config_v1","status_schema":"uci_arena_book_forge_status_v2","capabilities":["status","start"],"data_paths":["workspace"],"startup":{"windows":"per_user_task_scheduler","linux":"per_user_systemd"},"health_smoke":{"command":"status","success":"exit_zero_and_status_schema"},"outputs":{},"persistence":{"upgrade":"preserve_and_migrate","repair":"preserve","uninstall_default":"preserve"}})";
    constexpr std::string_view invalid_entrypoint_contract =
        R"({"schema":"uci_arena_standalone_service_contract_v1","component_id":"book_forge.service","component_version":"0.2.0","cli_entrypoint":"services/book-forge/src/wrong.mjs","configuration_schema":"uci_arena_book_forge_config_v1","status_schema":"uci_arena_book_forge_status_v2","capabilities":["status","start"],"data_paths":["workspace"],"startup":{"windows":"per_user_task_scheduler","linux":"per_user_systemd"},"health_smoke":{"command":"status","success":"exit_zero_and_status_schema"},"outputs":{},"persistence":{"upgrade":"preserve_and_migrate","repair":"preserve","uninstall_default":"preserve"}})";

    const fs::path valid_root = temporary_directory();
    const SetupPaths valid_paths = paths_under(valid_root);
    write_book_forge_service_install(
        valid_paths,
        std::pair{
            valid_contract,
            std::string_view{
                "eb5d969de295d01cbbb365c82d645798cc1e2c482e1f03ff664c0547bd49314a"}});
    REQUIRE(SetupEngine::verify(valid_paths).at("ok") == true);
    fs::remove_all(valid_root);

    const fs::path mismatched_root = temporary_directory();
    const SetupPaths mismatched_paths = paths_under(mismatched_root);
    write_book_forge_service_install(
        mismatched_paths,
        std::pair{
            invalid_entrypoint_contract,
            std::string_view{
                "45404169ac7dc063d4a41841a6e6d1ab430fbceee8f98a9f7f26317a80b7b2ae"}});
    REQUIRE(SetupEngine::verify(mismatched_paths).at("ok") == false);
    fs::remove_all(mismatched_root);

}

TEST_CASE("typed provider service and installation contracts are replaceable and receipt-verified") {
    const fs::path valid_root = temporary_directory();
    const SetupPaths valid_paths = paths_under(valid_root);
    write_provider_service_install(valid_paths);
    REQUIRE(SetupEngine::verify(valid_paths).at("ok") == true);
    fs::remove_all(valid_root);

    const fs::path missing_root = temporary_directory();
    const SetupPaths missing_paths = paths_under(missing_root);
    write_provider_service_install(missing_paths);
    const fs::path missing_manifest =
        missing_paths.install_root / "components" / "replaceable.provider" / "1.0.0" /
        "arena-component.json";
    json missing_contract = json::parse(std::ifstream(missing_manifest));
    missing_contract.erase("contract_manifest");
    missing_contract["manager_integration_api"] =
        manager_service_api({"orchestration_status"});
    std::ofstream(missing_manifest, std::ios::binary | std::ios::trunc)
        << missing_contract.dump(2);
    const json missing_verified = SetupEngine::verify(missing_paths);
    REQUIRE(missing_verified.at("ok") == false);
    REQUIRE(missing_verified.at("components").at(0).at("error") ==
            "integrated provider component is missing its contract manifest");
    fs::remove_all(missing_root);

    const fs::path incompatible_root = temporary_directory();
    const SetupPaths incompatible_paths = paths_under(incompatible_root);
    write_provider_service_install(incompatible_paths, "2.0");
    REQUIRE(SetupEngine::verify(incompatible_paths).at("ok") == false);
    fs::remove_all(incompatible_root);

    const fs::path tampered_root = temporary_directory();
    const SetupPaths tampered_paths = paths_under(tampered_root);
    write_provider_service_install(tampered_paths);
    std::ofstream(
        tampered_paths.install_root / "components" / "replaceable.provider" / "1.0.0" /
            "contracts" / "manager-operations.json",
        std::ios::app)
        << " ";
    REQUIRE(SetupEngine::verify(tampered_paths).at("ok") == false);
    fs::remove_all(tampered_root);
}

TEST_CASE("standalone Manager API accepts additive typed operations") {
    const fs::path valid_root = temporary_directory();
    const SetupPaths valid_paths = paths_under(valid_root);
    const json extended_api = manager_service_api(
        {"orchestration_status", "target_register", "campaign_enqueue", "job_control"});
    write_manager_api_service_install(valid_paths, extended_api);
    const json verified = SetupEngine::verify(valid_paths);
    REQUIRE(verified.at("ok") == true);
    fs::remove_all(valid_root);

    const fs::path incomplete_root = temporary_directory();
    const SetupPaths incomplete_paths = paths_under(incomplete_root);
    json incomplete_api = manager_service_api({"orchestration_status"});
    incomplete_api["operations"].erase(incomplete_api["operations"].end() - 2);
    write_manager_api_service_install(incomplete_paths, incomplete_api);
    const json rejected = SetupEngine::verify(incomplete_paths);
    REQUIRE(rejected.at("ok") == false);
    REQUIRE(
        rejected.at("components").at(0).at("error") ==
        "standalone service Manager API operations are incomplete");
    fs::remove_all(incomplete_root);
}

TEST_CASE("download missing knowledge selects opening book and bounded Syzygy WDL") {
    const fs::path root = temporary_directory();
    const json catalog{
        {"schema", "arena_install_catalog_v1"},
        {"schema_version", 1},
        {"suite_version", "1.0.0"},
        {"presets", {{"manager_only", json::array({"manager.core"})}}},
        {"optional_component_groups",
         {{"knowledge", json::array({"opening_book.standard", "syzygy.wdl.3-4-5"})},
          {"books", json::array({"opening_book.standard"})},
          {"tablebases", json::array({"syzygy.wdl.3-4-5"})}}},
        {"components", json::array({component("manager.core"), component("opening_book.standard"),
                                    component("syzygy.wdl.3-4-5")})},
    };
    const auto plan =
        SetupEngine::plan(catalog, paths_under(root), "manager_only", {}, false, true);
    REQUIRE(plan.at("download_missing_knowledge") == true);
    REQUIRE(plan.at("actions").size() == 3);
    REQUIRE(plan.at("actions").at(1).at("component_id") == "opening_book.standard");
    REQUIRE(plan.at("actions").at(2).at("component_id") == "syzygy.wdl.3-4-5");
    fs::remove_all(root);
}

TEST_CASE("missing books and tablebases can be selected independently") {
    const fs::path root = temporary_directory();
    const json catalog{
        {"schema", "arena_install_catalog_v1"},
        {"schema_version", 1},
        {"suite_version", "1.0.0"},
        {"presets", {{"manager_only", json::array({"manager.core"})}}},
        {"optional_component_groups",
         {{"knowledge", json::array({"opening_book.standard", "syzygy.wdl.3-4-5"})},
          {"books", json::array({"opening_book.standard"})},
          {"tablebases", json::array({"syzygy.wdl.3-4-5"})}}},
        {"components", json::array({component("manager.core"), component("opening_book.standard"),
                                    component("syzygy.wdl.3-4-5")})},
    };
    const auto books = SetupEngine::plan(catalog, paths_under(root), "manager_only", {}, false,
                                         false, true, false);
    REQUIRE(books.at("download_missing_books") == true);
    REQUIRE(books.at("download_missing_tablebases") == false);
    REQUIRE(books.at("actions").size() == 2);
    REQUIRE(books.at("actions").at(1).at("component_id") == "opening_book.standard");

    const auto tablebases = SetupEngine::plan(catalog, paths_under(root), "manager_only", {}, false,
                                              false, false, true);
    REQUIRE(tablebases.at("download_missing_books") == false);
    REQUIRE(tablebases.at("download_missing_tablebases") == true);
    REQUIRE(tablebases.at("actions").size() == 2);
    REQUIRE(tablebases.at("actions").at(1).at("component_id") == "syzygy.wdl.3-4-5");
    fs::remove_all(root);
}

TEST_CASE("inventory reports actionable missing and present knowledge assets") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    auto inventory = SetupEngine::inventory(paths);
    REQUIRE(inventory.at("knowledge").at("opening_book").at("state") == "missing");
    REQUIRE(inventory.at("knowledge").at("syzygy").at("state") == "missing");

    const fs::path book_root = paths.knowledge_root / "opening_books";
    const fs::path tablebase_root = paths.knowledge_root / "tablebases";
    fs::create_directories(book_root);
    fs::create_directories(tablebase_root);
    const std::array<std::string, 3> book_files{
        "strong_rare_v1.bin", "strong_rare_v1.stats", "strong_rare_v1.policy"};
    json book_artifacts = json::object();
    for (const std::string& name : book_files) {
        std::ofstream(book_root / name) << "fixture";
        book_artifacts[name] = sha256_text("fixture");
    }
    std::ofstream(book_root / "snapshot.manifest.json")
        << json{{"schema", "uci_arena_book_snapshot_v2"},
                {"artifacts", std::move(book_artifacts)}}.dump(2);
    std::ofstream(tablebase_root / "KQvK.rtbw") << "fixture";

    inventory = SetupEngine::inventory(paths);
    REQUIRE(inventory.at("knowledge").at("opening_book").at("state") == "detected_unmanaged");
    REQUIRE(inventory.at("knowledge").at("syzygy").at("state") == "detected_unmanaged");
    REQUIRE(inventory.at("knowledge").at("syzygy").at("wdl_files") == 1);

    fs::remove(book_root / "strong_rare_v1.stats");
    inventory = SetupEngine::inventory(paths);
    REQUIRE(inventory.at("knowledge").at("opening_book").at("state") == "partial");
    REQUIRE(inventory.at("knowledge").at("opening_book").at("missing_files") ==
            json::array({"strong_rare_v1.stats"}));

    std::ofstream(book_root / "strong_rare_v1.stats") << "fixture";
    std::ofstream(book_root / "snapshot.manifest.json")
        << json{{"schema", "uci_arena_book_snapshot_v2"},
                {"artifacts",
                 {{"../escaped.bin", sha256_text("fixture")}}}}
               .dump(2);
    inventory = SetupEngine::inventory(paths);
    REQUIRE(inventory.at("knowledge").at("opening_book").at("state") == "invalid");
    REQUIRE(inventory.at("knowledge").at("opening_book").contains("validation_error"));

    fs::remove_all(root);
}

TEST_CASE("file-set tablebases verify every file before atomic activation") {
    const fs::path root = temporary_directory();
    SetupPaths paths = paths_under(root);
    paths.bundle_owner = "{11111111-1111-1111-1111-111111111111}";
    constexpr const char* component_id = "replacement.tables";
    const std::string manifest =
        R"({"schema":"arena_syzygy_file_set_manifest_v1","schema_version":1,"component_id":"replacement.tables","component_version":"1.0.0","total_size_bytes":11,"files":[{"path":"KQvK.rtbw","piece_count":3,"size_bytes":11,"sha256":"b905c2469583111c5d8dc8057dc426fcf74f17ec72adade00d8d7c80443b6d2f","sources":["KQvK.rtbw"]}]})";
    std::ofstream(root / "KQvK.rtbw", std::ios::binary) << "wdl-fixture";
    std::ofstream(root / "syzygy.file-set.json", std::ios::binary) << manifest;

    const json artifact{
        {"type", "file_set"},        {"source", "syzygy.file-set.json"},
        {"sha256", sha256_text(manifest)}, {"size_bytes", manifest.size()},
        {"file_count", 1},           {"total_size_bytes", 11},
    };
    const json plan{
        {"schema", "arena_install_plan_v1"},
        {"blockers", json::array()},
        {"offline", true},
        {"catalog_directory", root.string()},
        {"actions", json::array({{{"action", "install"},
                                  {"component_id", component_id},
                                  {"version", "1.0.0"},
                                  {"enabled", true},
                                  {"artifact", artifact}}})},
    };
    const json applied = SetupEngine::apply(json::object(), plan, paths, true, false);
    REQUIRE(applied.at("state") == "committed");
    REQUIRE(applied.at("components").at(0).at("bundle_owner") == paths.bundle_owner);
    REQUIRE(applied.at("components").at(0).at("component_id") == component_id);
    REQUIRE(applied.at("components").at(0).at("activated_resources").at(0).at("kind") ==
            "syzygy");
    const json inventory = SetupEngine::inventory(paths);
    REQUIRE(inventory.at("knowledge").at("syzygy").at("state") == "installed_managed");
    REQUIRE(inventory.at("knowledge").at("syzygy").at("managed_components") ==
            json::array({component_id}));
    REQUIRE(fs::is_regular_file(paths.knowledge_root / "tablebases" / "KQvK.rtbw"));
    REQUIRE(fs::file_size(paths.knowledge_root / "tablebases" / "KQvK.rtbw") == 11);
    const fs::path locator_path = paths.data_root / "locators" / "syzygy.json";
    REQUIRE(fs::is_regular_file(locator_path));
    std::ifstream locator_stream(locator_path, std::ios::binary);
    const json locator = json::parse(locator_stream);
    REQUIRE(locator.at("kind") == "syzygy");
    REQUIRE(fs::path(locator.at("path").get<std::string>()) ==
            fs::absolute(paths.knowledge_root / "tablebases").lexically_normal());
    locator_stream.close();
    const json verified = SetupEngine::verify(paths);
    INFO(verified.dump(2));
    REQUIRE(verified.at("ok") == true);
    const json uninstall_plan = SetupEngine::plan_uninstall(paths, {component_id});
    const json uninstall = SetupEngine::uninstall(paths, uninstall_plan, true);
    REQUIRE(uninstall.at("components").size() == 1);
    REQUIRE(fs::is_regular_file(paths.knowledge_root / "tablebases" / "KQvK.rtbw"));
    REQUIRE(fs::is_regular_file(locator_path));
    REQUIRE_FALSE(fs::exists(paths.data_root / "receipts" /
                             (std::string(component_id) + ".json")));
    REQUIRE_FALSE(fs::exists(paths.install_root / "components" / component_id / "1.0.0"));

    fs::remove_all(root);
}

TEST_CASE("file-set catalog totals fail closed before activation") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const std::string manifest =
        R"({"schema":"arena_syzygy_file_set_manifest_v1","schema_version":1,"component_id":"opening_book.misleading","component_version":"1.0.0","total_size_bytes":11,"files":[{"path":"KQvK.rtbw","piece_count":3,"size_bytes":11,"sha256":"b905c2469583111c5d8dc8057dc426fcf74f17ec72adade00d8d7c80443b6d2f","sources":["KQvK.rtbw"]}]})";
    std::ofstream(root / "KQvK.rtbw", std::ios::binary) << "wdl-fixture";
    std::ofstream(root / "syzygy.file-set.json", std::ios::binary) << manifest;
    const json plan{
        {"schema", "arena_install_plan_v1"},
        {"blockers", json::array()},
        {"offline", true},
        {"catalog_directory", root.string()},
        {"actions", json::array({{{"action", "install"},
                                  {"component_id", "opening_book.misleading"},
                                  {"version", "1.0.0"},
                                  {"enabled", true},
                                  {"artifact",
                                   {{"type", "file_set"},
                                    {"source", "syzygy.file-set.json"},
                                     {"sha256", sha256_text(manifest)},
                                     {"size_bytes", manifest.size()},
                                    {"file_count", 1},
                                    {"total_size_bytes", 12}}}}})},
    };
    REQUIRE_THROWS(SetupEngine::apply(json::object(), plan, paths, true, false));
    REQUIRE_FALSE(fs::exists(paths.knowledge_root / "tablebases" / "KQvK.rtbw"));
    REQUIRE_FALSE(fs::exists(paths.data_root / "locators" / "syzygy.json"));
    fs::remove_all(root);
}

TEST_CASE("later failure restores prior knowledge and removes activation authority") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const fs::path tablebases = paths.knowledge_root / "tablebases";
    fs::create_directories(tablebases);
    std::ofstream(tablebases / "existing.rtbw", std::ios::binary) << "operator-state";
    const std::string manifest =
        R"({"schema":"arena_syzygy_file_set_manifest_v1","schema_version":1,"component_id":"replacement.tables","component_version":"1.0.0","total_size_bytes":11,"files":[{"path":"KQvK.rtbw","piece_count":3,"size_bytes":11,"sha256":"b905c2469583111c5d8dc8057dc426fcf74f17ec72adade00d8d7c80443b6d2f","sources":["KQvK.rtbw"]}]})";
    std::ofstream(root / "KQvK.rtbw", std::ios::binary) << "wdl-fixture";
    std::ofstream(root / "syzygy.file-set.json", std::ios::binary) << manifest;
    const json plan{
        {"schema", "arena_install_plan_v1"},
        {"blockers", json::array()},
        {"offline", true},
        {"catalog_directory", root.string()},
        {"actions",
         json::array(
             {{{"action", "install"},
               {"component_id", "replacement.tables"},
               {"version", "1.0.0"},
               {"enabled", true},
               {"artifact",
                {{"type", "file_set"},
                 {"source", "syzygy.file-set.json"},
                 {"sha256", sha256_text(manifest)},
                 {"size_bytes", manifest.size()},
                 {"file_count", 1},
                 {"total_size_bytes", 11}}}},
              {{"action", "install"},
               {"component_id", "later.failure"},
               {"version", "1.0.0"},
               {"enabled", true},
               {"artifact",
                {{"type", "file_set"},
                 {"source", "missing.file-set.json"},
                 {"sha256", std::string(64, '0')},
                 {"size_bytes", 1},
                 {"file_count", 1},
                 {"total_size_bytes", 1}}}}})},
    };
    REQUIRE_THROWS(SetupEngine::apply(json::object(), plan, paths, true, false));
    REQUIRE(fs::is_regular_file(tablebases / "existing.rtbw"));
    std::ifstream prior(tablebases / "existing.rtbw", std::ios::binary);
    REQUIRE(std::string(std::istreambuf_iterator<char>(prior),
                        std::istreambuf_iterator<char>()) == "operator-state");
    prior.close();
    REQUIRE_FALSE(fs::exists(tablebases / "KQvK.rtbw"));
    REQUIRE_FALSE(fs::exists(paths.data_root / "locators" / "syzygy.json"));
    REQUIRE_FALSE(fs::exists(paths.data_root / "receipts" / "replacement.tables.json"));
    REQUIRE_FALSE(fs::exists(paths.install_root / "components" / "replacement.tables" /
                             "1.0.0"));
    fs::remove_all(root);
}

TEST_CASE("inventory returns bounded versioned discovery and browse contracts") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const fs::path provider_root = root / "providers";
    const fs::path book_forge = provider_root / "book-forge";
    fs::create_directories(book_forge);
    std::ofstream(book_forge / "arena-component.json")
        << json{
               {"schema", "arena_install_component_v1"},
               {"component_id", "replacement.forge"},
               {"component_version", "1.0.0"},
               {"files", json::array()},
               {"discovery",
                json::array({{{"kind", "book_forge"},
                              {"locator", {{"source", "active_root"}}}}})},
           }
               .dump();

    const fs::path too_deep = provider_root / "nested" / "not-scanned";
    fs::create_directories(too_deep);
    std::ofstream(too_deep / "arena-component.json")
        << json{
               {"schema", "arena_install_component_v1"},
               {"component_id", "replacement.evidence"},
               {"component_version", "1.0.0"},
               {"files", json::array()},
               {"discovery",
                json::array({{{"kind", "evidence_service"},
                              {"locator", {{"source", "active_root"}}}}})},
           }
               .dump();

    const fs::path misleading_name = provider_root / "legacy-name-only";
    fs::create_directories(misleading_name);
    std::ofstream(misleading_name / "arena-component.json")
        << json{
               {"schema", "arena_install_component_v1"},
               {"component_id", "book_forge.service"},
               {"component_version", "99.0.0"},
               {"files", json::array()},
           }
               .dump();

    const json inventory = SetupEngine::inventory(paths, {provider_root});
    const json& book = inventory.at("discovery").at("book_forge");
    REQUIRE(book.at("schema") == "arena_setup_discovery_v1");
    REQUIRE(book.at("schema_version") == 1);
    REQUIRE(book.at("selection").at("source") == "provider_manifest");
    REQUIRE(std::none_of(book.at("candidates").begin(), book.at("candidates").end(),
                         [](const json& candidate) {
                             return candidate.value("details", json::object())
                                        .value("component_id", "") == "book_forge.service";
                         }));
    REQUIRE(book.at("browse").at("selection") == "file_or_directory");
    REQUIRE_FALSE(book.at("browse").at("hints").empty());
    REQUIRE(inventory.at("discovery").at("opening_book").at("browse").at("managed_copy_allowed") ==
            true);

    const auto& evidence_candidates =
        inventory.at("discovery").at("evidence_service").at("candidates");
    REQUIRE(std::none_of(evidence_candidates.begin(), evidence_candidates.end(),
                         [&too_deep](const json& candidate) {
                             return candidate.value("path", "") ==
                                    fs::absolute(too_deep).lexically_normal().string();
                         }));
    fs::remove_all(root);
}

TEST_CASE("selected asset roots discover books and Syzygy one level only") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const fs::path asset_root = root / "mounted-assets";
    const fs::path first_books = asset_root / "a-books";
    const fs::path second_books = asset_root / "z-books";
    const fs::path syzygy = asset_root / "syzygy";
    const fs::path too_deep = asset_root / "nested" / "too-deep";
    fs::create_directories(first_books);
    fs::create_directories(second_books);
    fs::create_directories(syzygy);
    fs::create_directories(too_deep);
    std::ofstream(first_books / "first.bin") << "book-one";
    std::ofstream(second_books / "second.book") << "book-two-with-more-coverage";
    std::ofstream(syzygy / "KQvK.rtbw") << "tablebase";
    std::ofstream(too_deep / "hidden.rtbz") << "not-discovered";

    const json inventory = SetupEngine::inventory(paths, {asset_root});
    const auto& discovery = inventory.at("discovery");
    REQUIRE(discovery.at("opening_book").at("selection").at("source") == "discovered");
    REQUIRE(discovery.at("opening_book").at("selection").at("path") ==
            fs::absolute(second_books).lexically_normal().string());
    REQUIRE(discovery.at("syzygy").at("selection").at("path") ==
            fs::absolute(syzygy).lexically_normal().string());
    REQUIRE(inventory.at("knowledge").at("opening_book").at("state") ==
            "detected_unmanaged");
    REQUIRE(inventory.at("knowledge").at("opening_book").at("path") ==
            fs::absolute(second_books).lexically_normal().string());
    REQUIRE(inventory.at("knowledge").at("syzygy").at("state") ==
            "detected_unmanaged");
    REQUIRE(inventory.at("knowledge").at("syzygy").at("path") ==
            fs::absolute(syzygy).lexically_normal().string());
    REQUIRE(inventory.at("knowledge").at("syzygy").at("wdl_files") == 1);
    REQUIRE(inventory.at("knowledge").at("syzygy").at("dtz_files") == 0);
    REQUIRE(inventory.at("knowledge").at("syzygy").at("bytes") == 9);
    const auto& syzygy_candidates = discovery.at("syzygy").at("candidates");
    REQUIRE(std::none_of(syzygy_candidates.begin(), syzygy_candidates.end(),
                         [&too_deep](const json& candidate) {
                             return candidate.value("path", "") ==
                                    fs::absolute(too_deep).lexically_normal().string();
                         }));

    fs::remove_all(root);
}

TEST_CASE("indexed discovery finds reusable knowledge and Lichess evidence") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const fs::path external = root / "external-assets";
    const fs::path syzygy = external / "syzygy-3-5";
    fs::create_directories(syzygy);
    const fs::path book = external / "operator.book";
    const fs::path binary_decoy = external / "not-a-polyglot.bin";
    const fs::path table = syzygy / "KQvK.rtbw";
    const fs::path timing = external / "old-timing-profile.json";
    const fs::path single = write_lichess_single_profile(external);
    const fs::path capacity =
        external / "uci_arena_lichess_capacity_profile_v1.json";
    const fs::path transport = external / "transport_profile_v2.json";
    std::ofstream(book, std::ios::binary) << "book";
    std::ofstream(binary_decoy, std::ios::binary) << "not a book";
    std::ofstream(table, std::ios::binary) << "tablebase";
    std::ofstream(timing) << json{{"schema", "uci_time_profile_v2"},
                                  {"qualified", true}}
                                 .dump();
    std::ofstream(capacity)
        << json{{"schema", "uci_arena_lichess_capacity_profile_v1"},
                {"qualified", true},
                {"maximum_concurrency", 2},
                {"evidence", {{"single_game", true}}}}
               .dump();
    std::ofstream(transport)
        << json{{"schema", "uci_arena_lichess_transport_profile_v2"},
                {"qualified", true},
                {"sample_count", 635},
                {"game_count", 24}}
               .dump();

    arena::ports::IndexedAssetSearchResult result;
    result.adapter = "fake_system_index";
    result.state = arena::ports::IndexedSearchState::complete;
    result.maximum_results = 256;
    result.elapsed = std::chrono::milliseconds(7);
    result.detail_code = "complete";
    result.candidates = {
        {arena::ports::IndexedAssetKind::opening_book, book},
        {arena::ports::IndexedAssetKind::opening_book, binary_decoy},
        {arena::ports::IndexedAssetKind::syzygy, table},
        {arena::ports::IndexedAssetKind::timing_profile, timing},
        {arena::ports::IndexedAssetKind::lichess_single_game_profile, single},
        {arena::ports::IndexedAssetKind::lichess_capacity_profile, capacity},
        {arena::ports::IndexedAssetKind::lichess_transport_profile, transport},
        {arena::ports::IndexedAssetKind::syzygy, external / "missing.rtbz"},
    };
    FakeAssetSearchIndex index(std::move(result));

    const json inventory = SetupEngine::inventory(paths, {}, &index);
    const json& discovery = inventory.at("discovery");
    REQUIRE(index.last_query.maximum_results == 256);
    REQUIRE(index.last_query.timeout == std::chrono::milliseconds(2000));
    REQUIRE(index.last_query.rules.size() == 6);
    REQUIRE(arena::ports::valid_indexed_asset_query(index.last_query));
    auto underfunded_query = index.last_query;
    underfunded_query.maximum_results = underfunded_query.rules.size() - 1;
    REQUIRE_FALSE(arena::ports::valid_indexed_asset_query(underfunded_query));
    REQUIRE(discovery.at("opening_book").at("selection").at("source") == "indexed");
    REQUIRE(discovery.at("opening_book").at("selection").at("path") ==
            fs::absolute(book).lexically_normal().string());
    REQUIRE(discovery.at("syzygy").at("selection").at("path") ==
            fs::absolute(syzygy).lexically_normal().string());
    const json& timing_candidate =
        discovery.at("timing_profile").at("candidates").at(0);
    REQUIRE(timing_candidate.at("details").at("validation").at("classification") ==
            "legacy_parseable");
    for (const auto kind : {"lichess_single_game_profile",
                            "lichess_capacity_profile",
                            "lichess_transport_profile"}) {
        const json& candidate = discovery.at(kind).at("candidates").at(0);
        REQUIRE(candidate.at("source") == "indexed");
        REQUIRE(candidate.at("details").at("validation").at("classification") ==
                "qualified_declared");
        REQUIRE(discovery.at(kind).at("indexed_search").at("accepted_candidates") == 1);
    }
    const json& search = discovery.at("syzygy").at("indexed_search");
    REQUIRE(search.at("state") == "complete");
    REQUIRE(search.at("ownership") == "external_dependency");
    REQUIRE(search.at("registration") == "explicit_import_required");
    REQUIRE(search.at("accepted_candidates") == 1);
    REQUIRE(search.at("rejected_candidates") == 1);
    const json& book_search = discovery.at("opening_book").at("indexed_search");
    REQUIRE(book_search.at("accepted_candidates") == 1);
    REQUIRE(book_search.at("rejected_candidates") == 1);
    REQUIRE_FALSE(fs::exists(paths.data_root / "locators"));

    const json bounded = SetupEngine::inventory(paths, {external});
    for (const auto kind : {"timing_profile", "lichess_single_game_profile",
                            "lichess_capacity_profile",
                            "lichess_transport_profile"}) {
        REQUIRE(bounded.at("discovery")
                    .at(kind)
                    .at("selection")
                    .at("source") == "discovered");
    }
    REQUIRE(bounded.at("discovery").at("opening_book").at("selection").at("path") ==
            fs::absolute(book).lexically_normal().string());
    fs::remove_all(root);
}

TEST_CASE("saved knowledge locator outranks an indexed candidate") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const fs::path saved = root / "saved.book";
    const fs::path indexed = root / "indexed.book";
    std::ofstream(saved) << "saved";
    std::ofstream(indexed) << "indexed";
    const json imported = SetupEngine::import_path(paths, "opening_book", saved);
    REQUIRE(imported.at("mode") == "in_place_reference");
    std::ofstream(saved, std::ios::app) << "-updated-after-selection";

    arena::ports::IndexedAssetSearchResult result;
    result.adapter = "fake_system_index";
    result.state = arena::ports::IndexedSearchState::complete;
    result.maximum_results = 256;
    result.detail_code = "complete";
    result.candidates = {
        {arena::ports::IndexedAssetKind::opening_book, indexed},
    };
    FakeAssetSearchIndex index(std::move(result));
    const json inventory = SetupEngine::inventory(paths, {}, &index);
    REQUIRE(inventory.at("discovery")
                .at("opening_book")
                .at("selection")
                .at("source") == "saved_locator");
    REQUIRE(inventory.at("discovery")
                .at("opening_book")
                .at("selection")
                .at("details")
                .at("validation")
                .at("book_bytes") == fs::file_size(saved));
    REQUIRE(inventory.at("discovery")
                .at("opening_book")
                .at("candidates")
                .size() == 2);
    fs::remove_all(root);
}

TEST_CASE("unavailable file index is visible and does not fail inventory") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    arena::ports::IndexedAssetSearchResult result;
    result.adapter = "fake_system_index";
    result.state = arena::ports::IndexedSearchState::unavailable;
    result.maximum_results = 256;
    result.detail_code = "index_disabled";
    FakeAssetSearchIndex index(std::move(result));
    const json inventory = SetupEngine::inventory(paths, {}, &index);
    REQUIRE(inventory.at("discovery")
                .at("timing_profile")
                .at("indexed_search")
                .at("state") == "unavailable");
    REQUIRE(inventory.at("discovery")
                .at("timing_profile")
                .at("selection")
                .at("source") == "unresolved");
    fs::remove_all(root);
}

TEST_CASE("saved in-place Syzygy locator is projected into knowledge inventory") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const fs::path syzygy = root / "mounted-syzygy";
    fs::create_directories(syzygy);
    std::ofstream(syzygy / "KQvK.RTBW") << "wdl";
    std::ofstream(syzygy / "KQvK.rtbz") << "dtz-data";

    const json imported = SetupEngine::import_path(paths, "syzygy", syzygy);
    REQUIRE(imported.at("mode") == "in_place_reference");
    const json inventory = SetupEngine::inventory(paths);
    const json& knowledge = inventory.at("knowledge").at("syzygy");
    REQUIRE(knowledge.at("state") == "detected_unmanaged");
    REQUIRE(knowledge.at("source") == "saved_locator");
    REQUIRE(knowledge.at("path") == fs::absolute(syzygy).lexically_normal().string());
    REQUIRE(knowledge.at("wdl_files") == 1);
    REQUIRE(knowledge.at("dtz_files") == 1);
    REQUIRE(knowledge.at("bytes") == 11);

    fs::remove_all(root);
}

TEST_CASE("discovery precedence is locator then receipt then provider manifest") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const fs::path provider_root =
        paths.install_root / "components" / "replacement.forge" / "1.0.0";
    fs::create_directories(provider_root);
    std::ofstream(provider_root / "arena-component.json")
        << json{
               {"schema", "arena_install_component_v1"},
               {"component_id", "replacement.forge"},
               {"component_version", "1.0.0"},
               {"files", json::array()},
               {"discovery",
                json::array({{{"kind", "book_forge"},
                              {"locator", {{"source", "active_root"}}}}})},
           }
               .dump();
    const fs::path cli = provider_root / "book-forge.mjs";
    std::ofstream(cli) << "fixture";

    auto inventory = SetupEngine::inventory(paths, {provider_root});
    REQUIRE(inventory.at("discovery").at("book_forge").at("selection").at("source") ==
            "provider_manifest");

    const json imported = SetupEngine::import_path(paths, "book_forge", cli);
    REQUIRE(imported.at("mode") == "in_place_reference");
    inventory = SetupEngine::inventory(paths, {provider_root});
    REQUIRE(inventory.at("discovery").at("book_forge").at("selection").at("source") ==
            "saved_locator");

    const fs::path receipts = paths.data_root / "receipts";
    fs::create_directories(receipts);
    std::ofstream(receipts / "replacement.forge.json")
        << json{
               {"schema", "arena_install_receipt_v1"},
               {"component_id", "replacement.forge"},
               {"component_version", "1.0.0"},
               {"state", "installed_managed"},
               {"active_root", provider_root.string()},
           }
               .dump();
    inventory = SetupEngine::inventory(paths, {provider_root});
    REQUIRE(inventory.at("discovery").at("book_forge").at("selection").at("source") ==
            "saved_locator");
    REQUIRE(std::ranges::any_of(
        inventory.at("discovery").at("book_forge").at("candidates"),
        [](const json& candidate) {
            return candidate.value("source", "") == "install_receipt" &&
                   candidate.value("state", "") == "installed_managed";
        }));
    fs::remove_all(root);
}

TEST_CASE("local imports validate UCI and never return Lichess token contents") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const json engine = SetupEngine::import_path(paths, "uci_engine", ARENA_MOCK_UCI_PATH);
    REQUIRE(engine.at("schema") == "arena_setup_import_result_v1");
    REQUIRE(engine.at("validation").at("engine_name") == "Arena Mock Engine");
    REQUIRE(engine.at("validation").at("fingerprint").get<std::string>().size() == 64);

    const fs::path token = root / "lichess-token.txt";
    const std::string token_contents = "TEST_TOKEN_CONTENT_MUST_NOT_ESCAPE";
    std::ofstream(token) << token_contents;
    const json token_result = SetupEngine::import_path(paths, "lichess_token_path", token);
    REQUIRE(token_result.at("validation").at("validation") == "path_reference_only");
    REQUIRE(token_result.dump().find(token_contents) == std::string::npos);
    REQUIRE_THROWS(SetupEngine::import_path(paths, "restaurant", root));
    fs::remove_all(root);
}

TEST_CASE("dashboard token reference save persists only path metadata") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const fs::path token = root / "operator-token.txt";
    const std::string sentinel = "TOKEN_VALUE_MUST_NOT_ESCAPE";
    std::ofstream(token) << sentinel;

    const json result =
        SetupEngine::save_lichess_token_reference(paths.data_root, token);
    REQUIRE(result.at("schema") ==
            "arena_bot_token_reference_result_v1");
    REQUIRE(result.at("state") == "saved");
    REQUIRE(result.dump().find(sentinel) == std::string::npos);
    REQUIRE(result.dump().find(token.string()) == std::string::npos);

    const fs::path locator =
        paths.data_root / "locators" / "lichess_token_path.json";
    REQUIRE(fs::is_regular_file(locator));
    std::ifstream input(locator);
    const json metadata = json::parse(input);
    REQUIRE(metadata.at("validation").at("validation") ==
            "path_reference_only");
    REQUIRE(metadata.at("path") ==
            fs::absolute(token).lexically_normal().string());
    REQUIRE(metadata.dump().find(sentinel) == std::string::npos);
    input.close();

    const fs::path manager_owned_token =
        paths.data_root / "credentials" / "lichess.token";
    fs::create_directories(manager_owned_token.parent_path());
    std::ofstream(manager_owned_token) << "NON_SECRET_FIXTURE_VALUE";
    const auto require_manager_owned_rejection = [](const auto& operation) {
        try {
            operation();
            FAIL("Manager-owned credential path was accepted");
        } catch (const std::runtime_error& error) {
            REQUIRE(
                std::string(error.what()).find("outside Manager data") !=
                std::string::npos);
        }
    };
    require_manager_owned_rejection([&] {
        static_cast<void>(SetupEngine::save_lichess_token_reference(
            paths.data_root, manager_owned_token));
    });
    require_manager_owned_rejection([&] {
        static_cast<void>(SetupEngine::import_path(
            paths, "lichess_token_path", manager_owned_token));
    });
    fs::remove_all(root);
}

TEST_CASE("locator imports refresh only manifest-declared provider consumers") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const fs::path configuration = write_mock_renderer_consumer(
        paths, "replaceable.token.consumer", "lichess_token_path");

    const fs::path token = root / "lichess-token.txt";
    const std::string token_contents = "TEST_TOKEN_CONTENT_MUST_NOT_ESCAPE";
    std::ofstream(token) << token_contents;
    const json imported = SetupEngine::import_path(paths, "lichess_token_path", token);
    REQUIRE(imported.at("refreshed_consumers") ==
            json::array({"replaceable.token.consumer"}));
    std::ifstream generated_input(configuration);
    json generated;
    generated_input >> generated;
    REQUIRE(generated.at("locators").at("selected") ==
            fs::absolute(token).lexically_normal().string());
    REQUIRE(generated.dump().find(token_contents) == std::string::npos);
    const fs::path projection = configuration.parent_path() /
                                "generated" / "context-projection.json";
    REQUIRE(fs::is_regular_file(projection));
    REQUIRE(json::parse(std::ifstream(projection))
                .at("locators")
                .at("selected") == fs::absolute(token).lexically_normal().string());
    generated_input.close();
    fs::remove_all(root);
}

TEST_CASE("explicit saved knowledge selection does not erase installed authority") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const fs::path bundled = root / "bundled.book";
    const fs::path selected = root / "selected.book";
    std::ofstream(bundled) << "bootstrap";
    std::ofstream(selected) << "operator selection with stronger retained evidence";

    const json managed =
        SetupEngine::import_path(paths, "opening_book", bundled, true);
    REQUIRE(managed.at("state") == "installed_managed");
    static_cast<void>(
        SetupEngine::import_path(paths, "opening_book", selected, false));

    const json inventory = SetupEngine::inventory(paths);
    const json& discovery = inventory.at("discovery").at("opening_book");
    REQUIRE(discovery.at("selection").at("source") == "saved_locator");
    REQUIRE(discovery.at("selection").at("path") ==
            fs::absolute(selected).lexically_normal().string());
    REQUIRE(std::ranges::any_of(
        discovery.at("candidates"), [](const json& candidate) {
            return candidate.value("source", "") == "install_receipt" &&
                   candidate.value("state", "") == "installed_managed";
        }));
    fs::remove_all(root);
}

TEST_CASE("component names do not grant knowledge activation authority") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const std::string payload = "ordinary";
    const std::string manifest =
        std::string(R"({"schema":"arena_install_file_set_v1","schema_version":1,"component_id":"syzygy.misleading","component_version":"1.0.0","total_size_bytes":8,"files":[{"path":"payload.dat","size_bytes":8,"sha256":")") +
        sha256_text(payload) + R"(","source":"payload.dat"}]})";
    std::ofstream(root / "payload.dat", std::ios::binary) << payload;
    std::ofstream(root / "generic.file-set.json", std::ios::binary) << manifest;
    const json plan{
        {"schema", "arena_install_plan_v1"},
        {"blockers", json::array()},
        {"offline", true},
        {"catalog_directory", root.string()},
        {"actions", json::array({{{"action", "install"},
                                  {"component_id", "syzygy.misleading"},
                                  {"version", "1.0.0"},
                                  {"enabled", true},
                                  {"artifact",
                                   {{"type", "file_set"},
                                    {"source", "generic.file-set.json"},
                                    {"sha256", sha256_text(manifest)},
                                    {"size_bytes", manifest.size()},
                                    {"file_count", 1},
                                    {"total_size_bytes", payload.size()}}}}})},
    };
    const json applied = SetupEngine::apply(json::object(), plan, paths, true, false);
    REQUIRE(applied.at("components").at(0).at("activated_resources").empty());
    REQUIRE_FALSE(fs::exists(paths.knowledge_root / "tablebases" / "payload.dat"));
    REQUIRE_FALSE(fs::exists(paths.data_root / "locators" / "syzygy.json"));
    const json verified = SetupEngine::verify(paths);
    INFO(verified.dump(2));
    REQUIRE(verified.at("ok") == true);
    fs::remove_all(root);
}

TEST_CASE("Book Forge credential recovery accepts only typed ready responses") {
    const auto ready = assess_book_forge_credential_recovery({
        {"ok", true},
        {"retried", 0},
        {"acquisition", {{"credentialConfigured", true}, {"authState", "ready"}}},
    });
    REQUIRE(ready.ready);
    REQUIRE_FALSE(ready.retryable);
    REQUIRE(ready.retried == 0);

    const auto blocked = assess_book_forge_credential_recovery({
        {"ok", true},
        {"retried", 7U},
        {"acquisition", {{"credentialConfigured", true}, {"authState", "blocked"}}},
    });
    REQUIRE_FALSE(blocked.ready);
    REQUIRE(blocked.retryable);
    REQUIRE(blocked.retried == 7);
    REQUIRE(blocked.failure_detail == "authState remained blocked");

    const auto missing_credential = assess_book_forge_credential_recovery({
        {"ok", true},
        {"retried", 0},
        {"acquisition", {{"credentialConfigured", false}, {"authState", "blocked"}}},
    });
    REQUIRE_FALSE(missing_credential.ready);
    REQUIRE_FALSE(missing_credential.retryable);
    REQUIRE(missing_credential.failure_detail == "credentialConfigured was not true");

    const auto malformed_count = assess_book_forge_credential_recovery({
        {"ok", true},
        {"retried", -1},
        {"acquisition", {{"credentialConfigured", true}, {"authState", "ready"}}},
    });
    REQUIRE_FALSE(malformed_count.ready);
    REQUIRE_FALSE(malformed_count.retryable);
    REQUIRE(malformed_count.failure_detail == "retried was not a non-negative integer");
}

TEST_CASE("Book Forge credential recovery retries only a well-formed blocked state") {
    int calls = 0;
    const json recovered = recover_book_forge_credential(
        [&] {
            ++calls;
            return json{
                {"ok", true},
                {"retried", calls == 1 ? 4 : 0},
                {"acquisition",
                 {{"credentialConfigured", true},
                  {"authState", calls == 1 ? "blocked" : "ready"}}},
            };
        },
        3, std::chrono::milliseconds::zero());
    REQUIRE(calls == 2);
    REQUIRE(recovered.at("attempts") == 2);
    REQUIRE(recovered.at("retried") == 4);
    REQUIRE(recovered.at("auth_state") == "ready");

    calls = 0;
    REQUIRE_THROWS(recover_book_forge_credential(
        [&] {
            ++calls;
            return json{
                {"ok", true},
                {"retried", 0},
                {"acquisition",
                 {{"credentialConfigured", false}, {"authState", "blocked"}}},
            };
        },
        3, std::chrono::milliseconds::zero()));
    REQUIRE(calls == 1);

    calls = 0;
    REQUIRE_THROWS(recover_book_forge_credential(
        [&] {
            ++calls;
            return json{
                {"ok", true},
                {"retried", 0},
                {"acquisition",
                 {{"credentialConfigured", true}, {"authState", "blocked"}}},
            };
        },
        3, std::chrono::milliseconds::zero()));
    REQUIRE(calls == 3);
}

TEST_CASE("disabled providers refresh through declared locator subscriptions") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const fs::path configuration = write_mock_renderer_consumer(
        paths, "replaceable.profile.consumer", "lichess_transport_profile");

    const fs::path transport = root / "transport_profile_v2.json";
    std::ofstream(transport)
        << json{
               {"schema", "uci_arena_lichess_transport_profile_v2"},
               {"qualified", true},
           }
               .dump(2);
    const json imported =
        SetupEngine::import_path(
            paths, "lichess_transport_profile", transport);
    REQUIRE(
        imported.at("refreshed_consumers") ==
        json::array({"replaceable.profile.consumer"}));
    std::ifstream updated_input(configuration);
    const json updated = json::parse(updated_input);
    REQUIRE(
        updated.at("locators").at("selected") ==
        fs::absolute(transport).lexically_normal().string());
    REQUIRE(updated.at("enabled") == false);
    updated_input.close();
    fs::remove_all(root);
}

TEST_CASE("provider startup cleanup uses only the declared startup control") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const std::string component_id = "replaceable.lifecycle.consumer";
    const fs::path configuration = write_mock_renderer_consumer(
        paths, component_id, "lichess_transport_profile");
    const fs::path workspace = configuration.parent_path();
    std::ofstream(workspace / "valuable-state.json") << "{}";

    const json plan = SetupEngine::plan_uninstall(paths, {component_id});
    REQUIRE(plan.at("components").at(0).at("startup_cleanup_required") == true);
    const json result = SetupEngine::uninstall(paths, plan, true);

    REQUIRE(result.at("components").at(0).at("startup_cleanup").at("installed") ==
            false);
    REQUIRE_FALSE(fs::exists(paths.install_root / "components" / component_id));
    REQUIRE(fs::is_regular_file(workspace / "valuable-state.json"));
    fs::remove_all(root);
}

TEST_CASE("timing profile imports retain qualified build provenance without granting authority") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const json identity{
        {"engine", {{"executable_sha256", "sha256:" + std::string(64, 'a')}}},
        {"model", {{"model_fingerprint", "42"}, {"model_id", "model-1"}}},
        {"search", {{"provider_effective", "cpu"}}},
    };
    json profile{
        {"schema", "uci_time_profile_v3"},
        {"qualified", true},
        {"gates", {{"passed", true}}},
        {"permissions",
         {{"provider_safety_qualified", true},
          {"forecast_shadow_qualified", false},
          {"adaptive_play_qualified", false}}},
        {"identity", identity},
        {"identity_sha256", "sha256:"
                            "fe440d1087301bf982f8e930212ef84b4809864abd49992c21b960962a91d15d"},
    };
    const fs::path profile_path = root / "uci_time_profile_v3.json";
    std::ofstream(profile_path) << profile.dump(2);

    const json imported = SetupEngine::import_path(paths, "timing_profile", profile_path);
    REQUIRE(imported.at("mode") == "in_place_reference");
    REQUIRE(imported.at("validation").at("validation") == "qualified_uci_time_profile_v3");
    REQUIRE(imported.at("validation").at("model_id") == "model-1");
    REQUIRE(imported.at("validation").at("provider") == "cpu");
    REQUIRE(imported.at("validation").at("qualified_engine_provenance_sha256") ==
            "sha256:" + std::string(64, 'a'));
    REQUIRE_FALSE(imported.at("validation").contains("engine_sha256"));
    const json inventory = SetupEngine::inventory(paths);
    REQUIRE(inventory.at("discovery").at("timing_profile").at("selection").at("source") ==
            "saved_locator");

    profile["permissions"]["provider_safety_qualified"] = false;
    const fs::path unsafe_profile_path = root / "unsafe-profile.json";
    std::ofstream(unsafe_profile_path) << profile.dump(2);
    REQUIRE_THROWS(SetupEngine::import_path(paths, "timing_profile", unsafe_profile_path));

    profile["permissions"]["provider_safety_qualified"] = true;
    profile["identity_sha256"] = "sha256:" + std::string(64, '0');
    const fs::path drifted_profile_path = root / "drifted-profile.json";
    std::ofstream(drifted_profile_path) << profile.dump(2);
    REQUIRE_THROWS(SetupEngine::import_path(paths, "timing_profile", drifted_profile_path));
    fs::remove_all(root);
}

TEST_CASE("Lichess safety profiles preserve qualification and manage the evidence closure") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const std::array profiles{
        std::tuple{
            "lichess_single_game_profile",
            "uci_arena_single_game_profile_v1",
            "uci_arena_single_game_profile_v1.json",
        },
        std::tuple{
            "lichess_capacity_profile",
            "uci_arena_lichess_capacity_profile_v1",
            "uci_arena_lichess_capacity_profile_v1.json",
        },
        std::tuple{
            "lichess_transport_profile",
            "uci_arena_lichess_transport_profile_v2",
            "transport_profile_v2.json",
        },
    };
    for (const auto& [kind, schema, filename] : profiles) {
        const fs::path profile_path =
            std::string(kind) == "lichess_single_game_profile"
                ? write_lichess_single_profile(root, filename)
                : root / filename;
        if (std::string(kind) != "lichess_single_game_profile") {
            std::ofstream(profile_path)
                << json{{"schema", schema}, {"qualified", true}}.dump(2);
        }
        const json imported = SetupEngine::import_path(paths, kind, profile_path);
        REQUIRE(imported.at("mode") ==
                (std::string(kind) == "lichess_single_game_profile"
                     ? "managed_copy"
                     : "in_place_reference"));
        REQUIRE(imported.at("validation").at("validation") ==
                "qualified_" + std::string(schema));
        if (std::string(kind) == "lichess_single_game_profile") {
            REQUIRE(imported.at("path") != fs::absolute(profile_path).string());
            REQUIRE(fs::is_regular_file(imported.at("path").get<std::string>()));
            REQUIRE(imported.at("validation").at("evidence_files") == 2);
        }
    }

    const json inventory = SetupEngine::inventory(paths);
    for (const auto& [kind, schema, filename] : profiles) {
        (void)schema;
        (void)filename;
        const json& discovered = inventory.at("discovery").at(kind);
        REQUIRE(discovered.at("selection").at("source") == "saved_locator");
        REQUIRE_FALSE(discovered.at("browse").at("filters").empty());
        REQUIRE_FALSE(discovered.at("browse").at("hints").empty());
    }

    const fs::path unqualified = root / "unqualified.json";
    std::ofstream(unqualified)
        << json{
               {"schema", "uci_arena_lichess_capacity_profile_v1"},
               {"qualified", false},
           }
               .dump(2);
    REQUIRE_THROWS(
        SetupEngine::import_path(paths, "lichess_capacity_profile", unqualified));
    fs::remove_all(root);
}

TEST_CASE("Lichess installation is disabled without blocking on onboarding evidence") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    json lichess = component("lichess.adapter");
    lichess["default_enabled"] = false;
    const json catalog{
        {"schema", "arena_install_catalog_v1"},
        {"schema_version", 1},
        {"suite_version", "1.0.0"},
        {"presets", {{"manager_only", json::array({"manager.core"})}}},
        {"components",
         json::array({component("manager.core"), lichess})},
    };
    const json disabled =
        SetupEngine::plan(catalog, paths, "custom", {"lichess.adapter"}, true, false);
    REQUIRE(disabled.at("blockers").empty());
    const auto adapter = std::find_if(
        disabled.at("actions").begin(), disabled.at("actions").end(),
        [](const json& action) {
            return action.value("component_id", "") == "lichess.adapter";
        });
    REQUIRE(adapter != disabled.at("actions").end());
    REQUIRE(adapter->at("enabled") == false);

    const json identity{
        {"engine", {{"executable_sha256", "sha256:" + std::string(64, 'a')}}},
        {"model", {{"model_fingerprint", "42"}, {"model_id", "model-1"}}},
        {"search", {{"provider_effective", "cpu"}}},
    };
    const fs::path timing = root / "uci_time_profile_v3.json";
    std::ofstream(timing)
        << json{
               {"schema", "uci_time_profile_v3"},
               {"qualified", true},
               {"gates", {{"passed", true}}},
               {"permissions",
                {{"provider_safety_qualified", true},
                 {"forecast_shadow_qualified", false},
                 {"adaptive_play_qualified", false}}},
               {"identity", identity},
               {"identity_sha256",
                "sha256:"
                "fe440d1087301bf982f8e930212ef84b4809864abd49992c21b960962a91d15d"},
           }
               .dump(2);
    static_cast<void>(SetupEngine::import_path(paths, "timing_profile", timing));
    for (const auto& [kind, schema, filename] :
         std::array{
             std::tuple{
                 "lichess_single_game_profile",
                 "uci_arena_single_game_profile_v1",
                 "uci_arena_single_game_profile_v1.json",
             },
             std::tuple{
                 "lichess_capacity_profile",
                 "uci_arena_lichess_capacity_profile_v1",
                 "uci_arena_lichess_capacity_profile_v1.json",
             },
             std::tuple{
                 "lichess_transport_profile",
                 "uci_arena_lichess_transport_profile_v2",
                 "transport_profile_v2.json",
             },
         }) {
        const fs::path profile_path =
            std::string(kind) == "lichess_single_game_profile"
                ? write_lichess_single_profile(root, filename)
                : root / filename;
        if (std::string(kind) != "lichess_single_game_profile") {
            std::ofstream(profile_path)
                << json{{"schema", schema}, {"qualified", true}}.dump(2);
        }
        static_cast<void>(SetupEngine::import_path(paths, kind, profile_path));
    }
    const json ready =
        SetupEngine::plan(catalog, paths, "custom", {"lichess.adapter"}, true, false);
    REQUIRE(ready.at("blockers").empty());
    const auto still_disabled = std::find_if(
        ready.at("actions").begin(), ready.at("actions").end(),
        [](const json& action) {
            return action.value("component_id", "") == "lichess.adapter";
        });
    REQUIRE(still_disabled != ready.at("actions").end());
    REQUIRE(still_disabled->at("enabled") == false);
    fs::remove_all(root);
}

TEST_CASE("managed copy is restricted to small flat opening books") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const fs::path source = root / "small-book";
    fs::create_directories(source);
    std::ofstream(source / "book.bin") << "polyglot";
    std::ofstream(source / "book.policy") << "policy";

    const json imported = SetupEngine::import_path(paths, "opening_book", source, true);
    REQUIRE(imported.at("mode") == "managed_copy");
    REQUIRE(imported.at("state") == "installed_managed");
    REQUIRE(fs::is_regular_file(fs::path(imported.at("path").get<std::string>()) / "book.bin"));
    REQUIRE(SetupEngine::verify(paths).at("ok") == true);

    const json inventory = SetupEngine::inventory(paths);
    REQUIRE(inventory.at("discovery").at("opening_book").at("selection").at("source") ==
            "install_receipt");
    REQUIRE(inventory.at("knowledge").at("opening_book").at("state") ==
            "installed_managed");
    const fs::path managed_root = imported.at("path").get<std::string>();
    const std::string managed_id =
        imported.at("receipt").at("component_id").get<std::string>();
    const json uninstall_plan =
        SetupEngine::plan_uninstall(paths, {managed_id});
    REQUIRE(uninstall_plan.at("components").at(0).at("remove_active_root") == false);
    const json uninstall_result =
        SetupEngine::uninstall(paths, uninstall_plan, true);
    REQUIRE(uninstall_result.at("components").at(0).at("data_preserved") == true);
    REQUIRE(fs::is_regular_file(managed_root / "book.bin"));
    REQUIRE_FALSE(fs::exists(paths.data_root / "receipts" / (managed_id + ".json")));
    const json purge_plan = SetupEngine::plan_purge(paths, {managed_id});
    REQUIRE(purge_plan.at("blockers").empty());
    static_cast<void>(SetupEngine::purge(paths, purge_plan, true));
    REQUIRE_FALSE(fs::exists(managed_root));
    REQUIRE_THROWS(SetupEngine::import_path(paths, "syzygy", source, true));

    const fs::path nested = root / "nested-book";
    fs::create_directories(nested / "child");
    std::ofstream(nested / "book.bin") << "polyglot";
    REQUIRE_THROWS(SetupEngine::import_path(paths, "opening_book", nested, true));

    const fs::path too_many = root / "too-many-book-files";
    fs::create_directories(too_many);
    for (int index = 0; index < 65; ++index) {
        std::ofstream(too_many / ("book-" + std::to_string(index) + ".bin")) << "fixture";
    }
    REQUIRE_THROWS(SetupEngine::import_path(paths, "opening_book", too_many, true));
    fs::remove_all(root);
}

TEST_CASE("import rejects symbolic-link traversal when supported") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const fs::path target = root / "book.bin";
    const fs::path link = root / "book-link.bin";
    std::ofstream(target) << "polyglot";
    std::error_code error;
    fs::create_symlink(target, link, error);
    if (!error) {
        REQUIRE_THROWS(SetupEngine::import_path(paths, "opening_book", link));
    }
    fs::remove_all(root);
}

TEST_CASE("unsigned catalogs require an explicit development override") {
    const fs::path root = temporary_directory();
    const fs::path catalog_path = root / "catalog.json";
    std::ofstream(catalog_path)
        << R"({"schema":"arena_install_catalog_v1","schema_version":1,"suite_version":"1.0.0","presets":{},"components":[]})";
    REQUIRE_THROWS(SetupEngine::load_catalog(catalog_path));
    REQUIRE(SetupEngine::load_catalog(catalog_path, {}, true).at("suite_version") == "1.0.0");
    fs::remove_all(root);
}

TEST_CASE("unsafe retained release media fails preflight before installation mutation") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const fs::path media = root / "installer-media";
    const fs::path external_payload = root / "external-manager.tar.gz";
    const fs::path working_marker = paths.install_root / "working-installation.marker";
    fs::create_directories(media);
    fs::create_directories(paths.install_root);
    std::ofstream(external_payload) << "payload";
    std::ofstream(working_marker) << "working";
    const json catalog{
        {"schema", "arena_install_catalog_v1"},
        {"schema_version", 1},
        {"suite_version", "1.0.0"},
        {"presets", {{"manager_only", json::array({"manager.core"})}}},
        {"components", json::array({{{"id", "manager.core"},
                                     {"version", "1.0.0"},
                                     {"dependencies", json::array()},
                                     {"installed_bytes", 7},
                                     {"staging_bytes", 7},
                                     {"backup_bytes", 0},
                                     {"workspace_reserve_bytes", 0},
                                     {"artifacts",
                                      {{SetupEngine::platform_key(),
                                        {{"source", external_payload.string()},
                                         {"sha256", "239f59ed55e737c77147cf55ad0c1b030b6d7e"
                                                    "e748a7426952f9b852d5a935e5"},
                                         {"size_bytes", 7}}}}}}})},
    };
    const fs::path catalog_path = media / "catalog.json";
    std::ofstream(catalog_path) << catalog.dump(2);
    const json loaded = SetupEngine::load_catalog(catalog_path, {}, true);

    REQUIRE_THROWS(SetupEngine::validate_release_media(loaded, catalog_path, {}, true));
    REQUIRE(fs::is_regular_file(working_marker));
    REQUIRE_FALSE(fs::exists(paths.data_root / "release"));
    fs::remove_all(root);
}

TEST_CASE("verified release media is retained for later repair and downloads") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const fs::path media = root / "installer-media";
    fs::create_directories(media / "payloads");
    std::ofstream(media / "payloads" / "manager.tar.gz") << "payload";
    const json catalog{
        {"schema", "arena_install_catalog_v1"},
        {"schema_version", 1},
        {"suite_version", "1.0.0"},
        {"presets", {{"manager_only", json::array({"manager.core"})}}},
        {"components", json::array({{{"id", "manager.core"},
                                     {"version", "1.0.0"},
                                     {"dependencies", json::array()},
                                     {"installed_bytes", 7},
                                     {"staging_bytes", 7},
                                     {"backup_bytes", 0},
                                     {"workspace_reserve_bytes", 0},
                                     {"artifacts",
                                      {{SetupEngine::platform_key(),
                                        {{"source", "payloads/manager.tar.gz"},
                                         {"sha256", "239f59ed55e737c77147cf55ad0c1b030b6d7e"
                                                    "e748a7426952f9b852d5a935e5"},
                                         {"size_bytes", 7}}}}}}})},
    };
    std::ofstream(media / "catalog.json") << catalog.dump(2);
    const json loaded = SetupEngine::load_catalog(media / "catalog.json", {}, true);
    const json retained = SetupEngine::persist_release_media(
        loaded, paths, media / "catalog.json", {}, true);
    REQUIRE(retained.at("schema") == "arena_retained_release_media_v1");
    REQUIRE(fs::is_regular_file(paths.data_root / "release" / "catalog.json"));
    REQUIRE(fs::is_regular_file(paths.data_root / "release" / "payloads" / "manager.tar.gz"));
    REQUIRE(
        fs::is_regular_file(
            paths.data_root / "release" /
            "development-trust.json"));
    REQUIRE_THROWS(
        SetupEngine::load_catalog(
            media / "catalog.json",
            {},
            false,
            {},
            paths.data_root / "release" /
                "development-trust.json"));

    fs::remove_all(media);
    const json reloaded =
        SetupEngine::load_catalog(
            paths.data_root / "release" / "catalog.json",
            {},
            false,
            {},
            paths.data_root / "release" /
                "development-trust.json");
    REQUIRE(reloaded.at("suite_version") == "1.0.0");
    REQUIRE(
        reloaded.at("_catalog_trust") ==
        "retained_unsigned_development");

    std::ofstream(
        paths.data_root / "release" / "catalog.json",
        std::ios::app)
        << ' ';
    REQUIRE_THROWS(
        SetupEngine::load_catalog(
            paths.data_root / "release" / "catalog.json",
            {},
            false,
            {},
            paths.data_root / "release" /
                "development-trust.json"));
    fs::remove_all(root);
}

TEST_CASE("removal lifecycle accepts a replaceable startup adapter") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const std::string component_id = "replaceable.provider";
    const fs::path active_root =
        paths.install_root / "components" / component_id / "1.0.0";
    const fs::path workspace = paths.workspace_root / component_id;
    const fs::path receipts = paths.data_root / "receipts";
    fs::create_directories(active_root);
    fs::create_directories(workspace);
    fs::create_directories(receipts);
    std::ofstream(active_root / "payload") << "payload";
    std::ofstream(workspace / "state") << "preserved";
    std::ofstream(receipts / (component_id + ".json"))
        << json{
               {"schema", "arena_install_receipt_v1"},
               {"schema_version", 1},
               {"component_id", component_id},
               {"component_version", "1.0.0"},
               {"state", "installed_managed"},
               {"active_root", active_root.string()},
               {"artifact_sha256", std::string(64, 'a')},
               {"dependencies", json::array()},
               {"data_paths", json::array({workspace.string()})},
               {"activated_files", json::array()},
           }
               .dump(2);

    bool startup_invoked = false;
    RemovalLifecycleHooks hooks;
    hooks.startup_cleanup_required =
        [](const json&) { return true; };
    hooks.uninstall_startup = [&startup_invoked](const json& receipt) {
        startup_invoked = true;
        return json{{"component_id", receipt.at("component_id")},
                    {"installed", false},
                    {"adapter", "fake"}};
    };
    const json plan =
        RemovalLifecycle::plan_uninstall(paths, {component_id}, false, hooks);
    REQUIRE(plan.at("components").at(0).at("startup_cleanup_required") == true);
    const json result =
        RemovalLifecycle::uninstall(paths, plan, true, hooks);
    REQUIRE(startup_invoked);
    REQUIRE(result.at("components").at(0).at("startup_cleanup").at("adapter") ==
            "fake");
    REQUIRE_FALSE(fs::exists(active_root));
    REQUIRE(fs::is_regular_file(workspace / "state"));
    fs::remove_all(root);
}

TEST_CASE("uninstall planning fails closed on ambiguous receipt authority") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const fs::path active_root =
        paths.install_root / "components" / "identity.fixture" / "1.0.0";
    const fs::path receipts = paths.data_root / "receipts";
    fs::create_directories(active_root);
    fs::create_directories(receipts);
    std::ofstream(active_root / "payload") << "payload";
    std::ofstream(receipts / "wrong-name.json")
        << json{
               {"schema", "arena_install_receipt_v1"},
               {"schema_version", 1},
               {"component_id", "identity.fixture"},
               {"component_version", "1.0.0"},
               {"state", "installed_managed"},
               {"active_root", active_root.string()},
               {"artifact_sha256", std::string(64, 'a')},
               {"dependencies", json::array()},
               {"data_paths", json::array()},
               {"activated_files", json::array()},
           }
               .dump(2);
    REQUIRE_THROWS(SetupEngine::plan_uninstall(paths, {}, true));
    REQUIRE(fs::is_regular_file(active_root / "payload"));
    REQUIRE(fs::is_regular_file(receipts / "wrong-name.json"));
    fs::remove_all(root);
}

TEST_CASE("suite uninstall removes every receipt while preserving workspaces") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const fs::path receipts = paths.data_root / "receipts";
    fs::create_directories(receipts);
    const auto install = [&](const std::string& id, const json& dependencies,
                             const std::string& state = "installed_managed") {
        const fs::path component_root = paths.install_root / "components" / id / "1.0.0";
        fs::create_directories(component_root);
        std::ofstream(component_root / "payload") << id;
        std::ofstream(receipts / (id + ".json"))
            << json{
                   {"schema", "arena_install_receipt_v1"},
                   {"schema_version", 1},
                   {"component_id", id},
                   {"component_version", "1.0.0"},
                   {"state", state},
                   {"active_root", component_root.string()},
                   {"artifact_sha256", std::string(64, 'a')},
                   {"dependencies", dependencies},
                   {"data_paths", json::array()},
                   {"activated_files", json::array()},
               }
                   .dump(2);
        return component_root;
    };
    const fs::path manager = install("manager.core", json::array());
    const fs::path node = install("node_runtime.private", json::array());
    const fs::path provider =
        install("optional.provider", json::array({"node_runtime.private"}),
                "installed_disabled");
    const fs::path preserved = paths.workspace_root / "provider" / "state.json";
    fs::create_directories(preserved.parent_path());
    std::ofstream(preserved) << "{}";
    const std::array<fs::path, 8> retained_manager_state{
        paths.data_root / "arena.db",
        paths.data_root / "client.key",
        paths.data_root / "locators" / "opening_book.json",
        paths.data_root / "release" / "catalog.json",
        paths.data_root / "trust" / "tuf-versions.json",
        paths.data_root / "cache" / std::string(64, 'a'),
        paths.data_root / "transactions" / "completed.json",
        paths.knowledge_root / "opening_books" / "operator-book.bin",
    };
    for (const auto& path : retained_manager_state) {
        fs::create_directories(path.parent_path());
        std::ofstream(path) << "retained";
    }

    REQUIRE_THROWS(SetupEngine::plan_uninstall(paths, {"node_runtime.private"}));
    json plan = SetupEngine::plan_uninstall(paths, {}, true);
    REQUIRE(plan.at("schema") == "arena_install_uninstall_plan_v1");
    REQUIRE(fs::is_regular_file(manager / "payload"));
    REQUIRE(fs::is_regular_file(node / "payload"));
    REQUIRE(fs::is_regular_file(provider / "payload"));
    REQUIRE_THROWS(SetupEngine::uninstall(paths, plan, false));
    json altered_plan = plan;
    altered_plan.at("components").at(0).at("remove_active_root") = false;
    REQUIRE_THROWS(SetupEngine::uninstall(paths, altered_plan, true));
    std::ofstream(
        fs::path(plan.at("components").at(0).at("receipt_path").get<std::string>()),
        std::ios::app)
        << ' ';
    REQUIRE_THROWS(SetupEngine::uninstall(paths, plan, true));
    plan = SetupEngine::plan_uninstall(paths, {}, true);
    const json result = SetupEngine::uninstall(paths, plan, true);
    REQUIRE(result.at("all") == true);
    REQUIRE(result.at("components").size() == 3);
    REQUIRE_FALSE(fs::exists(manager));
    REQUIRE_FALSE(fs::exists(node));
    REQUIRE_FALSE(fs::exists(provider));
    REQUIRE_FALSE(fs::exists(paths.install_root / "components" / "manager.core"));
    REQUIRE_FALSE(fs::exists(paths.install_root / "components" / "node_runtime.private"));
    REQUIRE_FALSE(fs::exists(paths.install_root / "components" / "optional.provider"));
    REQUIRE(fs::is_regular_file(preserved));
    for (const auto& path : retained_manager_state) {
        REQUIRE(fs::is_regular_file(path));
    }
    REQUIRE(std::ranges::none_of(
        fs::directory_iterator(receipts),
        [](const fs::directory_entry& entry) { return entry.path().extension() == ".json"; }));
    const fs::path preservation =
        paths.data_root / "transactions" / "uninstall-preservation";
    REQUIRE(fs::is_directory(preservation));
    REQUIRE(std::ranges::distance(fs::directory_iterator(preservation),
                                  fs::directory_iterator{}) == 3);
    const json repeated_plan = SetupEngine::plan_uninstall(paths, {}, true);
    REQUIRE(repeated_plan.at("components").empty());
    REQUIRE(SetupEngine::uninstall(paths, repeated_plan, true)
                .at("components")
                .empty());
    fs::remove_all(root);
}

TEST_CASE("older bundle uninstall cannot remove receipts adopted by a newer bundle") {
    const fs::path root = temporary_directory();
    SetupPaths paths = paths_under(root);
    const fs::path receipts = paths.data_root / "receipts";
    const fs::path component_root = paths.install_root / "components" / "manager.core" / "1.0.0";
    fs::create_directories(receipts);
    fs::create_directories(component_root);
    std::ofstream(component_root / "payload") << "manager";
    std::ofstream(receipts / "manager.core.json")
        << json{
               {"schema", "arena_install_receipt_v1"},
               {"schema_version", 1},
               {"component_id", "manager.core"},
               {"component_version", "1.0.0"},
               {"state", "installed_managed"},
               {"active_root", component_root.string()},
               {"artifact_sha256", std::string(64, 'a')},
               {"bundle_owner", "{22222222-2222-2222-2222-222222222222}"},
               {"dependencies", json::array()},
               {"data_paths", json::array()},
               {"activated_files", json::array()},
           }
               .dump(2);

    paths.bundle_owner = "{11111111-1111-1111-1111-111111111111}";
    const json old_plan = SetupEngine::plan_uninstall(paths, {}, true);
    const json old_uninstall = SetupEngine::uninstall(paths, old_plan, true);
    REQUIRE(old_uninstall.at("components").empty());
    REQUIRE(old_uninstall.at("skipped_foreign_receipts") == 1);
    REQUIRE(fs::is_regular_file(component_root / "payload"));
    REQUIRE(fs::is_regular_file(receipts / "manager.core.json"));

    paths.bundle_owner = "{22222222-2222-2222-2222-222222222222}";
    const json current_plan = SetupEngine::plan_uninstall(paths, {}, true);
    const json current_uninstall = SetupEngine::uninstall(paths, current_plan, true);
    REQUIRE(current_uninstall.at("components").size() == 1);
    REQUIRE_FALSE(fs::exists(component_root));
    REQUIRE_FALSE(fs::exists(receipts / "manager.core.json"));
    fs::remove_all(root);
}

TEST_CASE("purge is separate bounded and preserves modified or external files") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const std::string component_id = "provider.fixture";
    const fs::path active_root =
        paths.install_root / "components" / component_id / "1.0.0";
    const fs::path workspace = paths.workspace_root / "provider-fixture";
    const fs::path unchanged = paths.knowledge_root / "fixture" / "unchanged.bin";
    const fs::path modified = paths.knowledge_root / "fixture" / "modified.bin";
    const fs::path external = root / "operator-owned" / "external.bin";
    fs::create_directories(active_root);
    fs::create_directories(workspace);
    fs::create_directories(unchanged.parent_path());
    fs::create_directories(external.parent_path());
    std::ofstream(active_root / "payload") << "payload";
    std::ofstream(workspace / "observations.db") << "valuable";
    std::ofstream(unchanged, std::ios::binary) << "unchanged";
    std::ofstream(modified, std::ios::binary) << "original";
    std::ofstream(external, std::ios::binary) << "external";
    const fs::path receipts = paths.data_root / "receipts";
    fs::create_directories(receipts);
    std::ofstream(receipts / (component_id + ".json"))
        << json{
               {"schema", "arena_install_receipt_v1"},
               {"schema_version", 1},
               {"component_id", component_id},
               {"component_version", "1.0.0"},
               {"state", "installed_managed"},
               {"active_root", active_root.string()},
               {"artifact_sha256", std::string(64, 'a')},
               {"dependencies", json::array()},
               {"ownership",
                json::array({
                    {{"path", workspace.string()},
                     {"ownership_class", "user_preserved"},
                     {"normal_uninstall", "preserve"},
                     {"purge", "explicit_recursive"},
                     {"kind", "directory_tree"}},
                    {{"path", unchanged.string()},
                     {"ownership_class", "generated"},
                     {"normal_uninstall", "preserve"},
                     {"purge", "exact_identity"},
                     {"kind", "file"},
                     {"size_bytes", 9},
                     {"sha256", sha256_text("unchanged")}},
                    {{"path", modified.string()},
                     {"ownership_class", "generated"},
                     {"normal_uninstall", "preserve"},
                     {"purge", "exact_identity"},
                     {"kind", "file"},
                     {"size_bytes", 8},
                     {"sha256", sha256_text("original")}},
                    {{"path", external.string()},
                     {"ownership_class", "external_dependency"},
                     {"normal_uninstall", "preserve"},
                     {"purge", "forbidden"},
                     {"kind", "file"}},
                })},
               {"data_paths", json::array()},
               {"activated_files", json::array()},
           }
               .dump(2);

    const json uninstall_plan =
        SetupEngine::plan_uninstall(paths, {component_id});
    REQUIRE(fs::is_regular_file(active_root / "payload"));
    REQUIRE(fs::is_regular_file(workspace / "observations.db"));
    REQUIRE_THROWS(SetupEngine::uninstall(paths, uninstall_plan, false));
    const json uninstall_result =
        SetupEngine::uninstall(paths, uninstall_plan, true);
    REQUIRE_FALSE(fs::exists(active_root));
    REQUIRE(fs::is_regular_file(workspace / "observations.db"));
    REQUIRE(fs::is_regular_file(unchanged));
    REQUIRE(fs::is_regular_file(modified));
    REQUIRE(fs::is_regular_file(external));
    REQUIRE(uninstall_result.at("components").at(0).at("data_preserved") == true);

    const json purge_plan = SetupEngine::plan_purge(paths, {component_id});
    REQUIRE(purge_plan.at("blockers").empty());
    REQUIRE(purge_plan.at("components").at(0).at("operations").size() == 3);
    REQUIRE(purge_plan.at("components").at(0).at("preserved").size() == 1);
    REQUIRE(fs::is_regular_file(workspace / "observations.db"));
    REQUIRE_THROWS(SetupEngine::purge(paths, purge_plan, false));
    const fs::path injected = paths.cache_root / "injected";
    fs::create_directories(injected);
    std::ofstream(injected / "sentinel") << "must remain";
    json altered_purge_plan = purge_plan;
    altered_purge_plan.at("components").at(0).at("operations").at(0).at("path") =
        injected.string();
    REQUIRE_THROWS(SetupEngine::purge(paths, altered_purge_plan, true));
    REQUIRE(fs::is_regular_file(injected / "sentinel"));
    std::ofstream(modified, std::ios::binary | std::ios::trunc) << "operator-modified";
    const json purge_result = SetupEngine::purge(paths, purge_plan, true);
    REQUIRE(purge_result.at("ok") == true);
    REQUIRE(purge_result.at("components").at(0).at("state") ==
            "purged_with_preserved_files");
    REQUIRE_FALSE(fs::exists(workspace));
    REQUIRE_FALSE(fs::exists(unchanged));
    REQUIRE(fs::is_regular_file(modified));
    REQUIRE(fs::is_regular_file(external));

    const json repeated_plan = SetupEngine::plan_purge(paths, {component_id});
    REQUIRE(repeated_plan.at("blockers").empty());
    const json repeated = SetupEngine::purge(paths, repeated_plan, true);
    REQUIRE(repeated.at("ok") == true);
    REQUIRE(fs::is_regular_file(modified));
    REQUIRE(fs::is_regular_file(external));
    fs::remove_all(root);
}

TEST_CASE("purge planning blocks ownership at a lifecycle root") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const std::string component_id = "unsafe.fixture";
    const fs::path active_root =
        paths.install_root / "components" / component_id / "1.0.0";
    const fs::path receipts = paths.data_root / "receipts";
    fs::create_directories(active_root);
    fs::create_directories(receipts);
    std::ofstream(active_root / "payload") << "payload";
    std::string forbidden_root = paths.knowledge_root.string();
#ifdef _WIN32
    std::ranges::transform(
        forbidden_root, forbidden_root.begin(), [](const unsigned char character) {
            return static_cast<char>(std::toupper(character));
        });
#endif
    std::ofstream(receipts / (component_id + ".json"))
        << json{
               {"schema", "arena_install_receipt_v1"},
               {"schema_version", 1},
               {"component_id", component_id},
               {"component_version", "1.0.0"},
               {"state", "installed_managed"},
               {"active_root", active_root.string()},
               {"artifact_sha256", std::string(64, 'a')},
               {"dependencies", json::array()},
               {"ownership",
                json::array({{{"path", forbidden_root},
                              {"ownership_class", "user_preserved"},
                              {"normal_uninstall", "preserve"},
                              {"purge", "explicit_recursive"},
                              {"kind", "directory_tree"}}})},
               {"data_paths", json::array()},
               {"activated_files", json::array()},
           }
               .dump(2);
    const json uninstall_plan =
        SetupEngine::plan_uninstall(paths, {component_id});
    static_cast<void>(SetupEngine::uninstall(paths, uninstall_plan, true));
    const json purge_plan = SetupEngine::plan_purge(paths, {component_id});
    REQUIRE(purge_plan.at("components").empty());
    REQUIRE(purge_plan.at("blockers").size() == 1);
    REQUIRE_THROWS(SetupEngine::purge(paths, purge_plan, true));
    REQUIRE(fs::is_directory(paths.data_root));

    const std::string protected_id = "protected.fixture";
    const fs::path protected_active =
        paths.install_root / "components" / protected_id / "1.0.0";
    fs::create_directories(protected_active);
    std::ofstream(protected_active / "payload") << "payload";
    std::ofstream(receipts / (protected_id + ".json"))
        << json{
               {"schema", "arena_install_receipt_v1"},
               {"schema_version", 1},
               {"component_id", protected_id},
               {"component_version", "1.0.0"},
               {"state", "installed_managed"},
               {"active_root", protected_active.string()},
               {"artifact_sha256", std::string(64, 'a')},
               {"dependencies", json::array()},
               {"ownership",
                json::array({{{"path", (paths.data_root / "transactions").string()},
                              {"ownership_class", "user_preserved"},
                              {"normal_uninstall", "preserve"},
                              {"purge", "explicit_recursive"},
                              {"kind", "directory_tree"}}})},
               {"data_paths", json::array()},
               {"activated_files", json::array()},
           }
               .dump(2);
    const json protected_uninstall =
        SetupEngine::plan_uninstall(paths, {protected_id});
    static_cast<void>(SetupEngine::uninstall(paths, protected_uninstall, true));
    const json protected_purge = SetupEngine::plan_purge(paths, {protected_id});
    REQUIRE(protected_purge.at("components").empty());
    REQUIRE(protected_purge.at("blockers").size() == 1);
    REQUIRE(fs::is_directory(paths.data_root / "transactions" /
                             "uninstall-preservation"));
    fs::remove_all(root);
}

TEST_CASE("purge refuses a persistent tree containing a symbolic link") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const std::string component_id = "symlink.fixture";
    const fs::path active_root =
        paths.install_root / "components" / component_id / "1.0.0";
    const fs::path workspace = paths.workspace_root / "symlink-fixture";
    const fs::path external = root / "external-data";
    const fs::path receipts = paths.data_root / "receipts";
    fs::create_directories(active_root);
    fs::create_directories(workspace);
    fs::create_directories(external);
    fs::create_directories(receipts);
    std::ofstream(active_root / "payload") << "payload";
    std::ofstream(workspace / "state") << "valuable";
    std::ofstream(external / "sentinel") << "external";
    std::ofstream(receipts / (component_id + ".json"))
        << json{
               {"schema", "arena_install_receipt_v1"},
               {"schema_version", 1},
               {"component_id", component_id},
               {"component_version", "1.0.0"},
               {"state", "installed_managed"},
               {"active_root", active_root.string()},
               {"artifact_sha256", std::string(64, 'a')},
               {"dependencies", json::array()},
               {"data_paths", json::array({workspace.string()})},
               {"activated_files", json::array()},
           }
               .dump(2);
    const json uninstall_plan =
        SetupEngine::plan_uninstall(paths, {component_id});
    static_cast<void>(SetupEngine::uninstall(paths, uninstall_plan, true));
    const json purge_plan = SetupEngine::plan_purge(paths, {component_id});
    std::error_code link_error;
    fs::create_directory_symlink(external, workspace / "external-link", link_error);
    if (!link_error) {
        REQUIRE_THROWS(SetupEngine::purge(paths, purge_plan, true));
        REQUIRE(fs::is_regular_file(workspace / "state"));
        REQUIRE(fs::is_regular_file(external / "sentinel"));
    }
    fs::remove_all(root);
}

TEST_CASE("purge planning blocks recursive ownership overlapping external data") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const std::string component_id = "overlap.fixture";
    const fs::path active_root =
        paths.install_root / "components" / component_id / "1.0.0";
    const fs::path workspace = paths.workspace_root / "overlap-fixture";
    const fs::path external = workspace / "operator-owned.bin";
    const fs::path receipts = paths.data_root / "receipts";
    fs::create_directories(active_root);
    fs::create_directories(workspace);
    fs::create_directories(receipts);
    std::ofstream(active_root / "payload") << "payload";
    std::ofstream(external) << "external";
    std::ofstream(receipts / (component_id + ".json"))
        << json{
               {"schema", "arena_install_receipt_v1"},
               {"schema_version", 1},
               {"component_id", component_id},
               {"component_version", "1.0.0"},
               {"state", "installed_managed"},
               {"active_root", active_root.string()},
               {"artifact_sha256", std::string(64, 'a')},
               {"dependencies", json::array()},
               {"ownership",
                json::array({
                    {{"path", workspace.string()},
                     {"ownership_class", "user_preserved"},
                     {"normal_uninstall", "preserve"},
                     {"purge", "explicit_recursive"},
                     {"kind", "directory_tree"}},
                    {{"path", external.string()},
                     {"ownership_class", "external_dependency"},
                     {"normal_uninstall", "preserve"},
                     {"purge", "forbidden"},
                     {"kind", "file"}},
                })},
               {"data_paths", json::array()},
               {"activated_files", json::array()},
           }
               .dump(2);
    const json uninstall_plan =
        SetupEngine::plan_uninstall(paths, {component_id});
    static_cast<void>(SetupEngine::uninstall(paths, uninstall_plan, true));
    const json purge_plan = SetupEngine::plan_purge(paths, {component_id});
    REQUIRE(purge_plan.at("components").empty());
    REQUIRE(purge_plan.at("blockers").size() == 1);
    REQUIRE(fs::is_regular_file(external));
    fs::remove_all(root);
}
