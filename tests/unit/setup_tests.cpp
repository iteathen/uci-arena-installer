#include <algorithm>
#include <archive.h>
#include <archive_entry.h>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cctype>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <optional>
#include <sodium.h>
#include <string>
#include <string_view>
#include <tuple>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#endif

#include "arena/application/installer/credential_recovery.hpp"
#include "arena/application/installer/removal_lifecycle.hpp"
#include "arena/application/installer/setup.hpp"
#include "arena/domain/uuid.hpp"
#include "arena/platform/filesystem_path.hpp"
#include "arena/platform/workspace_permissions.hpp"
#include "arena/ports/asset_search_index.hpp"
#include "arena/ports/provider_lifecycle.hpp"
#include "arena/ports/native_process.hpp"

namespace {

namespace fs = std::filesystem;
using arena::application::installer::SetupEngine;
using arena::application::installer::SetupPaths;
using arena::application::installer::RemovalLifecycle;
using arena::application::installer::RemovalLifecycleHooks;
using arena::application::installer::assess_book_forge_credential_recovery;
using arena::application::installer::recover_book_forge_credential;
using nlohmann::json;

TEST_CASE("provider startup lifecycle has a bounded replacement-safe deadline") {
    STATIC_REQUIRE(arena::ports::provider_lifecycle_timeout_default ==
                   std::chrono::minutes(10));
}

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

void remove_locator_test_directory(const fs::path& root) {
    REQUIRE(root.filename().string().starts_with("arena-setup-"));
    REQUIRE_FALSE(fs::is_symlink(root));
    REQUIRE(fs::canonical(root).parent_path() == fs::canonical(fs::temp_directory_path()));
    REQUIRE(fs::canonical(root) == fs::absolute(root).lexically_normal());
    fs::remove_all(arena::platform::filesystem_api_path(root));
}

#ifdef _WIN32
std::jthread hold_transaction_journal_after_invocation(
    const SetupPaths& paths, const fs::path& diagnostic_barrier,
    bool& journal_locked, std::string& lock_failure) {
    return std::jthread([paths, diagnostic_barrier, &journal_locked, &lock_failure](std::stop_token stop) {
        HANDLE held = INVALID_HANDLE_VALUE;
        try {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!fs::exists(diagnostic_barrier.string() + ".entered") &&
                   std::chrono::steady_clock::now() < deadline && !stop.stop_requested()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (!fs::exists(diagnostic_barrier.string() + ".entered"))
                throw std::runtime_error("fixture invocation barrier missing");
            fs::path journal;
            for (const auto& entry : fs::directory_iterator(paths.data_root / "transactions")) {
                if (entry.path().extension() == ".json") {
                    if (!journal.empty()) throw std::runtime_error("fixture journal selection is ambiguous");
                    journal = entry.path();
                }
            }
            if (journal.empty()) throw std::runtime_error("fixture journal missing");
            held = CreateFileW(journal.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (held == INVALID_HANDLE_VALUE) throw std::runtime_error("fixture journal lock failed");
            journal_locked = true;
            std::ofstream(diagnostic_barrier.string() + ".release") << "release\n";
            while (!stop.stop_requested()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } catch (const std::exception& error) { lock_failure = error.what(); }
        if (held != INVALID_HANDLE_VALUE) CloseHandle(held);
    });
}
#endif

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

json typed_lifecycle_arguments(const std::string& command,
                               const std::string& component_id,
                               const std::string& variant) {
    return json::array({"lifecycle",
                        command,
                        "--typed-lifecycle",
                        "--component-id",
                        component_id,
                        "--component-version",
                        "1.0.0",
                        "--variant",
                        variant});
}

json typed_lifecycle_control(const std::string& component_id,
                             const std::string& install_variant = "ready",
                             const std::string& post_replace_variant = "ready",
                             const bool replacement = false) {
    json operations{
        {"install",
         {{"arguments",
           typed_lifecycle_arguments("enable", component_id, install_variant)},
          {"success_dispositions",
           install_variant == "safe_inactive"
               ? json::array({"ready", "safe_inactive"})
               : json::array({"ready"})}}},
        {"uninstall",
         {{"arguments",
           typed_lifecycle_arguments("disable", component_id, "ready")},
          {"success_dispositions", json::array({"ready"})}}},
    };
    json control{
        {"schema", "arena_provider_startup_control_v3"},
        {"schema_version", 3},
        {"entrypoint", "installer_integration"},
        {"operations", std::move(operations)},
        {"workspace_argument",
         {{"flag", "--state-root"}, {"relative_path", "runtime"}}},
        {"configuration_argument", "--settings"},
    };
    if (replacement) {
        control["operations"]["pre_replace"] = {
            {"arguments",
             typed_lifecycle_arguments("prepare_replace", component_id,
                                       "ready")},
            {"success_dispositions", json::array({"ready"})}};
        control["operations"]["post_replace"] = {
            {"arguments",
             typed_lifecycle_arguments("complete_replace", component_id,
                                       post_replace_variant)},
            {"success_dispositions",
             post_replace_variant == "safe_inactive"
                 ? json::array({"ready", "safe_inactive"})
                 : json::array({"ready"})}};
        control["replacement_arguments"] = {
            {"previous_root", "--previous-root"},
            {"replacement_root", "--replacement-root"}};
    }
    return control;
}

json ready_uninstall_lifecycle(const std::string& component_id,
                               const std::string& component_version = "1.0.0") {
    return {
        {"schema", "arena_provider_lifecycle_result_v1"},
        {"schema_version", 1},
        {"component_id", component_id},
        {"component_version", component_version},
        {"operation", "uninstall"},
        {"disposition", "ready"},
        {"observed",
         {{"installation", "absent"},
          {"activity", "inactive"},
          {"configuration", "not_applicable"},
          {"replacement", "not_applicable"}}},
    };
}

void write_component_archive(const fs::path& source_root,
                             const fs::path& archive_path) {
    archive* writer = archive_write_new();
    REQUIRE(writer != nullptr);
    REQUIRE(archive_write_add_filter_gzip(writer) == ARCHIVE_OK);
    REQUIRE(archive_write_set_format_pax_restricted(writer) == ARCHIVE_OK);
    REQUIRE(archive_write_open_filename(writer, archive_path.string().c_str()) ==
            ARCHIVE_OK);
    for (const auto& source : fs::recursive_directory_iterator(source_root)) {
        if (!source.is_regular_file()) continue;
        const fs::path relative = source.path().lexically_relative(source_root);
        archive_entry* entry = archive_entry_new();
        REQUIRE(entry != nullptr);
        archive_entry_set_pathname(entry, relative.generic_string().c_str());
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0644);
        archive_entry_set_size(
            entry, static_cast<la_int64_t>(fs::file_size(source.path())));
        REQUIRE(archive_write_header(writer, entry) == ARCHIVE_OK);
        std::ifstream input(source.path(), std::ios::binary);
        std::array<char, 4096> buffer{};
        while (input) {
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize count = input.gcount();
            if (count > 0) {
                REQUIRE(archive_write_data(writer, buffer.data(),
                                           static_cast<std::size_t>(count)) ==
                        static_cast<la_ssize_t>(count));
            }
        }
        archive_entry_free(entry);
    }
    REQUIRE(archive_write_close(writer) == ARCHIVE_OK);
    REQUIRE(archive_write_free(writer) == ARCHIVE_OK);
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
                                      const std::string& locator_kind,
                                      const fs::path& startup_workspace_relative = "runtime",
                                      const std::vector<std::string>& accepted_sources = {}) {
    write_mock_renderer_runtime(paths);
    const fs::path active_root =
        paths.install_root / "components" / component_id / "1.0.0";
    const fs::path renderer = active_root / "installer-integration.fixture";
    const fs::path workspace = paths.workspace_root / component_id;
    const fs::path configuration = workspace / "manager-config.json";
    fs::create_directories(active_root);
    fs::create_directories(workspace);
    std::ofstream(renderer, std::ios::binary) << "renderer";
    json locator_binding{{"name", "selected"},
                         {"kind", locator_kind},
                         {"required", false}};
    if (!accepted_sources.empty()) {
        locator_binding["accepted_sources"] = accepted_sources;
    }
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
        {"locator_bindings", json::array({locator_binding})},
        {"startup_control",
         {{"schema", "arena_provider_startup_control_v1"},
          {"schema_version", 1},
          {"entrypoint", "installer_integration"},
          {"actions",
           {{"install", json::array({"lifecycle", "enable"})},
            {"uninstall", json::array({"lifecycle", "disable"})}}},
           {"workspace_argument",
            {{"flag", "--state-root"},
             {"relative_path", startup_workspace_relative.generic_string()}}},
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
    const fs::path evidence_root =
        root / "evidence" /
        (std::string(64, 'a') + "-" + std::string(64, 'b'));
    fs::create_directories(arena::platform::filesystem_api_path(evidence_root));
    const fs::path evidence_relative =
        fs::path("evidence") /
        (std::string(64, 'a') + "-" + std::string(64, 'b'));
    const fs::path diagnostic_relative = evidence_relative / "time-profile.json";
    const fs::path complete_relative = evidence_relative / "complete-game.json";
    constexpr std::string_view diagnostic_contents = "diagnostic";
    constexpr std::string_view complete_contents = "complete-game";
    std::ofstream(arena::platform::filesystem_api_path(root / diagnostic_relative),
                  std::ios::binary)
        << diagnostic_contents;
    std::ofstream(arena::platform::filesystem_api_path(root / complete_relative),
                  std::ios::binary)
        << complete_contents;
    const json diagnostic{
        {"passed", true},
        {"artifact", diagnostic_relative.generic_string()},
        {"artifact_sha256", "sha256:" + sha256_text(diagnostic_contents)},
    };
    const json complete{
        {"passed", true},
        {"artifact", complete_relative.generic_string()},
        {"artifact_sha256", "sha256:" + sha256_text(complete_contents)},
    };
    const fs::path profile_path = root / filename;
    std::ofstream(arena::platform::filesystem_api_path(profile_path))
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

void write_manager_api_service_install(
    const SetupPaths& paths, const json& manager_api,
    const std::string_view component_version = "0.3.0",
    const std::string_view status_schema = "uci_arena_evidence_status_v1") {
    constexpr std::string_view component_id = "uci_arena_evidence.service";
    constexpr std::string_view cli_relative = "services/evidence-service/src/cli.mjs";
    constexpr std::string_view contract_relative =
        "services/evidence-service/contracts/service-contract.json";
    constexpr std::string_view cli_contents = "evidence";
    write_mock_renderer_runtime(paths);
    const fs::path active_root = paths.install_root / "components" /
                                 component_id / component_version;
    const fs::path cli = active_root / cli_relative;
    const fs::path contract_path = active_root / contract_relative;
    const fs::path workspace = paths.workspace_root / "evidence-service";
    const fs::path configuration = workspace / "manager-config.json";
    const fs::path generated = workspace / "generated" /
                               "context-projection.json";
    fs::create_directories(cli.parent_path());
    fs::create_directories(contract_path.parent_path());
    fs::create_directories(paths.data_root / "receipts");
    fs::create_directories(workspace);
    fs::create_directories(generated.parent_path());
    std::ofstream(cli, std::ios::binary) << cli_contents;
    std::ofstream(configuration, std::ios::binary)
        << json{
               {"schema", "replaceable_provider_configuration_v1"},
               {"enabled", true},
               {"bindings", json::object()},
               {"locators", json::object()},
               {"locator_details", json::object()},
           }
               .dump(2);
    std::ofstream(generated, std::ios::binary)
        << json{
               {"schema", "replaceable_provider_projection_v1"},
               {"locators", json::object()},
               {"locator_details", json::object()},
           }
               .dump(2);

    const json startup{
        {"windows", "per_user_task_scheduler"},
        {"linux", "per_user_systemd"},
    };
    const json capabilities =
        json::array({"status", "start", "startup_install", "startup_uninstall"});
    const json contract{
        {"schema", "uci_arena_standalone_service_contract_v1"},
        {"component_id", component_id},
        {"component_version", std::string(component_version)},
        {"cli_entrypoint", cli_relative},
        {"configuration_schema", "uci_arena_evidence_service_config_v1"},
        {"status_schema", std::string(status_schema)},
        {"capabilities", capabilities},
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
    const json integration{
        {"schema", "arena_provider_installer_integration_v1"},
        {"schema_version", 1},
        {"entrypoint", "cli"},
        {"invocation",
         {{"kind", "dependency_runtime"},
          {"runtime_component_id", "runtime.host"},
          {"runtime_entrypoint", "execute"}}},
        {"arguments", json::array({"--context"})},
        {"workspace_name", "evidence-service"},
        {"configure_when_disabled", true},
        {"dependency_bindings", json::array()},
        {"locator_bindings", json::array()},
        {"startup_control",
         {{"schema", "arena_provider_startup_control_v1"},
          {"schema_version", 1},
          {"entrypoint", "cli"},
          {"actions",
           {{"install", json::array({"lifecycle", "enable"})},
            {"uninstall", json::array({"lifecycle", "disable"})}}},
          {"workspace_argument",
           {{"flag", "--workspace"}, {"relative_path", "."}}},
          {"configuration_argument", "--config"},
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
               {"component_version", std::string(component_version)},
               {"entrypoints", {{"cli", cli_relative}}},
               {"contract_manifest", contract_relative},
               {"manager_integration_api", manager_api},
               {"capabilities", capabilities},
               {"data_paths", json::array({"workspace"})},
               {"startup", startup},
               {"dependencies", json::array({"runtime.host"})},
               {"workspace_name", "evidence-service"},
               {"installer_integration", integration},
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
               {"component_version", std::string(component_version)},
               {"state", "installed_managed"},
               {"active_root", active_root.string()},
               {"artifact_sha256", std::string(64, 'a')},
               {"dependencies", json::array({"runtime.host"})},
               {"data_paths", json::array({workspace.string()})},
               {"configuration_path", configuration.string()},
               {"activated_files", json::array()},
               {"startup",
                {{"installed", true},
                 {"matchesCurrentConfiguration", true},
                 {"active", true}}},
               {"service_contract",
                {{"schema", "uci_arena_standalone_service_contract_v1"},
                 {"path", contract_relative},
                 {"sha256", contract_sha256},
                 {"configuration_schema", "uci_arena_evidence_service_config_v1"},
                 {"status_schema", std::string(status_schema)},
                 {"manager_integration_api", manager_api}}},
           }
               .dump(2);
}

} // namespace

#ifdef _WIN32
namespace {
void add_fixture_broad_directory_write(const fs::path& directory) {
    PSECURITY_DESCRIPTOR descriptor{};
    PACL original{};
    std::wstring native = directory.native();
    REQUIRE(GetNamedSecurityInfoW(native.data(), SE_FILE_OBJECT,
                DACL_SECURITY_INFORMATION, nullptr, nullptr, &original, nullptr,
                &descriptor) == ERROR_SUCCESS);
    std::array<unsigned char, SECURITY_MAX_SID_SIZE> sid{};
    DWORD length = static_cast<DWORD>(sid.size());
    REQUIRE(CreateWellKnownSid(WinAuthenticatedUserSid, nullptr, sid.data(), &length));
    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_GENERIC_EXECUTE | DELETE;
    access.grfAccessMode = GRANT_ACCESS;
    access.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(sid.data());
    PACL updated{};
    REQUIRE(SetEntriesInAclW(1, &access, original, &updated) == ERROR_SUCCESS);
    const DWORD status = SetNamedSecurityInfoW(native.data(), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION, nullptr, nullptr, updated, nullptr);
    LocalFree(updated); LocalFree(descriptor);
    REQUIRE(status == ERROR_SUCCESS);
}

std::string fixture_dacl(const fs::path& directory) {
    PSECURITY_DESCRIPTOR descriptor{};
    std::wstring native = directory.native();
    REQUIRE(GetNamedSecurityInfoW(native.data(), SE_FILE_OBJECT,
                DACL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, nullptr,
                &descriptor) == ERROR_SUCCESS);
    LPSTR text{};
    REQUIRE(ConvertSecurityDescriptorToStringSecurityDescriptorA(descriptor,
        SDDL_REVISION_1, DACL_SECURITY_INFORMATION, &text, nullptr));
    const std::string result{text};
    LocalFree(text); LocalFree(descriptor);
    return result;
}

bool fixture_authenticated_users_can_write(const fs::path& directory) {
    PSECURITY_DESCRIPTOR descriptor{}; PACL dacl{};
    std::wstring native = directory.native();
    REQUIRE(GetNamedSecurityInfoW(native.data(), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr, &descriptor) == ERROR_SUCCESS);
    std::array<unsigned char, SECURITY_MAX_SID_SIZE> sid{};
    DWORD length = static_cast<DWORD>(sid.size());
    REQUIRE(CreateWellKnownSid(WinAuthenticatedUserSid, nullptr, sid.data(), &length));
    TRUSTEE_W trustee{}; trustee.TrusteeForm = TRUSTEE_IS_SID;
    trustee.ptstrName = reinterpret_cast<LPWSTR>(sid.data());
    ACCESS_MASK rights{};
    REQUIRE(GetEffectiveRightsFromAclW(dacl, &trustee, &rights) == ERROR_SUCCESS);
    LocalFree(descriptor);
    return (rights & (FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY | FILE_DELETE_CHILD |
        DELETE | WRITE_DAC | WRITE_OWNER | GENERIC_WRITE | GENERIC_ALL)) != 0;
}

json fixture_empty_install_plan() {
    return {{"schema", "arena_install_plan_v1"}, {"blockers", json::array()},
            {"actions", json::array()}};
}
} // namespace

TEST_CASE("per-user fresh data root is private before Installer state creation", "[data-root-access]") {
    const fs::path root = temporary_directory();
    add_fixture_broad_directory_write(root);
    REQUIRE(fixture_authenticated_users_can_write(root));
    SetupPaths paths = paths_under(root);
    paths.manager_service_access = false;
    REQUIRE(SetupEngine::apply(json::object(), fixture_empty_install_plan(), paths,
        true, false).at("state") == "committed");
    REQUIRE_FALSE(fixture_authenticated_users_can_write(paths.data_root));
    REQUIRE_FALSE(fixture_authenticated_users_can_write(paths.data_root / "transactions"));
    REQUIRE(fixture_dacl(paths.data_root).starts_with("D:P"));
    const auto dacl = fixture_dacl(paths.data_root);
    REQUIRE(SetupEngine::apply(json::object(), fixture_empty_install_plan(), paths,
        true, false).at("state") == "committed");
    REQUIRE(fixture_dacl(paths.data_root) == dacl);
    REQUIRE(fixture_authenticated_users_can_write(root));
    remove_locator_test_directory(root);
}

TEST_CASE("per-user unsafe existing data root refuses before effects and preserves child ACL", "[data-root-access]") {
    const fs::path root = temporary_directory();
    SetupPaths paths = paths_under(root);
    paths.manager_service_access = false;
    fs::create_directory(paths.data_root);
    add_fixture_broad_directory_write(paths.data_root);
    const fs::path retained = paths.data_root / "retained-artifact.txt";
    std::ofstream(retained) << "owned fixture artifact";
    const auto before = fixture_dacl(paths.data_root), child_before = fixture_dacl(retained);
    try {
        static_cast<void>(SetupEngine::apply(json::object(), fixture_empty_install_plan(), paths, true, false));
        FAIL("unsafe existing data root was accepted");
    } catch (const std::runtime_error& error) {
        REQUIRE(std::string(error.what()) == "per-user data root is not private; prepare its directory permissions explicitly or select a fresh root");
    }
    REQUIRE_FALSE(fs::exists(paths.install_root));
    REQUIRE_FALSE(fs::exists(paths.data_root / "transactions"));
    REQUIRE_FALSE(fs::exists(paths.data_root / "receipts"));
    REQUIRE(fixture_dacl(paths.data_root) == before);
    REQUIRE(fixture_dacl(retained) == child_before);
    REQUIRE(fs::file_size(retained) == 22);
    remove_locator_test_directory(root);
}

TEST_CASE("per-user data root requires an ordinary existing parent", "[data-root-access]") {
    const fs::path root = temporary_directory();
    SetupPaths paths = paths_under(root);
    paths.manager_service_access = false;
    paths.data_root = root / "missing-parent" / "data";
    REQUIRE_THROWS(SetupEngine::apply(json::object(), fixture_empty_install_plan(), paths, true, false));
    REQUIRE_FALSE(fs::exists(root / "missing-parent"));
    REQUIRE_FALSE(fs::exists(paths.install_root));
    remove_locator_test_directory(root);
}

TEST_CASE("standalone per-user release retention prepares its root before staging", "[data-root-access]") {
    const fs::path root = temporary_directory();
    add_fixture_broad_directory_write(root);
    SetupPaths paths = paths_under(root);
    paths.manager_service_access = false;
    const auto media = root / "media";
    fs::create_directory(media);
    const json catalog{{"schema", "arena_install_catalog_v1"}, {"schema_version", 1},
        {"suite_version", "1.0.0"}, {"components", json::array()}};
    const auto source = media / "catalog.json";
    std::ofstream(source) << catalog.dump();
    REQUIRE(SetupEngine::persist_release_media(catalog, paths, source, {}, true).at("schema") == "arena_retained_release_media_v1");
    REQUIRE_FALSE(fixture_authenticated_users_can_write(paths.data_root));
    REQUIRE_FALSE(fixture_authenticated_users_can_write(paths.data_root / "release"));
    add_fixture_broad_directory_write(paths.data_root);
    const auto before = fixture_dacl(paths.data_root);
    REQUIRE_THROWS(SetupEngine::persist_release_media(catalog, paths, source, {}, true));
    REQUIRE(fixture_dacl(paths.data_root) == before);
    REQUIRE(fs::is_regular_file(paths.data_root / "release" / "catalog.json"));
    remove_locator_test_directory(root);
}

TEST_CASE("per-user existing data root rejects null object callback and inherit-only broad authority", "[data-root-access]") {
    for (const std::string variant : {"null", "object", "callback", "inherit-only", "foreign-sid"}) {
        INFO(variant);
        const fs::path root = temporary_directory();
        SetupPaths paths = paths_under(root);
        paths.manager_service_access = false;
        arena::platform::prepare_per_user_data_root(paths.data_root);
        std::wstring native = paths.data_root.native();
        PSECURITY_DESCRIPTOR descriptor{}; PACL original{};
        REQUIRE(GetNamedSecurityInfoW(native.data(), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION, nullptr, nullptr, &original, nullptr, &descriptor) == ERROR_SUCCESS);
        std::array<unsigned char, SECURITY_MAX_SID_SIZE> sid{};
        DWORD length = static_cast<DWORD>(sid.size());
        REQUIRE(CreateWellKnownSid(WinAuthenticatedUserSid, nullptr, sid.data(), &length));
        std::vector<unsigned char> storage(original->AclSize + SECURITY_MAX_SID_SIZE + 128);
        auto* modified = reinterpret_cast<ACL*>(storage.data());
        REQUIRE(InitializeAcl(modified, static_cast<DWORD>(storage.size()), ACL_REVISION_DS));
        for (DWORD i = 0; i < original->AceCount; ++i) {
            void* ace{}; REQUIRE(GetAce(original, i, &ace));
            REQUIRE(AddAce(modified, ACL_REVISION_DS, MAXDWORD, ace, static_cast<ACE_HEADER*>(ace)->AceSize));
        }
        if (variant == "object") {
            GUID object_type{0x22b85c12, 0x6a1b, 0x48d8, {0xb4, 0x27, 0x41, 0x22, 0x67, 0x85, 0x02, 0xa1}};
            REQUIRE(AddAccessAllowedObjectAce(modified, ACL_REVISION_DS, 0,
                FILE_GENERIC_READ, &object_type, nullptr, sid.data()));
        } else if (variant == "callback") {
            std::vector<unsigned char> ace(sizeof(ACE_HEADER) + sizeof(ACCESS_MASK) + length);
            auto* header = reinterpret_cast<ACE_HEADER*>(ace.data());
            header->AceType = ACCESS_ALLOWED_CALLBACK_ACE_TYPE;
            header->AceSize = static_cast<WORD>(ace.size());
            const ACCESS_MASK access = FILE_GENERIC_READ;
            std::memcpy(ace.data() + sizeof(ACE_HEADER), &access, sizeof(access));
            std::memcpy(ace.data() + sizeof(ACE_HEADER) + sizeof(access), sid.data(), length);
            REQUIRE(AddAce(modified, ACL_REVISION_DS, MAXDWORD, ace.data(), static_cast<DWORD>(ace.size())));
        } else if (variant == "inherit-only") {
            REQUIRE(AddAccessAllowedAceEx(modified, ACL_REVISION_DS,
                INHERIT_ONLY_ACE | OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE,
                FILE_GENERIC_WRITE, sid.data()));
        } else if (variant == "foreign-sid") {
            PSID foreign{};
            REQUIRE(ConvertStringSidToSidW(L"S-1-5-21-111-222-333-444", &foreign));
            const BOOL added = AddAccessAllowedAceEx(modified, ACL_REVISION_DS, 0,
                FILE_GENERIC_WRITE, foreign);
            LocalFree(foreign);
            REQUIRE(added);
        }
        const DWORD status = SetNamedSecurityInfoW(native.data(), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            nullptr, nullptr, variant == "null" ? nullptr : modified, nullptr);
        LocalFree(descriptor);
        REQUIRE(status == ERROR_SUCCESS);
        const auto before = fixture_dacl(paths.data_root);
        REQUIRE_THROWS(arena::platform::prepare_per_user_data_root(paths.data_root));
        REQUIRE(fixture_dacl(paths.data_root) == before);
        REQUIRE_FALSE(fs::exists(paths.data_root / "transactions"));
        remove_locator_test_directory(root);
    }
}

TEST_CASE("valid TUF plan prepares a private per-user root before confirmed apply", "[data-root-access]") {
    const fs::path root = temporary_directory();
    add_fixture_broad_directory_write(root);
    const fs::path metadata = fs::path(ARENA_INSTALLER_SOURCE_DIR) / "tests/fixtures/tuf-plan-root";
    const auto execute = [&](const SetupPaths& paths, const std::string& command,
                             const fs::path& media, const bool confirmed, const unsigned exit_code,
                             const bool unsigned_catalog = false) {
        std::vector<std::string> arguments{
            "--install-scope", "per-user", "--install-root", paths.install_root.string(),
            "--data-root", paths.data_root.string(), "--cache-root", paths.cache_root.string(),
            "--workspace-root", paths.workspace_root.string(), "--knowledge-root", paths.knowledge_root.string(),
            "--catalog", (media / "catalog.json").string()};
        if (unsigned_catalog) arguments.push_back("--allow-unsigned-development-catalog");
        else { arguments.push_back("--metadata-dir"); arguments.push_back(media.string()); }
        arguments.insert(arguments.end(), {command, "--preset", "manager_only"});
        if (confirmed) arguments.push_back("--yes");
        std::cout << "fixture-cli-argv " << json(arguments).dump() << '\n';
        arena::ports::NativeProcess process;
        process.start(ARENA_SETUP_ROOT_FIXTURE_PATH, root, arguments);
        std::string output;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (process.running() && std::chrono::steady_clock::now() < deadline) {
            if (const auto line = process.read_line(std::chrono::milliseconds(100))) output += *line + '\n';
        }
        while (const auto line = process.read_line(std::chrono::milliseconds(10))) output += *line + '\n';
        process.stop(std::chrono::milliseconds(0));
        INFO(output);
        REQUIRE(process.exit_code() == exit_code);
        return output;
    };
    SetupPaths paths = paths_under(root);
    paths.manager_service_access = false;
    REQUIRE_FALSE(fs::exists(paths.data_root));
    static_cast<void>(execute(paths, "plan", metadata, false, 0));
    REQUIRE(fs::is_directory(paths.data_root));
    REQUIRE_FALSE(fixture_authenticated_users_can_write(paths.data_root));
    REQUIRE(fs::is_regular_file(paths.data_root / "trust/tuf-versions.json"));
    REQUIRE_FALSE(fs::exists(paths.data_root / "transactions"));
    REQUIRE_FALSE(fs::exists(paths.data_root / "receipts"));
    REQUIRE_FALSE(fs::exists(paths.install_root));
    static_cast<void>(execute(paths, "apply", metadata, true, 0));
    REQUIRE(fs::is_directory(paths.data_root / "transactions"));
    REQUIRE_FALSE(fixture_authenticated_users_can_write(paths.data_root / "transactions"));

    SetupPaths unconfirmed = paths_under(root / "unconfirmed");
    unconfirmed.manager_service_access = false;
    static_cast<void>(execute(unconfirmed, "apply", metadata, false, 2));
    REQUIRE(fs::is_regular_file(unconfirmed.data_root / "trust/tuf-versions.json"));
    REQUIRE_FALSE(fixture_authenticated_users_can_write(unconfirmed.data_root));
    REQUIRE_FALSE(fs::exists(unconfirmed.data_root / "transactions"));

    SetupPaths nonpublishing = paths_under(root / "nonpublishing");
    nonpublishing.manager_service_access = false;
    static_cast<void>(execute(nonpublishing, "apply", root / "missing-metadata", true, 2));
    REQUIRE_FALSE(fs::exists(nonpublishing.data_root));
    REQUIRE_FALSE(fs::exists(nonpublishing.install_root));

    SetupPaths diagnostic = paths_under(root / "unsigned-diagnostic");
    diagnostic.manager_service_access = false;
    static_cast<void>(execute(diagnostic, "plan", metadata, false, 0, true));
    REQUIRE_FALSE(fs::exists(diagnostic.data_root));
    REQUIRE_FALSE(fs::exists(diagnostic.install_root));

    SetupPaths rollback = paths_under(root / "rollback");
    rollback.manager_service_access = false;
    static_cast<void>(execute(rollback, "plan", metadata, false, 0));
    const auto state = rollback.data_root / "trust/tuf-versions.json";
    const json higher{{"schema", "arena_tuf_rollback_state_v1"}, {"root", 1},
        {"timestamp", 2}, {"snapshot", 1}, {"targets", 1}};
    const std::string prior = higher.dump();
    // The native publisher's activated stage retains hidden attributes.
    // OPEN_EXISTING mutates this owned fixture without CREATE_ALWAYS refusing it.
    HANDLE state_handle = CreateFileW(state.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    REQUIRE(state_handle != INVALID_HANDLE_VALUE);
    DWORD written{};
    const BOOL write_ok = WriteFile(state_handle, prior.data(), static_cast<DWORD>(prior.size()), &written, nullptr);
    const BOOL truncate_ok = SetEndOfFile(state_handle);
    CloseHandle(state_handle);
    REQUIRE(write_ok);
    REQUIRE(written == prior.size());
    REQUIRE(truncate_ok);
    const auto rollback_failure = execute(rollback, "plan", metadata, false, 2);
    REQUIRE(rollback_failure.find("TUF rollback detected for timestamp") != std::string::npos);
    const std::string preserved{std::istreambuf_iterator<char>(std::ifstream(state).rdbuf()), {}};
    REQUIRE(preserved == prior);
    REQUIRE_FALSE(fs::exists(rollback.data_root / "transactions"));

    SetupPaths unsafe = paths_under(root / "unsafe");
    unsafe.manager_service_access = false;
    fs::create_directory(unsafe.data_root);
    add_fixture_broad_directory_write(unsafe.data_root);
    const auto before = fixture_dacl(unsafe.data_root);
    const auto unsafe_failure = execute(unsafe, "plan", metadata, false, 2);
    REQUIRE(unsafe_failure.find("per-user data root is not private") != std::string::npos);
    REQUIRE(fixture_dacl(unsafe.data_root) == before);
    REQUIRE_FALSE(fs::exists(unsafe.data_root / "trust"));
    REQUIRE_FALSE(fs::exists(unsafe.data_root / "transactions"));
    REQUIRE_FALSE(fs::exists(unsafe.data_root / "receipts"));
    REQUIRE_FALSE(fs::exists(unsafe.install_root));
    remove_locator_test_directory(root);
}
#endif

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
                                    const std::string_view contract_version = "1.0",
                                    const bool startup_active = false) {
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
    if (startup_active) {
        write_mock_renderer_runtime(paths);
    }
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
            {"credential", "forbidden"},
            {"response", "replaceable_provider_status_v1"}}}}},
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
        {"invocation",
         startup_active
             ? json{{"kind", "dependency_runtime"},
                    {"runtime_component_id", "runtime.host"},
                    {"runtime_entrypoint", "execute"}}
             : json{{"kind", "direct"}}},
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
    json installer_integration{
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
    if (startup_active) {
        installer_integration["startup_control"] = {
            {"schema", "arena_provider_startup_control_v1"},
            {"schema_version", 1},
            {"entrypoint", "cli"},
            {"actions",
             {{"install", json::array({"startup", "install"})},
              {"uninstall", json::array({"startup", "uninstall"})}}},
            {"workspace_argument",
             {{"flag", "--runtime-directory"}, {"relative_path", "."}}},
            {"configuration_argument", "--config"},
            {"result",
             {{"envelope", "root"},
              {"require_configuration_match_on_install", true},
              {"require_live_on_install", true}}},
        };
    }
    std::ofstream(active_root / "arena-component.json", std::ios::binary)
        << json{
               {"schema", "arena_install_component_v1"},
               {"schema_version", 1},
               {"component_id", component_id},
               {"component_version", component_version},
               {"entrypoints",
                {{"cli", cli_relative}, {"installer_integration", renderer_relative}}},
               {"contract_manifest", contract_relative},
               {"capabilities",
                startup_active
                    ? json::array(
                          {"provider_status", "startup_install", "startup_uninstall"})
                    : json::array({"provider_status"})},
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
    json receipt{
               {"schema", "arena_install_receipt_v1"},
               {"schema_version", 1},
               {"component_id", component_id},
               {"component_version", component_version},
               {"state", "installed_disabled"},
               {"active_root", active_root.string()},
               {"artifact_sha256", std::string(64, 'a')},
               {"dependencies", json::array({"runtime.host"})},
               {"data_paths",
                startup_active
                    ? json::array({(paths.workspace_root / "replaceable-provider").string()})
                    : json::array()},
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
           };
    if (startup_active) {
        fs::create_directories(paths.workspace_root / "replaceable-provider");
        receipt["state"] = "installed_managed";
        receipt["startup"] = {
            {"installed", true},
            {"matchesCurrentConfiguration", true},
            {"active", true},
        };
    }
    std::ofstream(paths.data_root / "receipts" / "replaceable.provider.json",
                  std::ios::binary)
        << receipt.dump(2);
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

TEST_CASE("repair restages current components while upgrade refreshes exact artifacts") {
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
               {"artifact_sha256", std::string(64, '0')},
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

    const json upgrade = SetupEngine::plan(
        catalog, paths, "manager_only", {}, true, false, false, false, false,
        true);
    REQUIRE(upgrade.at("repair") == false);
    REQUIRE(upgrade.at("upgrade") == true);
    REQUIRE(upgrade.at("actions").at(0).at("action") == "refresh");
    REQUIRE(upgrade.at("actions").at(0).contains("artifact"));
    REQUIRE(upgrade.at("space").at("staging_bytes") == 0);
    fs::remove_all(root);
}

TEST_CASE("exact-current upgrade refreshes receipts without quiescing providers") {
    const fs::path root = temporary_directory();
    SetupPaths paths = paths_under(root);
    paths.bundle_owner = "new-bundle-owner";
    const std::string component_id = "replaceable.current.consumer";
    const fs::path configuration = write_mock_renderer_consumer(
        paths, component_id, "lichess_transport_profile");
    const fs::path receipt_path =
        paths.data_root / "receipts" / (component_id + ".json");
    json consumer_receipt = json::parse(std::ifstream(receipt_path));
    consumer_receipt["state"] = "installed_managed";
    consumer_receipt["startup"] = {{"installed", true}, {"active", true}};
    std::ofstream(receipt_path, std::ios::binary | std::ios::trunc)
        << consumer_receipt.dump(2);

    const json exact_artifact{{"sha256", std::string(64, 'a')}};
    const json plan{
        {"schema", "arena_install_plan_v1"},
        {"blockers", json::array()},
        {"offline", true},
        {"catalog_directory", root.string()},
        {"actions",
         json::array(
             {{{"action", "refresh"},
               {"component_id", "runtime.host"},
               {"version", "1.0.0"},
               {"dependencies", json::array()},
               {"enabled", true},
               {"artifact", exact_artifact}},
              {{"action", "refresh"},
               {"component_id", component_id},
               {"version", "1.0.0"},
               {"dependencies", json::array({"runtime.host"})},
               {"enabled", true},
               {"artifact", exact_artifact}}})},
    };

    const json applied =
        SetupEngine::apply(json::object(), plan, paths, true, true);
    INFO(applied.dump(2));
    REQUIRE(applied.at("state") == "committed");
    const json refreshed_receipt = json::parse(std::ifstream(receipt_path));
    REQUIRE(refreshed_receipt.at("bundle_owner") == "new-bundle-owner");
    REQUIRE(refreshed_receipt.at("startup").at("installed") == true);
    REQUIRE(refreshed_receipt.at("startup").at("active") == true);
    REQUIRE(fs::is_directory(
        paths.install_root / "components" / component_id / "1.0.0"));
    REQUIRE(json::parse(std::ifstream(configuration)).at("enabled") == true);

    const json transaction = json::parse(std::ifstream(
        paths.data_root / "transactions" /
        (applied.at("transaction_id").get<std::string>() + ".json")));
    REQUIRE(transaction.at("startup_quiescence").empty());
    REQUIRE(transaction.at("refreshed_current_components") ==
            json::array({component_id, "runtime.host"}));
    REQUIRE(transaction.at("refreshed_current_consumers") ==
            json::array({component_id}));
    REQUIRE(transaction.at("actions").at(0).at("state") ==
            "refreshed_current");
    REQUIRE(transaction.at("actions").at(1).at("state") ==
            "refreshed_current");
    fs::remove_all(root);
}

TEST_CASE("post-commit cleanup removes only verified superseded component roots") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    constexpr std::string_view component_id = "replaceable.component";
    const fs::path component_root =
        paths.install_root / "components" / component_id;
    const auto write_component = [&](const std::string_view version,
                                     const bool modified) {
        const fs::path active_root = component_root / version;
        fs::create_directories(active_root);
        constexpr std::string_view payload = "payload";
        std::ofstream(active_root / "payload", std::ios::binary) << payload;
        std::ofstream(active_root / "arena-component.json", std::ios::binary)
            << json{
                   {"schema", "arena_install_component_v1"},
                   {"schema_version", 1},
                   {"component_id", component_id},
                   {"component_version", version},
                   {"files",
                    json::array(
                        {{{"path", "payload"},
                          {"size_bytes", payload.size()},
                          {"sha256", sha256_text(payload)}}})},
               }
                   .dump(2);
        if (modified) {
            std::ofstream(active_root / "operator-note.txt", std::ios::binary)
                << "preserve me";
        }
        return active_root;
    };

    const fs::path current = write_component("2.0.0", false);
    const fs::path removable = write_component("1.0.0", false);
    const fs::path modified = write_component("0.9.0", true);
    fs::create_directories(paths.data_root / "receipts");
    std::ofstream(paths.data_root / "receipts" /
                  (std::string(component_id) + ".json"),
                  std::ios::binary)
        << json{
               {"schema", "arena_install_receipt_v1"},
               {"schema_version", 1},
               {"component_id", component_id},
               {"component_version", "2.0.0"},
               {"state", "installed_managed"},
               {"active_root", current.string()},
               {"dependencies", json::array()},
               {"data_paths", json::array()},
               {"configuration_path", ""},
               {"activated_files", json::array()},
           }
               .dump(2);
    const json plan{
        {"schema", "arena_install_plan_v1"},
        {"blockers", json::array()},
        {"actions",
         json::array({{{"action", "keep"},
                       {"component_id", component_id},
                       {"version", "2.0.0"}}})},
    };

    const json applied =
        SetupEngine::apply(json::object(), plan, paths, true, false);
    const json& cleanup = applied.at("superseded_component_cleanup");
    INFO(cleanup.dump(2));
    REQUIRE(cleanup.at("schema") ==
            "arena_superseded_component_cleanup_v1");
    REQUIRE_FALSE(fs::exists(removable));
    REQUIRE(fs::is_regular_file(modified / "operator-note.txt"));
    REQUIRE(fs::is_regular_file(current / "payload"));
    REQUIRE(std::ranges::any_of(cleanup.at("roots"), [&](const json& outcome) {
        return outcome.value("path", "") == removable.string() &&
               outcome.value("state", "") == "removed";
    }));
    REQUIRE(std::ranges::any_of(cleanup.at("roots"), [&](const json& outcome) {
        return outcome.value("path", "") == modified.string() &&
               outcome.value("state", "") ==
                   "preserved_modified_or_unrecognized";
    }));

    const json repeated =
        SetupEngine::apply(json::object(), plan, paths, true, false);
    REQUIRE(repeated.at("superseded_component_cleanup").at("roots").size() ==
            1);
    REQUIRE(repeated.at("superseded_component_cleanup")
                .at("roots")
                .at(0)
                .value("state", "") ==
            "preserved_modified_or_unrecognized");
    fs::remove_all(root);
}

TEST_CASE("clean plan classifies an unreceipted component root before mutation") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const fs::path partial_root =
        paths.install_root / "components" / "manager.core" / "1.0.0";
    fs::create_directories(partial_root);
    std::ofstream(partial_root / "partial.payload", std::ios::binary) << "interrupted";
    const json catalog{
        {"schema", "arena_install_catalog_v1"},
        {"schema_version", 1},
        {"suite_version", "1.0.0"},
        {"presets", {{"manager_only", json::array({"manager.core"})}}},
        {"optional_component_groups",
         {{"knowledge", json::array()}, {"books", json::array()}, {"tablebases", json::array()}}},
        {"components", json::array({component("manager.core")})},
    };

    const json clean =
        SetupEngine::plan(catalog, paths, "manager_only", {}, true, false);
    REQUIRE(clean.at("repair") == false);
    REQUIRE(clean.at("actions").at(0).at("action") == "repair");
    REQUIRE(clean.at("blockers") ==
            json::array({{{"code", "partial_installation_requires_repair"},
                          {"component_id", "manager.core"},
                          {"version", "1.0.0"},
                          {"path", partial_root.string()}}}));
    REQUIRE_FALSE(fs::exists(paths.install_root / ".staging"));
    REQUIRE_FALSE(fs::exists(paths.data_root / "transactions"));

    const json repair =
        SetupEngine::plan(catalog, paths, "manager_only", {}, true, false, false, false, true);
    REQUIRE(repair.at("repair") == true);
    REQUIRE(repair.at("blockers").empty());
    REQUIRE(repair.at("actions").at(0).at("action") == "repair");
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

    const fs::path active_root = temporary_directory();
    const SetupPaths active_paths = paths_under(active_root);
    write_provider_service_install(active_paths, "1.0", true);
    const json active_verified = SetupEngine::verify(active_paths);
    INFO(active_verified.dump(2));
    REQUIRE(active_verified.at("ok") == true);
    fs::remove_all(active_root);

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
    REQUIRE(std::ranges::any_of(
        rejected.at("components"), [](const json& component) {
            return component.value("component_id", "") ==
                       "uci_arena_evidence.service" &&
                   component.value("error", "") ==
                       "standalone service Manager API operations are incomplete";
        }));
    fs::remove_all(incomplete_root);
}

TEST_CASE("verify rejects live provider status identity and schema drift") {
    const json api = manager_service_api(
        {"orchestration_status", "target_register", "campaign_enqueue", "job_control"});

    const fs::path version_root = temporary_directory();
    const SetupPaths version_paths = paths_under(version_root);
    write_manager_api_service_install(version_paths, api, "0.3.1");
    const json version_result = SetupEngine::verify(version_paths);
    REQUIRE(version_result.at("ok") == false);
    REQUIRE(std::ranges::any_of(
        version_result.at("components"), [](const json& component) {
            return component.value("component_id", "") ==
                       "uci_arena_evidence.service" &&
                   component.value("state", "") == "broken" &&
                   component.value("error", "") ==
                       "provider Manager API status failed contract validation: "
                       "uci_arena_evidence.service";
        }));
    fs::remove_all(version_root);

    const fs::path schema_root = temporary_directory();
    const SetupPaths schema_paths = paths_under(schema_root);
    write_manager_api_service_install(
        schema_paths, api, "0.3.0", "uci_arena_evidence_status_v2");
    const json schema_result = SetupEngine::verify(schema_paths);
    REQUIRE(schema_result.at("ok") == false);
    REQUIRE(std::ranges::any_of(
        schema_result.at("components"), [](const json& component) {
            return component.value("component_id", "") ==
                       "uci_arena_evidence.service" &&
                   component.value("state", "") == "broken";
        }));
    fs::remove_all(schema_root);
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
    const fs::path retired_syzygy = root / "retired-syzygy";
    SetupPaths paths = paths_under(root);
    const fs::path locator_path = paths.data_root / "locators" / "syzygy.json";
    fs::create_directories(locator_path.parent_path());
    std::ofstream(locator_path)
        << json{
               {"schema", "arena_saved_locator_v1"},
               {"schema_version", 1},
               {"kind", "syzygy"},
               {"mode", "in_place_reference"},
               {"path", retired_syzygy.string()},
               {"validation", json::object()},
           }
               .dump(2);
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
    REQUIRE(inventory.at("discovery").at("syzygy").at("selection").at("source") ==
            "install_receipt");
    REQUIRE(fs::is_regular_file(paths.knowledge_root / "tablebases" / "KQvK.rtbw"));
    REQUIRE(fs::file_size(paths.knowledge_root / "tablebases" / "KQvK.rtbw") == 11);
    REQUIRE(fs::is_regular_file(locator_path));
    std::ifstream locator_stream(locator_path, std::ios::binary);
    const json locator = json::parse(locator_stream);
    REQUIRE(locator.at("kind") == "syzygy");
    REQUIRE(fs::path(locator.at("path").get<std::string>()) == retired_syzygy);
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
        external / "uci_arena_lichess_capacity_profile_v2.json";
    const fs::path legacy_capacity =
        external / "uci_arena_lichess_capacity_profile_v1.json";
    const fs::path transport = external / "transport_profile_v2.json";
    std::ofstream(book, std::ios::binary) << "book";
    std::ofstream(binary_decoy, std::ios::binary) << "not a book";
    std::ofstream(table, std::ios::binary) << "tablebase";
    std::ofstream(timing) << json{{"schema", "uci_time_profile_v2"},
                                  {"qualified", true}}
                                 .dump();
    std::ofstream(capacity)
        << json{{"schema", "uci_arena_lichess_capacity_profile_v2"},
                {"qualified", true},
                {"maximum_concurrency", 2},
                {"time_profile_identity_sha256", "sha256:" + std::string(64, 'a')},
                {"evidence", {{"single_game", true}}}}
               .dump();
    std::ofstream(legacy_capacity)
        << json{{"schema", "uci_arena_lichess_capacity_profile_v1"},
                {"qualified", true},
                {"maximum_concurrency", 4},
                {"evidence", {{"single_game", true}, {"concurrent", true}}}}
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
        {arena::ports::IndexedAssetKind::lichess_capacity_profile, legacy_capacity},
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
                            "lichess_transport_profile"}) {
        const json& candidate = discovery.at(kind).at("candidates").at(0);
        REQUIRE(candidate.at("source") == "indexed");
        REQUIRE(candidate.at("details").at("validation").at("classification") ==
                "qualified_declared");
        REQUIRE(discovery.at(kind).at("indexed_search").at("accepted_candidates") == 1);
    }
    const json& capacity_selection =
        discovery.at("lichess_capacity_profile").at("selection");
    REQUIRE(capacity_selection.at("path") ==
            fs::absolute(capacity).lexically_normal().string());
    REQUIRE(capacity_selection.at("details").at("validation").at("classification") ==
            "qualified_declared");
    const json& capacity_candidates =
        discovery.at("lichess_capacity_profile").at("candidates");
    REQUIRE(capacity_candidates.size() == 2);
    REQUIRE(std::any_of(capacity_candidates.begin(), capacity_candidates.end(),
                        [](const json& candidate) {
                            return candidate.at("details")
                                       .at("validation")
                                       .at("classification") == "legacy_parseable";
                        }));
    REQUIRE(discovery.at("lichess_capacity_profile")
                .at("indexed_search")
                .at("accepted_candidates") == 2);
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

TEST_CASE("dependency configuration binding renders fresh public configuration rather than stale stored bytes") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const fs::path consumer = write_mock_renderer_consumer(paths, "a.configuration.consumer", "timing_profile");
    const fs::path producer = write_mock_renderer_consumer(paths, "z.configuration.producer", "uci_engine");
    const fs::path producer_receipt_path = paths.data_root / "receipts" / "z.configuration.producer.json";
    json producer_receipt = json::parse(std::ifstream(producer_receipt_path));
    producer_receipt["state"] = "installed_managed";
    std::ofstream(producer_receipt_path) << producer_receipt.dump(2);
    const fs::path manifest_path = paths.install_root / "components" /
                                  "a.configuration.consumer" / "1.0.0" / "arena-component.json";
    json manifest = json::parse(std::ifstream(manifest_path));
    manifest["dependencies"].push_back("z.configuration.producer");
    manifest["installer_integration"]["dependency_bindings"].push_back({
        {"name", "producerConfiguration"}, {"component_id", "z.configuration.producer"},
        {"source", "configuration"}, {"required", true}});
    std::ofstream(manifest_path) << manifest.dump(2);
    std::ofstream(producer) << R"({"schema":"stale-stored-configuration","unselected":true})";
    const json imported = SetupEngine::import_path(paths, "uci_engine", ARENA_MOCK_UCI_PATH);
    REQUIRE(imported.at("refreshed_consumers") ==
            json::array({"a.configuration.consumer", "z.configuration.producer"}));
    const json generated = json::parse(std::ifstream(consumer));
    const json selected = generated.at("bindings").at("producerConfiguration");
    REQUIRE(selected.at("schema") == "replaceable_provider_configuration_v1");
    REQUIRE(selected.at("enabled") == true);
    REQUIRE(selected.at("locators").at("selected") ==
            fs::absolute(ARENA_MOCK_UCI_PATH).lexically_normal().string());
    REQUIRE_FALSE(selected.contains("unselected"));
    REQUIRE(selected == json::parse(std::ifstream(producer)));
    remove_locator_test_directory(root);
}

TEST_CASE("dependency configuration rejects cycles without replacing saved authority or consumer bytes") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const fs::path configuration = write_mock_renderer_consumer(paths, "cyclic.configuration", "uci_engine");
    const fs::path receipt_path = paths.data_root / "receipts" / "cyclic.configuration.json";
    json receipt = json::parse(std::ifstream(receipt_path));receipt["state"] = "installed_managed";
    std::ofstream(receipt_path) << receipt.dump(2);
    const fs::path manifest_path = paths.install_root / "components" /
                                  "cyclic.configuration" / "1.0.0" / "arena-component.json";
    json manifest = json::parse(std::ifstream(manifest_path));
    manifest["dependencies"].push_back("cyclic.configuration");
    manifest["installer_integration"]["dependency_bindings"].push_back({
        {"name", "cycle"}, {"component_id", "cyclic.configuration"},
        {"source", "configuration"}, {"required", true}});
    std::ofstream(manifest_path) << manifest.dump(2);
    try {
        static_cast<void>(SetupEngine::import_path(paths, "uci_engine", ARENA_MOCK_UCI_PATH));
        FAIL("cyclic configuration dependency was admitted");
    } catch (const std::runtime_error& failure) {
        REQUIRE(std::string(failure.what()).find("configuration dependency cycle") != std::string::npos);
    }
    REQUIRE(json::parse(std::ifstream(configuration)).at("schema") == "old");
    REQUIRE_FALSE(fs::exists(paths.data_root / "locators" / "uci_engine.json"));
    remove_locator_test_directory(root);
}

TEST_CASE("dependency configuration omits an absent optional producer but rejects a present corrupted producer") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const fs::path configuration = write_mock_renderer_consumer(paths, "optional.configuration.consumer", "uci_engine");
    const fs::path manifest_path = paths.install_root / "components" /
                                  "optional.configuration.consumer" / "1.0.0" / "arena-component.json";
    json manifest = json::parse(std::ifstream(manifest_path));
    manifest["installer_integration"]["dependency_bindings"].push_back({
        {"name", "optionalConfiguration"}, {"component_id", "optional.configuration.producer"},
        {"source", "configuration"}, {"required", false}});
    std::ofstream(manifest_path) << manifest.dump(2);
    static_cast<void>(SetupEngine::import_path(paths, "uci_engine", ARENA_MOCK_UCI_PATH));
    const json baseline = json::parse(std::ifstream(configuration));
    const fs::path locator = paths.data_root / "locators" / "uci_engine.json";
    const json saved_locator = json::parse(std::ifstream(locator));
    REQUIRE_FALSE(baseline.at("bindings").contains("optionalConfiguration"));
    const fs::path producer = write_mock_renderer_consumer(paths, "optional.configuration.producer", "timing_profile");
    const fs::path receipt_path = paths.data_root / "receipts" / "optional.configuration.producer.json";
    json receipt = json::parse(std::ifstream(receipt_path));receipt["state"] = "installed_managed";
    std::ofstream(receipt_path) << receipt.dump(2);
    const fs::path renderer = paths.install_root / "components" /
                              "optional.configuration.producer" / "1.0.0" / "installer-integration.fixture";
    std::ofstream(renderer) << "corrupted inventoried bytes";
    REQUIRE_THROWS(SetupEngine::import_path(paths, "uci_engine", ARENA_MOCK_UCI_PATH));
    REQUIRE(json::parse(std::ifstream(configuration)) == baseline);
    REQUIRE(json::parse(std::ifstream(producer)).at("schema") == "old");
    REQUIRE(json::parse(std::ifstream(locator)) == saved_locator);
    remove_locator_test_directory(root);
}

TEST_CASE("dependency configuration rejects a corrupted transitive-only locator producer before committing authority") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const fs::path consumer = write_mock_renderer_consumer(paths, "a.transitive.consumer", "timing_profile");
    const fs::path producer = write_mock_renderer_consumer(paths, "z.transitive.producer", "uci_engine");
    const fs::path receipt_path = paths.data_root / "receipts" / "z.transitive.producer.json";
    json receipt = json::parse(std::ifstream(receipt_path));receipt["state"] = "installed_managed";
    std::ofstream(receipt_path) << receipt.dump(2);
    const fs::path consumer_manifest_path = paths.install_root / "components" /
                                          "a.transitive.consumer" / "1.0.0" / "arena-component.json";
    json manifest = json::parse(std::ifstream(consumer_manifest_path));
    manifest["dependencies"].push_back("z.transitive.producer");
    manifest["installer_integration"]["dependency_bindings"].push_back({
        {"name", "producerConfiguration"}, {"component_id", "z.transitive.producer"},
        {"source", "configuration"}, {"required", true}});
    std::ofstream(consumer_manifest_path) << manifest.dump(2);
    const fs::path renderer = paths.install_root / "components" /
                              "z.transitive.producer" / "1.0.0" / "installer-integration.fixture";
    std::ofstream(renderer) << "corrupted transitive-only producer";
    REQUIRE_THROWS(SetupEngine::import_path(paths, "uci_engine", ARENA_MOCK_UCI_PATH));
    REQUIRE(json::parse(std::ifstream(consumer)).at("schema") == "old");
    REQUIRE(json::parse(std::ifstream(producer)).at("schema") == "old");
    REQUIRE_FALSE(fs::exists(paths.data_root / "locators" / "uci_engine.json"));
    remove_locator_test_directory(root);
}

TEST_CASE("dependency configuration rejects a linked version root before producer execution") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const fs::path consumer = write_mock_renderer_consumer(paths, "linked.configuration.consumer", "uci_engine");
    static_cast<void>(write_mock_renderer_consumer(paths, "linked.configuration.producer", "timing_profile"));
    const fs::path receipt_path = paths.data_root / "receipts" / "linked.configuration.producer.json";
    json receipt = json::parse(std::ifstream(receipt_path));receipt["state"] = "installed_managed";
    std::ofstream(receipt_path) << receipt.dump(2);
    const fs::path consumer_manifest_path = paths.install_root / "components" /
                                          "linked.configuration.consumer" / "1.0.0" / "arena-component.json";
    json manifest = json::parse(std::ifstream(consumer_manifest_path));
    manifest["dependencies"].push_back("linked.configuration.producer");
    manifest["installer_integration"]["dependency_bindings"].push_back({
        {"name", "producerConfiguration"}, {"component_id", "linked.configuration.producer"},
        {"source", "configuration"}, {"required", true}});
    std::ofstream(consumer_manifest_path) << manifest.dump(2);
    const fs::path version_root = paths.install_root / "components" / "linked.configuration.producer" / "1.0.0";
    const fs::path redirected = root / "redirected-producer";
    fs::rename(version_root, redirected);
    std::error_code link_error;fs::create_directory_symlink(redirected, version_root, link_error);
    if (link_error) {
        INFO(link_error.message());
        bool unavailable = link_error == std::errc::operation_not_permitted || link_error == std::errc::permission_denied;
#ifdef _WIN32
        unavailable = unavailable || link_error.value() == ERROR_PRIVILEGE_NOT_HELD;
#endif
        REQUIRE(unavailable);
        remove_locator_test_directory(root);SKIP("directory-link creation is unavailable on this host");
    }
    bool rejected = false;
    try { static_cast<void>(SetupEngine::import_path(paths, "uci_engine", ARENA_MOCK_UCI_PATH)); }
    catch (const std::exception&) { rejected = true; }
    fs::remove(version_root); // Remove the owned link before recursive fixture cleanup.
    REQUIRE(rejected);
    REQUIRE(json::parse(std::ifstream(consumer)).at("schema") == "old");
    REQUIRE_FALSE(fs::exists(paths.data_root / "locators" / "uci_engine.json"));
    REQUIRE(fs::is_regular_file(redirected / "installer-integration.fixture"));
    remove_locator_test_directory(root);
}

TEST_CASE("explicit locator clear revokes a UCI override and retains the managed profile") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const fs::path configuration = write_mock_renderer_consumer(
        paths, "replaceable.engine.consumer", "uci_engine", "runtime", {"saved_locator"});
    const fs::path component_root = paths.install_root / "components" /
                                    "replaceable.engine.consumer" / "1.0.0";
    const fs::path runtime_root = paths.install_root / "components" / "runtime.host" / "1.0.0";
    const fs::path profile = runtime_root / "launch-profile.json";
    const std::string profile_bytes = R"({"executable":"node","arguments":["engine.mjs","--device"]})";
    std::ofstream(profile) << profile_bytes;
    const fs::path manifest_path = component_root / "arena-component.json";
    json manifest = json::parse(std::ifstream(manifest_path));
    const fs::path runtime_manifest_path = runtime_root / "arena-component.json";
    json runtime_manifest = json::parse(std::ifstream(runtime_manifest_path));
    runtime_manifest["files"].push_back({{"path", "launch-profile.json"},
                                        {"size_bytes", profile_bytes.size()},
                                        {"sha256", sha256_text(profile_bytes)}});
    std::ofstream(runtime_manifest_path) << runtime_manifest.dump(2);
    manifest["installer_integration"]["dependency_bindings"].push_back({
        {"name", "managedEngineProfile"}, {"component_id", "runtime.host"},
        {"source", "component_path"}, {"path", "launch-profile.json"}, {"required", true}});
    std::ofstream(manifest_path) << manifest.dump(2);
    static_cast<void>(SetupEngine::import_path(paths, "uci_engine", ARENA_MOCK_UCI_PATH));
    REQUIRE(json::parse(std::ifstream(configuration)).at("locators").contains("selected"));
    const fs::path other = paths.data_root / "locators" / "timing_profile.json";
    std::ofstream(other) << "retained other locator";
    const fs::path credential = root / "operator-owned.fixture";
    std::ofstream(credential) << "non-secret preserved fixture";
    const json cleared = SetupEngine::import_path(paths, "uci_engine", {}, false, {}, true);
    REQUIRE(cleared.at("state") == "cleared");
    REQUIRE(cleared.at("removed") == true);
    REQUIRE_FALSE(fs::exists(paths.data_root / "locators" / "uci_engine.json"));
    const json generated = json::parse(std::ifstream(configuration));
    REQUIRE(generated.at("locators").empty());
    REQUIRE(generated.at("bindings").at("managedEngineProfile") == profile.string());
    REQUIRE(json::parse(std::ifstream(profile)).at("arguments") == json::array({"engine.mjs", "--device"}));
    REQUIRE(fs::file_size(other) == std::string("retained other locator").size());
    REQUIRE(fs::file_size(credential) == std::string("non-secret preserved fixture").size());
    REQUIRE(fs::is_regular_file(ARENA_MOCK_UCI_PATH));
    REQUIRE(SetupEngine::import_path(paths, "uci_engine", {}, false, {}, true).at("removed") == false);
    REQUIRE_THROWS(SetupEngine::import_path(paths, "unknown", {}, false, {}, true));
    try {
        static_cast<void>(SetupEngine::import_path(paths, "uci_engine", {}));
        FAIL("empty semantic import action was accepted");
    } catch (const std::runtime_error& failure) {
        REQUIRE(std::string(failure.what()) == "import requires a path or explicit clear");
    }
    REQUIRE_THROWS(SetupEngine::import_path(paths, "restaurant", {}, false, {}, true));
    REQUIRE_THROWS(SetupEngine::import_path(paths, "uci_engine", profile, false, {}, true));
    REQUIRE_THROWS(SetupEngine::import_path(paths, "uci_engine", {}, true, {}, true));
    REQUIRE_THROWS(SetupEngine::import_path(paths, "opening_book", {}, false,
                                          "immutable_pinned_snapshot", true));
    remove_locator_test_directory(root);
}

TEST_CASE("locator clear restores saved authority and earlier consumer writes on late failure") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const fs::path first = write_mock_renderer_consumer(paths, "a.consumer", "uci_engine");
    const fs::path last = write_mock_renderer_consumer(paths, "z.consumer", "uci_engine");
    static_cast<void>(SetupEngine::import_path(paths, "uci_engine", ARENA_MOCK_UCI_PATH));
    const fs::path locator = paths.data_root / "locators" / "uci_engine.json";
    const json old_locator = json::parse(std::ifstream(locator));
    const json old_first = json::parse(std::ifstream(first));
    const fs::path projection = first.parent_path() / "generated" / "context-projection.json";
    const json old_projection = json::parse(std::ifstream(projection));
    // A directory cannot be replaced by the final configuration file and is
    // not owned as a generated file, even when it happens to be empty.
    // Earlier consumer documents have already been written at this point.
    fs::remove(last);
    fs::create_directory(last);
    REQUIRE_THROWS(SetupEngine::import_path(paths, "uci_engine", {}, false, {}, true));
    REQUIRE(json::parse(std::ifstream(locator)) == old_locator);
    REQUIRE(json::parse(std::ifstream(first)) == old_first);
    REQUIRE(json::parse(std::ifstream(projection)) == old_projection);
    REQUIRE(fs::is_directory(last));
    remove_locator_test_directory(root);
}

TEST_CASE("locator clear reports failed compensation and retains exact recovery bytes") {
#ifdef _WIN32
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const fs::path configuration = write_mock_renderer_consumer(paths, "locked.consumer", "uci_engine");
    static_cast<void>(SetupEngine::import_path(paths, "uci_engine", ARENA_MOCK_UCI_PATH));
    const json previous = json::parse(std::ifstream(configuration));
    const fs::path projection = configuration.parent_path() / "generated" / "context-projection.json";
    const json previous_projection = json::parse(std::ifstream(projection));
    const fs::path locator = paths.data_root / "locators" / "uci_engine.json";
    const json previous_locator = json::parse(std::ifstream(locator));
    // Source bytes remain readable for backup, but the real Windows handle
    // denies deletion/replacement during both publication and compensation.
    const HANDLE held = CreateFileW(configuration.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(held != INVALID_HANDLE_VALUE);
    std::string failure;
    try {
        static_cast<void>(SetupEngine::import_path(paths, "uci_engine", {}, false, {}, true));
    } catch (const std::exception& error) {
        failure = error.what();
    } catch (...) {
        CloseHandle(held);
        throw;
    }
    CloseHandle(held);
    REQUIRE(failure.find("cannot atomically activate " + configuration.string()) == 0);
    REQUIRE(failure.find("consumer rollback failed") != std::string::npos);
    REQUIRE(failure.find("backup retained at") != std::string::npos);
    fs::path backup;
    const std::string prefix = configuration.filename().string() + ".import-rollback-";
    for (const auto& entry : fs::directory_iterator(configuration.parent_path())) {
        if (entry.path().filename().string().starts_with(prefix)) backup = entry.path();
    }
    REQUIRE_FALSE(backup.empty());
    REQUIRE(failure.find(backup.string()) != std::string::npos);
    REQUIRE(json::parse(std::ifstream(backup)) == previous);
    REQUIRE(json::parse(std::ifstream(configuration)) == previous);
    REQUIRE(json::parse(std::ifstream(projection)) == previous_projection);
    REQUIRE(json::parse(std::ifstream(locator)) == previous_locator);
    remove_locator_test_directory(root);
#else
    SKIP("native delete-sharing compensation falsifier requires Windows");
#endif
}

TEST_CASE("clearing a managed-copy locator preserves its payload and receipt") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const fs::path source = root / "book.bin";
    std::ofstream(source) << "retained book";
    const json imported = SetupEngine::import_path(paths, "opening_book", source, true);
    const fs::path active = imported.at("path").get<std::string>();
    const std::string id = imported.at("receipt").at("component_id").get<std::string>();
    const fs::path receipt = paths.data_root / "receipts" / (id + ".json");
    static_cast<void>(SetupEngine::import_path(paths, "opening_book", {}, false, {}, true));
    REQUIRE(fs::is_regular_file(active / "book.bin"));
    REQUIRE(fs::is_regular_file(source));
    REQUIRE(json::parse(std::ifstream(receipt)) == imported.at("receipt"));
    remove_locator_test_directory(root);
}

TEST_CASE("clear revokes stale qualified-profile references without inspecting their targets") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    fs::create_directories(paths.data_root / "locators");
    const fs::path retained = root / "historical-profile.fixture";
    const std::string historical_bytes = "unqualified historical fixture, not qualification evidence";
    std::ofstream(retained) << historical_bytes;
    for (const std::string kind : {"timing_profile", "lichess_single_game_profile",
                                   "lichess_capacity_profile"}) {
        const fs::path locator = paths.data_root / "locators" / (kind + ".json");
        // Retained historical metadata deliberately points at an incompatible
        // fixture. Revocation must not require requalifying or opening it.
        std::ofstream(locator) << json{{"schema", "arena_saved_locator_v1"},
                                      {"schema_version", 1}, {"kind", kind},
                                      {"mode", "in_place_reference"},
                                      {"path", retained.string()}}.dump();
        const json result = SetupEngine::import_path(paths, kind, {}, false, {}, true);
        REQUIRE(result.at("removed") == true);
        REQUIRE_FALSE(fs::exists(locator));
        REQUIRE(fs::file_size(retained) == historical_bytes.size());
        REQUIRE(SetupEngine::import_path(paths, kind, {}, false, {}, true).at("removed") == false);
    }
    remove_locator_test_directory(root);
}

TEST_CASE("cleared managed single-game profile stays absent from rendered consumer context") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    // Default locator source policy permits receipt fallback: prove that the
    // retained managed-copy receipt is not itself profile selection authority.
    const fs::path configuration = write_mock_renderer_consumer(
        paths, "replaceable.profile.consumer", "lichess_single_game_profile");
    const fs::path source = write_lichess_single_profile(root, "qualified-fixture.json");
    const json imported = SetupEngine::import_path(paths, "lichess_single_game_profile", source);
    const fs::path managed = imported.at("path").get<std::string>();
    const std::string id = imported.at("receipt").at("component_id").get<std::string>();
    const fs::path receipt = paths.data_root / "receipts" / (id + ".json");
    REQUIRE(json::parse(std::ifstream(configuration)).at("locators").at("selected") == managed.string());
    static_cast<void>(SetupEngine::import_path(paths, "lichess_single_game_profile", {}, false, {}, true));
    REQUIRE(json::parse(std::ifstream(configuration)).at("locators").empty());
    REQUIRE(json::parse(std::ifstream(configuration)).at("locator_details").empty());
    const fs::path projection = configuration.parent_path() / "generated" / "context-projection.json";
    REQUIRE(json::parse(std::ifstream(projection)).at("locators").empty());
    REQUIRE(fs::is_regular_file(managed));
    REQUIRE(fs::is_regular_file(source));
    REQUIRE(json::parse(std::ifstream(receipt)) == imported.at("receipt"));
    remove_locator_test_directory(root);
}

TEST_CASE("opening-book authority mode is explicit and reaches declared consumers") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const fs::path configuration = write_mock_renderer_consumer(
        paths, "replaceable.book.authority.consumer", "opening_book", "runtime",
        {"saved_locator"});
    const fs::path live_channel = root / "published" / "integration";
    fs::create_directories(live_channel);
    std::ofstream(live_channel / "strong_rare_v1.bin") << "live channel book";

    const json imported = SetupEngine::import_path(
        paths, "opening_book", live_channel, false,
        "service_managed_live_channel");
    REQUIRE(imported.at("authority_mode") == "service_managed_live_channel");
    const json locator = json::parse(std::ifstream(
        paths.data_root / "locators" / "opening_book.json"));
    REQUIRE(locator.at("mode") == "in_place_reference");
    REQUIRE(locator.at("authority_mode") == "service_managed_live_channel");

    const json generated = json::parse(std::ifstream(configuration));
    const json& details = generated.at("locator_details").at("selected");
    REQUIRE(details.at("kind") == "opening_book");
    REQUIRE(details.at("source") == "saved_locator");
    REQUIRE(details.at("storage_mode") == "in_place_reference");
    REQUIRE(details.at("authority_mode") == "service_managed_live_channel");
    REQUIRE(details.at("path") ==
            fs::absolute(live_channel).lexically_normal().string());

    REQUIRE_THROWS(
        SetupEngine::import_path(paths, "opening_book", live_channel, true,
                                 "service_managed_live_channel"));
    const fs::path pinned_file = root / "pinned.bin";
    std::ofstream(pinned_file) << "pinned book";
    REQUIRE_THROWS(
        SetupEngine::import_path(paths, "opening_book", pinned_file, false,
                                 "service_managed_live_channel"));
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

TEST_CASE("stale optional locator falls back to validated managed knowledge") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const fs::path source = root / "managed-book";
    fs::create_directories(source);
    std::ofstream(source / "book.bin") << "managed knowledge";
    const json managed =
        SetupEngine::import_path(paths, "opening_book", source, true);
    const fs::path managed_root = managed.at("path").get<std::string>();

    const fs::path locator_path = paths.data_root / "locators" / "opening_book.json";
    json stale_locator = json::parse(std::ifstream(locator_path));
    stale_locator["path"] = (root / "retired-book.bin").string();
    std::ofstream(locator_path, std::ios::binary | std::ios::trunc)
        << stale_locator.dump(2);

    const fs::path configuration = write_mock_renderer_consumer(
        paths, "replaceable.book.consumer", "opening_book");
    const json locators{{"selected", managed_root.string()}};
    const json locator_details{
        {"selected",
         {{"kind", "opening_book"},
          {"path", managed_root.string()},
          {"source", "install_receipt"},
          {"storage_mode", "installed_component"},
          {"authority_mode", "immutable_pinned_snapshot"}}},
    };
    std::ofstream(configuration, std::ios::binary | std::ios::trunc)
        << json{{"schema", "replaceable_provider_configuration_v1"},
                {"enabled", false},
                {"bindings", json::object()},
                {"locators", locators},
                {"locator_details", locator_details}}
               .dump(2);
    const fs::path generated =
        configuration.parent_path() / "generated" / "context-projection.json";
    fs::create_directories(generated.parent_path());
    std::ofstream(generated, std::ios::binary)
        << json{{"schema", "replaceable_provider_projection_v1"},
                {"locators", locators},
                {"locator_details", locator_details}}
               .dump(2);

    const json verified = SetupEngine::verify(paths);
    INFO(verified.dump(2));
    REQUIRE(verified.at("ok") == true);
    const json inventory = SetupEngine::inventory(paths);
    REQUIRE(inventory.at("discovery")
                .at("opening_book")
                .at("selection")
                .at("source") == "install_receipt");
    REQUIRE(fs::is_regular_file(locator_path));
    REQUIRE(json::parse(std::ifstream(locator_path)).at("path") ==
            stale_locator.at("path"));
    fs::remove_all(root);
}

TEST_CASE("provider locator source policy keeps managed receipts out of saved overrides") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const fs::path managed_source = root / "managed-book";
    fs::create_directories(managed_source);
    std::ofstream(managed_source / "book.bin") << "managed knowledge";
    static_cast<void>(
        SetupEngine::import_path(paths, "opening_book", managed_source, true));

    const fs::path locator_path = paths.data_root / "locators" / "opening_book.json";
    json stale_locator = json::parse(std::ifstream(locator_path));
    stale_locator["path"] = (root / "retired-book.bin").string();
    std::ofstream(locator_path, std::ios::binary | std::ios::trunc)
        << stale_locator.dump(2);

    const fs::path configuration = write_mock_renderer_consumer(
        paths, "replaceable.saved-only.consumer", "opening_book", "runtime",
        {"saved_locator"});
    const json empty_locators = json::object();
    std::ofstream(configuration, std::ios::binary | std::ios::trunc)
        << json{{"schema", "replaceable_provider_configuration_v1"},
                {"enabled", false},
                {"bindings", json::object()},
                {"locators", empty_locators},
                {"locator_details", json::object()}}
               .dump(2);
    const fs::path projection =
        configuration.parent_path() / "generated" / "context-projection.json";
    fs::create_directories(projection.parent_path());
    std::ofstream(projection, std::ios::binary)
        << json{{"schema", "replaceable_provider_projection_v1"},
                {"locators", empty_locators},
                {"locator_details", json::object()}}
               .dump(2);

    const json verified = SetupEngine::verify(paths);
    INFO(verified.dump(2));
    REQUIRE(verified.at("ok") == true);

    const fs::path operator_selected = root / "operator-selected.bin";
    std::ofstream(operator_selected) << "explicit operator selection";
    const json imported =
        SetupEngine::import_path(paths, "opening_book", operator_selected, false);
    REQUIRE(imported.at("refreshed_consumers") ==
            json::array({"replaceable.saved-only.consumer"}));
    REQUIRE(json::parse(std::ifstream(configuration))
                .at("locators")
                .at("selected") ==
            fs::absolute(operator_selected).lexically_normal().string());
    fs::remove_all(root);
}

TEST_CASE("provider upgrade migrates its workspace and active locators into product data") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    constexpr std::string_view component_id = "replaceable.product-data.provider";
    const std::string id(component_id);
    const fs::path legacy_configuration = write_mock_renderer_consumer(
        paths, id, "opening_book");
    const fs::path legacy_workspace = legacy_configuration.parent_path();
    const fs::path legacy_book = legacy_workspace / "published" / "live.book";
    fs::create_directories(legacy_book.parent_path());
    std::ofstream(legacy_book, std::ios::binary) << "provider-published-book";
    static_cast<void>(
        SetupEngine::import_path(paths, "opening_book", legacy_book, false));

    const fs::path package_root = root / "provider-package";
    fs::create_directories(package_root);
    fs::copy_file(
        paths.install_root / "components" / id / "1.0.0" /
            "installer-integration.fixture",
        package_root / "installer-integration.fixture");
    json manifest = json::parse(std::ifstream(
        paths.install_root / "components" / id / "1.0.0" /
        "arena-component.json"));
    manifest["component_version"] = "2.0.0";
    manifest["installer_integration"]["workspace_placement"] = "product_data";
    std::ofstream(package_root / "arena-component.json", std::ios::binary)
        << manifest.dump(2);
    const fs::path archive = root / "provider-2.0.0.tar.gz";
    write_component_archive(package_root, archive);

    const json plan{
        {"schema", "arena_install_plan_v1"},
        {"blockers", json::array()},
        {"offline", true},
        {"catalog_directory", root.string()},
        {"actions",
         json::array({{{"action", "install"},
                       {"component_id", id},
                       {"version", "2.0.0"},
                       {"dependencies", json::array({"runtime.host"})},
                       {"enabled", false},
                       {"artifact",
                        {{"type", "archive"},
                         {"source", archive.filename().string()},
                         {"sha256", sha256_path(archive)},
                         {"size_bytes", fs::file_size(archive)}}}}})},
    };
#ifdef _WIN32
    const HANDLE workspace_lock =
        CreateFileW(legacy_book.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(workspace_lock != INVALID_HANDLE_VALUE);
    std::jthread release_workspace_lock([workspace_lock] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CloseHandle(workspace_lock);
    });
#endif
    const json applied =
        SetupEngine::apply(json::object(), plan, paths, true, false);
#ifdef _WIN32
    release_workspace_lock.join();
#endif
    INFO(applied.dump(2));
    REQUIRE(applied.at("state") == "committed");

    const fs::path product_workspace =
        paths.data_root / "provider-data" / id;
    const fs::path product_book = product_workspace / "published" / "live.book";
    REQUIRE_FALSE(fs::exists(legacy_workspace));
    REQUIRE(fs::is_regular_file(product_book));
    REQUIRE(json::parse(std::ifstream(paths.data_root / "locators" /
                                      "opening_book.json"))
                .at("path") == fs::absolute(product_book).lexically_normal().string());
    REQUIRE(json::parse(std::ifstream(product_workspace / "manager-config.json"))
                .at("locators")
                .at("selected") ==
            fs::absolute(product_book).lexically_normal().string());
    const json receipt = json::parse(std::ifstream(
        paths.data_root / "receipts" / (id + ".json")));
    REQUIRE(receipt.at("data_paths") == json::array({product_workspace.string()}));
    REQUIRE(receipt.at("configuration_path") ==
            (product_workspace / "manager-config.json").string());
    REQUIRE(SetupEngine::verify(paths).at("ok") == true);

    const fs::path failing_package_root = root / "provider-package-failing";
    fs::create_directories(failing_package_root);
    fs::copy_file(
        paths.install_root / "components" / id / "2.0.0" /
            "installer-integration.fixture",
        failing_package_root / "installer-integration.fixture");
    json failing_manifest = json::parse(std::ifstream(
        paths.install_root / "components" / id / "2.0.0" /
        "arena-component.json"));
    failing_manifest["component_version"] = "3.0.0";
    failing_manifest["installer_integration"]["workspace_placement"] =
        "workspace_root";
    failing_manifest["installer_integration"]["locator_bindings"][0]["kind"] =
        "timing_profile";
    failing_manifest["installer_integration"]["locator_bindings"][0]["required"] =
        true;
    std::ofstream(failing_package_root / "arena-component.json", std::ios::binary)
        << failing_manifest.dump(2);
    const fs::path failing_archive = root / "provider-3.0.0.tar.gz";
    write_component_archive(failing_package_root, failing_archive);
    json failing_plan = plan;
    failing_plan["actions"][0]["version"] = "3.0.0";
    failing_plan["actions"][0]["artifact"] = {
        {"type", "archive"},
        {"source", failing_archive.filename().string()},
        {"sha256", sha256_path(failing_archive)},
        {"size_bytes", fs::file_size(failing_archive)},
    };
    REQUIRE_THROWS(
        SetupEngine::apply(json::object(), failing_plan, paths, true, false));
    REQUIRE_FALSE(fs::exists(legacy_workspace));
    REQUIRE(fs::is_regular_file(product_book));
    REQUIRE(json::parse(std::ifstream(paths.data_root / "locators" /
                                      "opening_book.json"))
                .at("path") == fs::absolute(product_book).lexically_normal().string());
    REQUIRE(json::parse(std::ifstream(
                paths.data_root / "receipts" / (id + ".json")))
                .at("component_version") == "2.0.0");
    REQUIRE(SetupEngine::verify(paths).at("ok") == true);
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

TEST_CASE("typed v3 install stores only neutral lifecycle evidence") {
    std::string variant;
    bool succeeds = true;
    bool deny_safe_inactive = false;
    bool deny_journal_replacement = false;
    SECTION("ready") { variant = "ready"; }
    SECTION("safe inactive") { variant = "safe_inactive"; }
    SECTION("safe inactive without permission") {
        variant = "safe_inactive";
        succeeds = false;
        deny_safe_inactive = true;
    }
    SECTION("failed") {
        variant = "failed";
        succeeds = false;
    }
    SECTION("malformed") {
        variant = "malformed";
        succeeds = false;
    }
    SECTION("untrusted parse failure") {
        variant = "invalid_canary";
        succeeds = false;
    }
#ifdef _WIN32
    SECTION("persistent journal replacement failure preserves original cause") {
        variant = "failed";
        succeeds = false;
        deny_journal_replacement = true;
    }
#endif

    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const std::string component_id = "replaceable.typed-install.consumer";
    static_cast<void>(write_mock_renderer_consumer(
        paths, component_id, "lichess_transport_profile"));
    const fs::path receipt_path =
        paths.data_root / "receipts" / (component_id + ".json");
    const json previous_receipt = json::parse(std::ifstream(receipt_path));
    const fs::path installed_root =
        previous_receipt.at("active_root").get<std::string>();
    json manifest = json::parse(
        std::ifstream(installed_root / "arena-component.json"));
    json lifecycle_control = typed_lifecycle_control(component_id, variant);
    const fs::path diagnostic_barrier = root / "diagnostic-barrier";
    if (deny_journal_replacement) {
        lifecycle_control["operations"]["install"]["arguments"].push_back("--diagnostic-barrier");
        lifecycle_control["operations"]["install"]["arguments"].push_back(diagnostic_barrier.string());
    }
    if (deny_safe_inactive) {
        lifecycle_control["operations"]["install"]["success_dispositions"] =
            json::array({"ready"});
    }
    manifest["installer_integration"]["startup_control"] =
        std::move(lifecycle_control);
    std::ofstream(installed_root / "arena-component.json",
                  std::ios::binary | std::ios::trunc)
        << manifest.dump(2);
    const fs::path archive = root / "typed-install.tar.gz";
    write_component_archive(installed_root, archive);
    fs::remove_all(paths.install_root / "components" / component_id);
    fs::remove(receipt_path);

    const json plan{
        {"schema", "arena_install_plan_v1"},
        {"blockers", json::array()},
        {"offline", true},
        {"catalog_directory", root.string()},
        {"actions",
         json::array({{{"action", "install"},
                       {"component_id", component_id},
                       {"version", "1.0.0"},
                       {"dependencies", json::array({"runtime.host"})},
                       {"enabled", true},
                       {"artifact",
                        {{"type", "archive"},
                         {"source", archive.filename().string()},
                         {"sha256", sha256_path(archive)},
                         {"size_bytes", fs::file_size(archive)}}}}})},
    };

    if (!succeeds) {
        std::string failure_message;
#ifdef _WIN32
        bool journal_locked = false;
        std::string lock_failure;
        std::optional<std::jthread> journal_guard;
        if (deny_journal_replacement) {
            journal_guard.emplace(hold_transaction_journal_after_invocation(
                paths, diagnostic_barrier, journal_locked, lock_failure));
        }
#endif
        try { static_cast<void>(SetupEngine::apply(json::object(), plan, paths, true, false)); }
        catch (const std::exception& error) { failure_message = error.what(); }
#ifdef _WIN32
        if (journal_guard) { journal_guard->request_stop(); journal_guard->join(); }
#endif
        REQUIRE_FALSE(failure_message.empty());
        fs::path failed_journal;
        for (const auto& entry : fs::directory_iterator(paths.data_root / "transactions")) {
            if (entry.path().extension() == ".json") {
                REQUIRE(failed_journal.empty());
                failed_journal = entry.path();
            }
        }
        REQUIRE_FALSE(failed_journal.empty());
        const json failed_transaction = json::parse(std::ifstream(failed_journal));
#ifdef _WIN32
        if (deny_journal_replacement) {
            REQUIRE(lock_failure.empty());
            REQUIRE(journal_locked);
            INFO(failure_message);
            REQUIRE(failure_message.find("reason=fixture.failed; opaque setup fixture reason") != std::string::npos);
            REQUIRE(failure_message.find("diagnostic journal persistence failed") != std::string::npos);
            REQUIRE(failure_message.find("rollback failed:") == std::string::npos);
            REQUIRE(failure_message.find("final transaction journal persistence failed") != std::string::npos);
            REQUIRE(failure_message.find("final journal state not persisted") != std::string::npos);
            REQUIRE(failed_transaction.at("provider_lifecycle_failures").empty());
            REQUIRE(failed_transaction.at("state") == "running");
            REQUIRE_FALSE(fs::exists(receipt_path));
            remove_locator_test_directory(root);
            return;
        }
#endif
        REQUIRE(failed_transaction.at("provider_lifecycle_failures").size() >= 1);
        const auto& failure = failed_transaction.at("provider_lifecycle_failures").at(0);
        REQUIRE(failure.at("result").at("disposition") == "failed");
        REQUIRE(failure.at("result").at("component_id") == component_id);
        REQUIRE(failure.at("request").at("runtime").is_string());
        REQUIRE(failure.at("request").at("controller_root").is_string());
        REQUIRE_FALSE(failure.at("request").contains("arguments"));
        REQUIRE(failure.at("phase") == "forward");
        REQUIRE(failure.at("request").at("configuration_argument_present") == true);
        REQUIRE(failed_transaction.dump().find("CANARY") == std::string::npos);
        REQUIRE(failure_message.find("CANARY") == std::string::npos);
        if (variant == "invalid_canary") {
            REQUIRE(failure.at("result").at("reason").at("message").get<std::string>().find(
                        "category=invalid_json") != std::string::npos);
            REQUIRE(failure_message.find("category=invalid_json") != std::string::npos);
        }
        if (variant == "failed") {
            REQUIRE(failure.at("result").at("reason").at("id") == "fixture.failed");
            REQUIRE(failure.at("result").at("reason").at("message") ==
                    "opaque setup fixture reason");
        }
        REQUIRE_FALSE(fs::exists(receipt_path));
        REQUIRE_FALSE(fs::exists(
            paths.install_root / "components" / component_id / "1.0.0"));
        remove_locator_test_directory(root);
        return;
    }

    const json applied =
        SetupEngine::apply(json::object(), plan, paths, true, false);
    REQUIRE(applied.at("state") == "committed");
    const json receipt = json::parse(std::ifstream(receipt_path));
    const json& lifecycle = receipt.at("startup");
    REQUIRE(lifecycle.at("schema") ==
            "arena_provider_lifecycle_result_v1");
    REQUIRE(lifecycle.at("operation") == "install");
    REQUIRE(lifecycle.at("disposition") ==
            (variant == "safe_inactive" ? "safe_inactive" : "ready"));
    REQUIRE(lifecycle.at("observed").at("activity") ==
            (variant == "safe_inactive" ? "inactive" : "active"));
    if (variant == "safe_inactive") {
        REQUIRE(lifecycle.at("reason").at("id") ==
                "fixture.safe_inactive");
        REQUIRE(lifecycle.at("reason").at("message") ==
                "opaque setup fixture reason");
    }
    const json transaction = json::parse(std::ifstream(
        paths.data_root / "transactions" /
        (applied.at("transaction_id").get<std::string>() + ".json")));
    REQUIRE(transaction.at("actions").at(0).at("lifecycle_result") ==
            lifecycle);
    const std::string persisted = receipt.dump() + transaction.dump();
    REQUIRE(persisted.find("requalificationRequired") == std::string::npos);
    REQUIRE(persisted.find("requalification_required") == std::string::npos);
    REQUIRE(persisted.find("resume_deferred") == std::string::npos);
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

    REQUIRE(result.at("components")
                .at(0)
                .at("lifecycle_result")
                .at("observed")
                .at("installation") == "absent");
    REQUIRE_FALSE(fs::exists(paths.install_root / "components" / component_id));
    REQUIRE(fs::is_regular_file(workspace / "valuable-state.json"));
    fs::remove_all(root);
}

TEST_CASE("provider startup control accepts the provider workspace root") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const std::string component_id = "replaceable.root-workspace.consumer";
    const fs::path configuration = write_mock_renderer_consumer(
        paths, component_id, "lichess_transport_profile", ".");
    const fs::path workspace = configuration.parent_path();
    std::ofstream(workspace / "valuable-state.json") << "{}";

    const json plan = SetupEngine::plan_uninstall(paths, {component_id});
    const json result = SetupEngine::uninstall(paths, plan, true);

    REQUIRE(result.at("components")
                .at(0)
                .at("lifecycle_result")
                .at("observed")
                .at("installation") == "absent");
    REQUIRE_FALSE(fs::exists(paths.install_root / "components" / component_id));
    REQUIRE(fs::is_regular_file(workspace / "valuable-state.json"));
    fs::remove_all(root);
}

TEST_CASE("staged typed v3 lifecycle quiesces and resumes with neutral evidence") {
    bool safe_inactive = false;
    bool fail_resume = false;
    bool deny_journal_replacement = false;
    std::string component_id;
    SECTION("live replacement resumes") {
        component_id = "replaceable.staged-lifecycle.consumer";
    }
    SECTION("declared safe inactive replacement remains inactive") {
        component_id = "replaceable.safe-inactive.consumer";
        safe_inactive = true;
    }
    SECTION("forward and compensation failures retain distinct neutral causes") {
        component_id = "replaceable.failed-resume.consumer";
        fail_resume = true;
    }
#ifdef _WIN32
    SECTION("journal failure retains original and real compensation failure") {
        component_id = "replaceable.failed-resume.consumer";
        fail_resume = true;
        deny_journal_replacement = true;
    }
#endif
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    static_cast<void>(write_mock_renderer_consumer(
        paths, component_id, "lichess_transport_profile"));

    const fs::path receipt_path =
        paths.data_root / "receipts" / (component_id + ".json");
    json previous_receipt = json::parse(std::ifstream(receipt_path));
    REQUIRE_FALSE(previous_receipt.contains("startup"));
    previous_receipt["service_contract"] = {{"credential_gated_startup", true}};
    std::ofstream(receipt_path, std::ios::binary | std::ios::trunc)
        << previous_receipt.dump(2);

    const fs::path previous_root =
        paths.install_root / "components" / component_id / "1.0.0";
    json previous_manifest = json::parse(
        std::ifstream(previous_root / "arena-component.json"));
    previous_manifest["installer_integration"]["startup_control"]["actions"]
                     ["uninstall"] = json::array({"lifecycle", "legacy_disable"});
    std::ofstream(previous_root / "arena-component.json",
                  std::ios::binary | std::ios::trunc)
        << previous_manifest.dump(2);

    const fs::path runtime_package_root = root / "post-replace-runtime-package";
    const fs::path runtime_relative =
        fs::path("bin") /
        (std::string("post-replace-only-runtime") +
         fs::path(ARENA_MOCK_SERVICE_PATH).extension().string());
    const fs::path runtime_executable = runtime_package_root / runtime_relative;
    fs::create_directories(runtime_executable.parent_path());
    fs::copy_file(ARENA_MOCK_SERVICE_PATH, runtime_executable);
    std::ofstream(runtime_package_root / "arena-component.json", std::ios::binary)
        << json{
               {"schema", "arena_install_component_v1"},
               {"schema_version", 1},
               {"component_id", "runtime.host"},
               {"component_version", "1.0.0"},
               {"entrypoints", {{"execute", runtime_relative.generic_string()}}},
               {"dependencies", json::array()},
               {"files",
                json::array({{{"path", runtime_relative.generic_string()},
                              {"size_bytes", fs::file_size(runtime_executable)},
                              {"sha256", sha256_path(runtime_executable)}}})},
           }
               .dump(2);
    const fs::path runtime_archive = root / "runtime.host-1.0.0.tar.gz";
    write_component_archive(runtime_package_root, runtime_archive);

    const fs::path package_root = root / "staged-provider-package";
    fs::create_directories(package_root);
    fs::copy_file(previous_root / "installer-integration.fixture",
                  package_root / "installer-integration.fixture");
    json replacement_manifest = previous_manifest;
    replacement_manifest["component_version"] = "1.0.0";
    replacement_manifest["installer_integration"]["startup_control"] =
        typed_lifecycle_control(
            component_id, "ready",
            fail_resume ? "failed" : safe_inactive ? "safe_inactive" : "ready", true);
    const fs::path diagnostic_barrier = root / "diagnostic-barrier";
    if (deny_journal_replacement) {
        auto& args = replacement_manifest["installer_integration"]["startup_control"]["operations"]["post_replace"]["arguments"];
        args.push_back("--diagnostic-barrier");
        args.push_back(diagnostic_barrier.string());
    }
    std::ofstream(package_root / "arena-component.json", std::ios::binary)
        << replacement_manifest.dump(2);
    const fs::path archive = root / "staged-provider-1.0.0.tar.gz";
    write_component_archive(package_root, archive);

    const json plan{
        {"schema", "arena_install_plan_v1"},
        {"blockers", json::array()},
        {"offline", true},
        {"catalog_directory", root.string()},
        {"actions",
         json::array({{{"action", "install"},
                       {"component_id", "runtime.host"},
                       {"version", "1.0.0"},
                       {"dependencies", json::array()},
                       {"enabled", true},
                       {"artifact",
                        {{"type", "archive"},
                         {"source", runtime_archive.filename().string()},
                         {"sha256", sha256_path(runtime_archive)},
                         {"size_bytes", fs::file_size(runtime_archive)}}}},
                      {{"action", "install"},
                       {"component_id", component_id},
                       {"version", "1.0.0"},
                       {"dependencies", json::array({"runtime.host"})},
                       {"enabled", false},
                       {"artifact",
                        {{"type", "archive"},
                         {"source", archive.filename().string()},
                         {"sha256", sha256_path(archive)},
                         {"size_bytes", fs::file_size(archive)}}}}})},
    };
    if (fail_resume) {
        std::string failure_message;
#ifdef _WIN32
        bool journal_locked = false;
        std::string lock_failure;
        std::optional<std::jthread> journal_guard;
        if (deny_journal_replacement) journal_guard.emplace(hold_transaction_journal_after_invocation(
            paths, diagnostic_barrier, journal_locked, lock_failure));
#endif
        try { static_cast<void>(SetupEngine::apply(json::object(), plan, paths, true, true)); }
        catch (const std::exception& error) { failure_message = error.what(); }
#ifdef _WIN32
        if (journal_guard) { journal_guard->request_stop(); journal_guard->join(); }
        if (deny_journal_replacement) {
            REQUIRE(lock_failure.empty());
            REQUIRE(journal_locked);
            INFO(failure_message);
            REQUIRE(failure_message.starts_with("provider lifecycle operation failed: " + component_id +
                                               "; reason=fixture.failed; opaque setup fixture reason"));
            REQUIRE(failure_message.find("diagnostic journal persistence failed") != std::string::npos);
            REQUIRE(failure_message.find("rollback failed: startup:") != std::string::npos);
            REQUIRE(failure_message.find("final transaction journal persistence failed") != std::string::npos);
            REQUIRE(failure_message.find("final journal state not persisted") != std::string::npos);
            remove_locator_test_directory(root);
            return;
        }
#endif
        REQUIRE(failure_message.find("fixture.failed") != std::string::npos);
        const auto transaction = json::parse(std::ifstream(
            fs::directory_iterator(paths.data_root / "transactions")->path()));
        const auto& failures = transaction.at("provider_lifecycle_failures");
        REQUIRE(failures.size() >= 2);
        REQUIRE(failures.at(0).at("phase") == "forward");
        REQUIRE(failures.at(0).at("result").at("operation") == "post_replace");
        REQUIRE(failures.at(0).at("result").at("reason").at("id") == "fixture.failed");
        REQUIRE(failures.back().at("phase") == "compensation");
        for (const auto& failure : failures) {
            REQUIRE(failure.at("result").at("disposition") == "failed");
            REQUIRE(failure.at("result").at("reason").at("id").is_string());
            REQUIRE(failure.at("request").at("replacement_arguments_present") == true);
            REQUIRE_FALSE(failure.at("request").at("previous_root").get<std::string>().empty());
            REQUIRE_FALSE(failure.at("request").contains("arguments"));
        }
        remove_locator_test_directory(root);
        return;
    }
    const json applied =
        SetupEngine::apply(json::object(), plan, paths, true, true);
    INFO(applied.dump(2));
    REQUIRE(applied.at("state") == "committed");
    REQUIRE(json::parse(std::ifstream(receipt_path)).at("component_version") ==
            "1.0.0");
    const json receipt = json::parse(std::ifstream(receipt_path));
    REQUIRE(receipt.at("startup").at("schema") ==
            "arena_provider_lifecycle_result_v1");
    REQUIRE(receipt.at("startup").at("operation") == "post_replace");
    REQUIRE(receipt.at("startup").at("disposition") ==
            (safe_inactive ? "safe_inactive" : "ready"));
    REQUIRE(receipt.at("startup").at("observed").at("activity") ==
            (safe_inactive ? "inactive" : "active"));
    const json transaction = json::parse(std::ifstream(
        paths.data_root / "transactions" /
        (applied.at("transaction_id").get<std::string>() + ".json")));
    REQUIRE(transaction.at("startup_quiescence").size() == 1);
    REQUIRE(transaction.at("startup_quiescence").at(0).at("component_id") ==
            component_id);
    REQUIRE(transaction.at("startup_quiescence").at(0).at("state") ==
            (safe_inactive ? "safe_inactive" : "ready"));
    REQUIRE(transaction.at("startup_quiescence")
                .at(0)
                .at("lifecycle_result") == receipt.at("startup"));
    const std::string persisted = receipt.dump() + transaction.dump();
    REQUIRE(persisted.find("requalificationRequired") == std::string::npos);
    REQUIRE(persisted.find("requalification_required") == std::string::npos);
    REQUIRE(persisted.find("resume_deferred") == std::string::npos);
    REQUIRE(transaction.at("actions").at(0).at("component_id") == "runtime.host");
    REQUIRE(fs::is_directory(paths.install_root / "components" / component_id /
                             "1.0.0"));
    fs::remove_all(root);
}

TEST_CASE("dependency replacement refreshes exact-current consumers before resume") {
    bool fail_after_refresh = false;
    std::string component_id = "replaceable.dependency.consumer";
    SECTION("committed consumer selects replacement dependency") {}
    SECTION("later failure restores prior generated configuration") {
        fail_after_refresh = true;
        component_id = "replaceable.fail-new-binding.consumer";
    }

    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const fs::path configuration = write_mock_renderer_consumer(
        paths, component_id, "lichess_transport_profile");
    const fs::path consumer_root =
        paths.install_root / "components" / component_id / "1.0.0";
    const fs::path consumer_manifest_path = consumer_root / "arena-component.json";
    json consumer_manifest = json::parse(std::ifstream(consumer_manifest_path));
    consumer_manifest["installer_integration"]["dependency_bindings"] =
        json::array({{{"name", "runtime"},
                      {"component_id", "runtime.host"},
                      {"source", "active_root"},
                      {"required", true}}});
    std::ofstream(consumer_manifest_path, std::ios::binary | std::ios::trunc)
        << consumer_manifest.dump(2);

    const fs::path old_runtime_root =
        paths.install_root / "components" / "runtime.host" / "1.0.0";
    const json old_configuration{
        {"schema", "replaceable_provider_configuration_v1"},
        {"enabled", true},
        {"bindings", {{"runtime", old_runtime_root.string()}}},
        {"locators", json::object()},
        {"locator_details", json::object()},
    };
    std::ofstream(configuration, std::ios::binary | std::ios::trunc)
        << old_configuration.dump(2);
    const fs::path generated =
        configuration.parent_path() / "generated" / "context-projection.json";
    REQUIRE_FALSE(fs::exists(generated));
    const fs::path consumer_receipt_path =
        paths.data_root / "receipts" / (component_id + ".json");
    json consumer_receipt = json::parse(std::ifstream(consumer_receipt_path));
    consumer_receipt["state"] = "installed_managed";
    consumer_receipt["startup"] = {{"installed", true}};
    std::ofstream(consumer_receipt_path, std::ios::binary | std::ios::trunc)
        << consumer_receipt.dump(2);

    const fs::path replacement_package = root / "runtime-host-replacement";
    const fs::path runtime_relative = fs::path("bin") /
                                      fs::path(ARENA_MOCK_SERVICE_PATH).filename();
    const fs::path replacement_runtime = replacement_package / runtime_relative;
    fs::create_directories(replacement_runtime.parent_path());
    fs::copy_file(ARENA_MOCK_SERVICE_PATH, replacement_runtime);
    std::ofstream(replacement_package / "arena-component.json", std::ios::binary)
        << json{
               {"schema", "arena_install_component_v1"},
               {"schema_version", 1},
               {"component_id", "runtime.host"},
               {"component_version", "2.0.0"},
               {"entrypoints", {{"execute", runtime_relative.generic_string()}}},
               {"dependencies", json::array()},
               {"files",
                json::array({{{"path", runtime_relative.generic_string()},
                              {"size_bytes", fs::file_size(replacement_runtime)},
                              {"sha256", sha256_path(replacement_runtime)}}})},
           }
               .dump(2);
    const fs::path replacement_archive = root / "runtime.host-2.0.0.tar.gz";
    write_component_archive(replacement_package, replacement_archive);
    const json plan{
        {"schema", "arena_install_plan_v1"},
        {"blockers", json::array()},
        {"offline", true},
        {"catalog_directory", root.string()},
        {"actions",
         json::array({{{"action", "install"},
                       {"component_id", "runtime.host"},
                       {"version", "2.0.0"},
                       {"dependencies", json::array()},
                       {"enabled", true},
                       {"artifact",
                        {{"type", "archive"},
                         {"source", replacement_archive.filename().string()},
                         {"sha256", sha256_path(replacement_archive)},
                         {"size_bytes", fs::file_size(replacement_archive)}}}},
                      {{"action", "refresh"},
                       {"component_id", component_id},
                       {"version", "1.0.0"},
                       {"dependencies", json::array({"runtime.host"})},
                       {"enabled", true},
                       {"artifact", {{"sha256", std::string(64, 'a')}}}}})},
    };

    if (fail_after_refresh) {
        REQUIRE_THROWS(SetupEngine::apply(json::object(), plan, paths, true, true));
        REQUIRE(json::parse(std::ifstream(configuration)) == old_configuration);
        REQUIRE_FALSE(fs::exists(generated));
        const json runtime_receipt = json::parse(std::ifstream(
            paths.data_root / "receipts" / "runtime.host.json"));
        REQUIRE(runtime_receipt.at("component_version") == "1.0.0");
        const json restored_consumer =
            json::parse(std::ifstream(consumer_receipt_path));
        REQUIRE_FALSE(restored_consumer.contains("refreshed_at"));
        REQUIRE(restored_consumer.at("startup").at("schema") ==
                "arena_provider_lifecycle_result_v1");
        REQUIRE(restored_consumer.at("startup")
                    .at("observed")
                    .at("installation") == "installed");
        REQUIRE(fs::is_directory(old_runtime_root));
        const auto transactions = fs::directory_iterator(
            paths.data_root / "transactions");
        const auto transaction = json::parse(std::ifstream(transactions->path()));
        REQUIRE(transaction.at("state") == "rolled_back");
    } else {
        const json applied =
            SetupEngine::apply(json::object(), plan, paths, true, true);
        INFO(applied.dump(2));
        REQUIRE(applied.at("state") == "committed");
        const fs::path replacement_root =
            paths.install_root / "components" / "runtime.host" / "2.0.0";
        const json refreshed = json::parse(std::ifstream(configuration));
        REQUIRE(refreshed.at("bindings").at("runtime") ==
                replacement_root.string());
        REQUIRE(fs::is_regular_file(generated));
        const json transaction = json::parse(std::ifstream(
            paths.data_root / "transactions" /
            (applied.at("transaction_id").get<std::string>() + ".json")));
        REQUIRE(transaction.at("refreshed_current_consumers") ==
                json::array({component_id}));
        REQUIRE(transaction.at("refreshed_dependency_consumers").empty());
        REQUIRE(transaction.at("startup_quiescence").at(0).at("state") ==
                "ready");
    }
    fs::remove_all(root);
}

TEST_CASE("optional provider binding replacement quiesces and refreshes consumer") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const std::string consumer_id = "optional.binding.consumer";
    const std::string binding_component_id = "optional.bound.component";
    const fs::path configuration = write_mock_renderer_consumer(
        paths, consumer_id, "lichess_transport_profile");

    const fs::path consumer_root =
        paths.install_root / "components" / consumer_id / "1.0.0";
    const fs::path consumer_manifest_path = consumer_root / "arena-component.json";
    json consumer_manifest = json::parse(std::ifstream(consumer_manifest_path));
    consumer_manifest["installer_integration"]["dependency_bindings"] =
        json::array({{{"name", "optional_runtime"},
                      {"component_id", binding_component_id},
                      {"source", "active_root"},
                      {"required", false}}});
    std::ofstream(consumer_manifest_path, std::ios::binary | std::ios::trunc)
        << consumer_manifest.dump(2);

    const fs::path consumer_receipt_path =
        paths.data_root / "receipts" / (consumer_id + ".json");
    json consumer_receipt = json::parse(std::ifstream(consumer_receipt_path));
    consumer_receipt["state"] = "installed_managed";
    consumer_receipt["startup"] = {{"installed", true}};
    REQUIRE(consumer_receipt.at("dependencies") == json::array({"runtime.host"}));
    std::ofstream(consumer_receipt_path, std::ios::binary | std::ios::trunc)
        << consumer_receipt.dump(2);

    const auto write_bound_component = [&](const fs::path& component_root,
                                           const std::string& version) {
        fs::create_directories(component_root);
        const fs::path payload = component_root / "bound.fixture";
        std::ofstream(payload, std::ios::binary) << version;
        std::ofstream(component_root / "arena-component.json", std::ios::binary)
            << json{
                   {"schema", "arena_install_component_v1"},
                   {"schema_version", 1},
                   {"component_id", binding_component_id},
                   {"component_version", version},
                   {"entrypoints", json::object()},
                   {"dependencies", json::array()},
                   {"files",
                    json::array({{{"path", "bound.fixture"},
                                  {"size_bytes", fs::file_size(payload)},
                                  {"sha256", sha256_path(payload)}}})},
               }
                   .dump(2);
    };

    const fs::path previous_bound_root =
        paths.install_root / "components" / binding_component_id / "1.0.0";
    write_bound_component(previous_bound_root, "1.0.0");
    std::ofstream(paths.data_root / "receipts" /
                      (binding_component_id + ".json"),
                  std::ios::binary)
        << json{
               {"schema", "arena_install_receipt_v1"},
               {"schema_version", 1},
               {"component_id", binding_component_id},
               {"component_version", "1.0.0"},
               {"state", "installed_managed"},
               {"active_root", previous_bound_root.string()},
               {"artifact_sha256", std::string(64, 'a')},
               {"dependencies", json::array()},
               {"data_paths", json::array()},
               {"configuration_path", ""},
               {"activated_files", json::array()},
           }
               .dump(2);

    const fs::path replacement_package = root / "optional-bound-component-2.0.0";
    write_bound_component(replacement_package, "2.0.0");
    const fs::path replacement_archive = root / "optional.bound.component-2.0.0.tar.gz";
    write_component_archive(replacement_package, replacement_archive);
    const json plan{
        {"schema", "arena_install_plan_v1"},
        {"blockers", json::array()},
        {"offline", true},
        {"catalog_directory", root.string()},
        {"actions",
         json::array({{{"action", "install"},
                       {"component_id", binding_component_id},
                       {"version", "2.0.0"},
                       {"dependencies", json::array()},
                       {"enabled", true},
                       {"artifact",
                        {{"type", "archive"},
                         {"source", replacement_archive.filename().string()},
                         {"sha256", sha256_path(replacement_archive)},
                         {"size_bytes", fs::file_size(replacement_archive)}}}}})},
    };

    const json applied =
        SetupEngine::apply(json::object(), plan, paths, true, true);
    INFO(applied.dump(2));
    REQUIRE(applied.at("state") == "committed");
    const fs::path replacement_root =
        paths.install_root / "components" / binding_component_id / "2.0.0";
    const json refreshed = json::parse(std::ifstream(configuration));
    REQUIRE(refreshed.at("bindings").at("optional_runtime") ==
            replacement_root.string());
    const json transaction = json::parse(std::ifstream(
        paths.data_root / "transactions" /
        (applied.at("transaction_id").get<std::string>() + ".json")));
    REQUIRE(transaction.at("refreshed_current_consumers").empty());
    REQUIRE(transaction.at("refreshed_dependency_consumers") ==
            json::array({consumer_id}));
    REQUIRE(transaction.at("startup_quiescence").size() == 1);
    REQUIRE(transaction.at("startup_quiescence").at(0).at("component_id") ==
            consumer_id);
    REQUIRE(transaction.at("startup_quiescence").at(0).at("state") == "ready");

    fs::remove_all(root);
}

TEST_CASE("timing profile imports derive only additive permissions from complete gates") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root);
    const json identity{
        {"engine", {{"executable_sha256", "sha256:" + std::string(64, 'a')}}},
        {"model", {{"model_fingerprint", "42"}, {"model_id", "model-1"}}},
        {"search", {{"provider_effective", "cpu"}, {"search_core", "mcgs"}}},
    };
    const json gates{
        {"legal_moves", true},
        {"nonterminal_null_moves", true},
        {"controller_disabled", true},
        {"provider_timing_captured", true},
        {"provider_service_reserve_valid", true},
        {"applied_provider_service_reserve_covered", true},
        {"applied_backup_emit_reserve_covered", true},
        {"p99_deadline_overshoot_passed", true},
        {"maximum_deadline_overshoot_passed", true},
        {"passed", true},
        {"mcgs_root_coverage_passed", true},
        {"mcgs_policy_only_fallback_passed", true},
        {"mcgs_graph_population_passed", true},
        {"mcgs_async_overlap_passed", true},
        {"mcgs_single_owner_passed", true},
        {"mcgs_low_budget_fallback_passed", true},
        {"mcgs_deterministic_fallback_passed", true},
        {"mcgs_completion_integrity_passed", true},
        {"mcgs_temporal_policy_passed", true},
        {"mcgs_deadline_tail_telemetry_passed", true},
    };
    json profile{
        {"schema", "uci_time_profile_v3"},
        {"qualified", true},
        {"gates", gates},
        {"identity", identity},
        {"identity_sha256", "sha256:" + sha256_text(identity.dump())},
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
    REQUIRE(imported.at("validation").at("permissions").at(
                "provider_safety_qualified") == true);
    REQUIRE(imported.at("validation").at("permissions").at(
                "forecast_shadow_qualified") == true);
    REQUIRE(imported.at("validation").at("permissions").at(
                "adaptive_play_qualified") == false);
    const json inventory = SetupEngine::inventory(paths);
    REQUIRE(inventory.at("discovery").at("timing_profile").at("selection").at("source") ==
            "saved_locator");

    profile["permissions"] = {{"provider_safety_qualified", false}};
    const fs::path unsafe_profile_path = root / "unsafe-profile.json";
    std::ofstream(unsafe_profile_path) << profile.dump(2);
    REQUIRE_THROWS(SetupEngine::import_path(paths, "timing_profile", unsafe_profile_path));

    profile.erase("permissions");
    profile["gates"]["mcgs_completion_integrity_passed"] = false;
    const fs::path incomplete_profile_path = root / "incomplete-profile.json";
    std::ofstream(incomplete_profile_path) << profile.dump(2);
    REQUIRE_THROWS(SetupEngine::import_path(paths, "timing_profile",
                                            incomplete_profile_path));

    profile["gates"]["mcgs_completion_integrity_passed"] = true;
    profile["permissions"] = {{"provider_safety_qualified", "yes"}};
    const fs::path malformed_profile_path = root / "malformed-profile.json";
    std::ofstream(malformed_profile_path) << profile.dump(2);
    REQUIRE_THROWS(SetupEngine::import_path(paths, "timing_profile",
                                            malformed_profile_path));

    profile.erase("permissions");
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
            "uci_arena_lichess_capacity_profile_v2",
            "uci_arena_lichess_capacity_profile_v2.json",
        },
        std::tuple{
            "lichess_transport_profile",
            "uci_arena_lichess_transport_profile_v2",
            "transport_profile_v2.json",
        },
    };
    fs::path single_source;
    fs::path single_managed;
    std::string single_component_id;
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
            single_source = profile_path;
            single_managed = imported.at("path").get<std::string>();
            single_component_id =
                imported.at("receipt").at("component_id").get<std::string>();
            REQUIRE(imported.at("path") != fs::absolute(profile_path).string());
            REQUIRE(fs::is_regular_file(single_managed));
            REQUIRE(single_managed.parent_path().filename().string().size() == 32);
            REQUIRE(imported.at("validation").at("evidence_files") == 2);
        }
    }

    const json repeated =
        SetupEngine::import_path(paths, "lichess_single_game_profile", single_source);
    REQUIRE(repeated.at("path").get<std::string>() == single_managed.string());

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
               {"schema", "uci_arena_lichess_capacity_profile_v2"},
               {"qualified", false},
           }
               .dump(2);
    REQUIRE_THROWS(
        SetupEngine::import_path(paths, "lichess_capacity_profile", unqualified));
    const fs::path legacy = root / "legacy-capacity.json";
    std::ofstream(legacy)
        << json{
               {"schema", "uci_arena_lichess_capacity_profile_v1"},
               {"qualified", true},
           }
               .dump(2);
    REQUIRE_THROWS(
        SetupEngine::import_path(paths, "lichess_capacity_profile", legacy));

    const json uninstall_plan =
        SetupEngine::plan_uninstall(paths, {single_component_id});
    REQUIRE(uninstall_plan.at("components").at(0).at("remove_active_root") ==
            false);
    const json uninstall_result =
        SetupEngine::uninstall(paths, uninstall_plan, true);
    REQUIRE(uninstall_result.at("components").at(0).at("data_preserved") ==
            true);
    REQUIRE(fs::is_regular_file(
        arena::platform::filesystem_api_path(single_managed)));

    const json purge_plan = SetupEngine::plan_purge(paths, {single_component_id});
    REQUIRE(purge_plan.at("blockers").empty());
    const json purge_result = SetupEngine::purge(paths, purge_plan, true);
    REQUIRE(purge_result.at("ok") == true);
    REQUIRE_FALSE(fs::exists(arena::platform::filesystem_api_path(single_managed)));
    fs::remove_all(arena::platform::filesystem_api_path(root));
}

TEST_CASE("Lichess profile import supports the publisher closure beyond legacy path limits") {
    const fs::path root = temporary_directory();
    const SetupPaths paths = paths_under(root / "UCI Arena Manager");
    const fs::path source_root = root / std::string(96, 'p');
    const fs::path profile = write_lichess_single_profile(source_root);
    const fs::path complete_game =
        source_root / "evidence" /
        (std::string(64, 'a') + "-" + std::string(64, 'b')) /
        "complete-game.json";
#ifdef _WIN32
    REQUIRE(complete_game.native().size() > 260);
#endif

    const json imported =
        SetupEngine::import_path(paths, "lichess_single_game_profile", profile);
    REQUIRE(imported.at("mode") == "managed_copy");
    REQUIRE(imported.at("validation").at("evidence_files") == 2);
    REQUIRE(imported.at("receipt").at("activated_files").empty());
    const fs::path managed = imported.at("path").get<std::string>();
    REQUIRE(fs::is_regular_file(arena::platform::filesystem_api_path(managed)));
    REQUIRE(SetupEngine::verify(paths).at("ok") == true);

    json conflicting;
    std::ifstream(arena::platform::filesystem_api_path(profile)) >> conflicting;
    conflicting["evidence"]["deadline"]["artifact_sha256"] =
        "sha256:" + std::string(64, '0');
    const fs::path conflicting_profile = source_root / "conflicting-profile.json";
    std::ofstream(arena::platform::filesystem_api_path(conflicting_profile))
        << conflicting.dump(2);
    REQUIRE_THROWS(SetupEngine::import_path(
        paths, "lichess_single_game_profile", conflicting_profile));
    const json inventory = SetupEngine::inventory(paths);
    REQUIRE(inventory.at("discovery")
                .at("lichess_single_game_profile")
                .at("selection")
                .at("path") == managed.string());
    REQUIRE(SetupEngine::verify(paths).at("ok") == true);

    fs::remove_all(arena::platform::filesystem_api_path(root));
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
               {"gates",
                {{"legal_moves", true},
                 {"nonterminal_null_moves", true},
                 {"controller_disabled", true},
                 {"provider_timing_captured", true},
                 {"provider_service_reserve_valid", true},
                 {"applied_provider_service_reserve_covered", true},
                 {"applied_backup_emit_reserve_covered", true},
                 {"p99_deadline_overshoot_passed", true},
                 {"maximum_deadline_overshoot_passed", true},
                 {"passed", true}}},
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
                 "uci_arena_lichess_capacity_profile_v2",
                 "uci_arena_lichess_capacity_profile_v2.json",
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
    fs::remove_all(arena::platform::filesystem_api_path(root));
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
    const fs::path locator_path = paths.data_root / "locators" / "opening_book.json";
    REQUIRE(fs::is_regular_file(locator_path));
    const json saved_locator = json::parse(std::ifstream(locator_path));
    REQUIRE(fs::path(saved_locator.at("path").get<std::string>()) == managed_root);
    REQUIRE(saved_locator.at("mode") == "managed_copy");
    REQUIRE(saved_locator.at("authority_mode") == "immutable_pinned_snapshot");
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
    bool valid_evidence = true;
    SECTION("ready uninstall evidence") {}
    SECTION("non-ready uninstall evidence") { valid_evidence = false; }
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
    hooks.uninstall_startup = [&startup_invoked, valid_evidence](
                                  const json& receipt) {
        startup_invoked = true;
        json result = ready_uninstall_lifecycle(
            receipt.at("component_id").get<std::string>(),
            receipt.at("component_version").get<std::string>());
        if (!valid_evidence) {
            result["observed"]["installation"] = "installed";
        }
        return result;
    };
    const json plan =
        RemovalLifecycle::plan_uninstall(paths, {component_id}, false, hooks);
    REQUIRE(plan.at("components").at(0).at("startup_cleanup_required") == true);
    if (!valid_evidence) {
        REQUIRE_THROWS(RemovalLifecycle::uninstall(paths, plan, true, hooks));
        REQUIRE(startup_invoked);
        REQUIRE(fs::is_directory(active_root));
        REQUIRE(fs::is_regular_file(receipts / (component_id + ".json")));
        fs::remove_all(root);
        return;
    }
    const json result = RemovalLifecycle::uninstall(paths, plan, true, hooks);
    REQUIRE(startup_invoked);
    REQUIRE(result.at("components")
                .at(0)
                .at("lifecycle_result")
                .at("operation") == "uninstall");
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
