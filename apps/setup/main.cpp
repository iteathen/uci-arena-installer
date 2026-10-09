#include <curl/curl.h>

#include <CLI/CLI.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "arena/application/installer/setup.hpp"
#include "arena/ports/asset_search_index.hpp"
#include "arena/platform/service_control.hpp"
#ifdef _WIN32
#include "arena/platform/windows_install_scope.hpp"
#endif

namespace {

namespace fs = std::filesystem;
using arena::application::installer::SetupEngine;
using arena::application::installer::SetupPaths;
using nlohmann::json;

const char* manager_service_name() {
#ifdef _WIN32
    return "UCIArenaManager";
#else
    return "uci-arena-manager.service";
#endif
}

bool manager_service_running() {
    const int status = arena::platform::perform_service_action(
        manager_service_name(), "status");
#ifdef _WIN32
    return status == 4;  // SERVICE_RUNNING
#else
    return status == 0;
#endif
}

fs::path environment_path(const char* name, const fs::path& fallback) {
    if (const char* value = std::getenv(name); value != nullptr && *value != '\0') {
        return value;
    }
    return fallback;
}

SetupPaths default_paths() {
#ifdef _WIN32
    if (arena::platform::is_windows_per_user_install()) {
        const fs::path local_app_data = environment_path(
            "LOCALAPPDATA",
            environment_path("USERPROFILE", "C:\\Users\\Default") /
                "AppData" / "Local");
        const fs::path user_profile = environment_path(
            "USERPROFILE", local_app_data.parent_path().parent_path());
        const fs::path root = local_app_data / "UCI Arena Manager";
        return {
            root / "installed",
            root,
            root / "cache",
            user_profile / "UCI Arena Workspaces",
            root / "knowledge",
        };
    }
    const fs::path program_files = environment_path("ProgramFiles", "C:\\Program Files");
    const fs::path program_data = environment_path("ProgramData", "C:\\ProgramData");
    const fs::path user_profile = environment_path("USERPROFILE", program_data);
    return {
        program_files / "UCI Arena Manager",
        program_data / "UCI Arena Manager",
        program_data / "UCI Arena Manager" / "cache",
        user_profile / "UCI Arena Workspaces",
        program_data / "UCI Arena Manager" / "knowledge",
    };
#else
    const fs::path state_home =
        environment_path("XDG_STATE_HOME", environment_path("HOME", "/tmp") / ".local/state");
    return {
        "/opt/uci-arena",
        "/var/lib/uci-arena",
        "/var/cache/uci-arena",
        state_home / "uci-arena/workspaces",
        "/var/lib/uci-arena/knowledge",
    };
#endif
}

void print(
    const json& value,
    const bool machine,
    const fs::path& result_file = {}) {
    const std::string rendered = value.dump(machine ? -1 : 2) + '\n';
    std::cout << rendered;
    if (!result_file.empty()) {
        std::ofstream output(result_file, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("cannot write the requested setup result file");
        }
        output << rendered;
        output.flush();
        if (!output) {
            throw std::runtime_error("cannot commit the requested setup result file");
        }
    }
}

json load_json_document(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot read lifecycle plan: " + path.string());
    }
    try {
        return json::parse(input);
    } catch (const json::exception& error) {
        throw std::runtime_error("invalid lifecycle plan JSON: " +
                                 std::string(error.what()));
    }
}

} // namespace

int main(int argc, char** argv) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    fs::path result_file;
    try {
        CLI::App app{"Trusted cross-platform installer for UCI Arena Manager and provider "
                     "components"};
        app.require_subcommand(1);

        SetupPaths paths = default_paths();
#ifdef _WIN32
        std::string install_scope =
            arena::platform::is_windows_per_user_install()
                ? "per-user"
                : "per-machine";
#endif
        fs::path catalog_path{"release/catalog.json"};
        fs::path metadata_directory;
        bool allow_unsigned_development_catalog = false;
        bool json_output = false;
        auto* catalog_option = app.add_option("--catalog", catalog_path, "Installation catalog");
        auto* metadata_option =
            app.add_option("--metadata-dir", metadata_directory, "Trusted TUF metadata directory");
        app.add_flag("--allow-unsigned-development-catalog", allow_unsigned_development_catalog,
                     "Permit an unsigned local catalog; forbidden for official releases");
        app.add_option("--install-root", paths.install_root);
        app.add_option("--data-root", paths.data_root);
        app.add_option("--cache-root", paths.cache_root);
        app.add_option("--workspace-root", paths.workspace_root);
        app.add_option("--knowledge-root", paths.knowledge_root);
#ifdef _WIN32
        app.add_option(
               "--install-scope",
               install_scope,
               "Fixed owning package scope: per-user or per-machine")
            ->check(CLI::IsMember({"per-user", "per-machine"}));
#endif
        app.add_option("--bundle-owner", paths.bundle_owner,
                       "Immutable calling bundle provider key");
        app.add_flag("--restart-manager-service", paths.restart_manager_service,
                     "Restart the installed Manager service after component activation");
        app.add_flag("--json", json_output, "Emit compact JSON");
        app.add_option(
            "--result-file",
            result_file,
            "Write the final machine-readable result to this helper-owned path");

        std::vector<fs::path> scan_roots;
        auto* inventory = app.add_subcommand("inventory", "Detect managed and local components");
        inventory->add_option("--scan-root", scan_roots,
                              "Additional bounded root to inspect; may be repeated");

        std::string import_kind;
        fs::path import_source;
        bool managed_copy = false;
        bool clear_locator = false;
        std::string authority_mode;
        auto* import_command =
            app.add_subcommand("import", "Save or explicitly clear a local component locator");
        import_command
            ->add_option("--kind", import_kind,
                         "opening_book, syzygy, uci_engine, book_forge, evidence_service, "
                         "timing_profile, lichess_single_game_profile, "
                         "lichess_capacity_profile, lichess_transport_profile, or "
                         "lichess_token_path")
            ->required()
            ->check(CLI::IsMember({"opening_book", "syzygy", "uci_engine", "book_forge",
                                   "evidence_service", "timing_profile",
                                   "lichess_single_game_profile", "lichess_capacity_profile",
                                   "lichess_transport_profile", "lichess_token_path",
                                   "restaurant"}));
        auto* import_action = import_command->add_option_group("Locator action");
        import_action->require_option(1, 1);
        auto* import_path_option = import_action->add_option("--path", import_source);
        auto* import_clear_option = import_action->add_flag(
            "--clear", clear_locator, "Revoke only this saved locator and rerender its consumers");
        import_clear_option->excludes(import_path_option);
        auto* managed_copy_option = import_command->add_flag("--managed-copy", managed_copy,
                                 "Copy a small opening book into Manager-owned knowledge storage");
        auto* authority_mode_option = import_command
            ->add_option("--authority-mode", authority_mode,
                         "Opening-book authority: service_managed_live_channel or "
                         "immutable_pinned_snapshot")
            ->check(CLI::IsMember({"service_managed_live_channel",
                                   "immutable_pinned_snapshot"}));
        import_clear_option->excludes(managed_copy_option);
        import_clear_option->excludes(authority_mode_option);

        std::string preset{"complete_arena"};
        std::vector<std::string> components;
        bool offline = false;
        bool download_missing_knowledge = false;
        bool download_missing_books = false;
        bool download_missing_tablebases = false;
        std::vector<std::string> bundle_features;
        auto configure_plan = [&](CLI::App* command) {
            command->add_option("--preset", preset, "complete_arena, manager_only, or custom")
                ->check(CLI::IsMember({"complete_arena", "manager_only", "custom"}));
            command->add_option("--component", components,
                                "Component id for the custom preset; may be repeated");
            command->add_flag("--offline", offline, "Forbid network acquisition");
            command->add_flag("--download-missing-knowledge", download_missing_knowledge,
                              "Add the signed opening-book and Syzygy WDL components");
            command->add_flag("--download-missing-books", download_missing_books,
                              "Add the signed standard opening-book component");
            command->add_flag("--download-missing-tablebases", download_missing_tablebases,
                              "Add the signed bounded Syzygy WDL component");
            command->add_option(
                "--bundle-feature", bundle_features,
                "Installer feature selection as component-id=0 or component-id=1; "
                "may be repeated");
        };

        auto* plan_command = app.add_subcommand("plan", "Resolve dependencies and capacity");
        configure_plan(plan_command);
        bool yes = false;
        auto* apply_command = app.add_subcommand("apply", "Apply a verified installation plan");
        configure_plan(apply_command);
        apply_command->add_flag("--yes", yes, "Confirm the reviewed plan");
        auto* repair_command =
            app.add_subcommand("repair", "Reinstall and verify selected component versions");
        configure_plan(repair_command);
        repair_command->add_flag("--yes", yes, "Confirm the reviewed repair plan");
        auto* upgrade_command =
            app.add_subcommand("upgrade", "Install the catalog-selected component versions");
        configure_plan(upgrade_command);
        upgrade_command->add_flag("--yes", yes, "Confirm the reviewed upgrade plan");

        auto* verify_command =
            app.add_subcommand("verify", "Verify all managed receipts and files");

        std::vector<std::string> uninstall_components;
        bool uninstall_all = false;
        auto configure_lifecycle_selection =
            [](CLI::App* command, std::vector<std::string>& selected, bool& all) {
                auto* selected_option =
                    command->add_option("--component", selected,
                                        "Managed component id; may be repeated")
                        ->expected(-1);
                auto* all_option = command->add_flag(
                    "--all", all, "Select every owned component with lifecycle evidence");
                selected_option->excludes(all_option);
                all_option->excludes(selected_option);
            };
        auto* uninstall_plan_command = app.add_subcommand(
            "uninstall-plan", "Preview removal of installed software authority");
        configure_lifecycle_selection(
            uninstall_plan_command, uninstall_components, uninstall_all);
        auto* uninstall_command =
            app.add_subcommand("uninstall", "Remove installed software authority; preserve data");
        configure_lifecycle_selection(uninstall_command, uninstall_components, uninstall_all);
        uninstall_command->add_flag("--yes", yes, "Confirm the generated uninstall plan");

        std::vector<std::string> purge_components;
        bool purge_all = false;
        auto* purge_plan_command = app.add_subcommand(
            "purge-plan", "Preview bounded deletion of preserved component data");
        configure_lifecycle_selection(purge_plan_command, purge_components, purge_all);
        fs::path purge_plan_file;
        auto* purge_command = app.add_subcommand(
            "purge", "Apply a separately reviewed ownership-aware purge plan");
        purge_command->add_option("--plan-file", purge_plan_file,
                                  "Previously reviewed purge-plan JSON")
            ->required();
        purge_command->add_flag("--yes", yes, "Confirm destructive purge");

        CLI11_PARSE(app, argc, argv);

#ifdef _WIN32
        paths.manager_service_access = install_scope == "per-machine";
        if (!paths.manager_service_access && paths.restart_manager_service) {
            throw std::runtime_error(
                "per-user setup cannot restart or impersonate the machine Manager service");
        }
#endif

        if (*inventory) {
            auto search_index = arena::ports::make_platform_asset_search_index();
            print(SetupEngine::inventory(paths, scan_roots, search_index.get()),
                  json_output, result_file);
        } else if (*import_command) {
            print(SetupEngine::import_path(paths, import_kind, import_source, managed_copy,
                                           authority_mode, clear_locator),
                  json_output, result_file);
        } else if (*verify_command) {
            const auto result = SetupEngine::verify(paths);
            print(result, json_output, result_file);
            curl_global_cleanup();
            return result.value("ok", false) ? 0 : 4;
        } else if (*uninstall_plan_command) {
            if (!uninstall_all && uninstall_components.empty()) {
                throw std::runtime_error("uninstall-plan requires --component or --all");
            }
            const json lifecycle_plan = SetupEngine::plan_uninstall(
                paths, uninstall_components, uninstall_all);
            print(lifecycle_plan, json_output, result_file);
            curl_global_cleanup();
            return lifecycle_plan.at("blockers").empty() ? 0 : 3;
        } else if (*uninstall_command) {
            if (!uninstall_all && uninstall_components.empty()) {
                throw std::runtime_error("uninstall requires --component or --all");
            }
            const json lifecycle_plan = SetupEngine::plan_uninstall(
                paths, uninstall_components, uninstall_all);
            if (!yes) {
                print(lifecycle_plan, json_output, result_file);
                curl_global_cleanup();
                return 15;
            }
            print(SetupEngine::uninstall(paths, lifecycle_plan, yes),
                  json_output, result_file);
        } else if (*purge_plan_command) {
            if (!purge_all && purge_components.empty()) {
                throw std::runtime_error("purge-plan requires --component or --all");
            }
            const json lifecycle_plan = SetupEngine::plan_purge(
                paths, purge_components, purge_all);
            print(lifecycle_plan, json_output, result_file);
            curl_global_cleanup();
            return lifecycle_plan.at("blockers").empty() ? 0 : 3;
        } else if (*purge_command) {
            const json lifecycle_plan = load_json_document(purge_plan_file);
            if (!yes) {
                print(lifecycle_plan, json_output, result_file);
                curl_global_cleanup();
                return 15;
            }
            const json purge_result =
                SetupEngine::purge(paths, lifecycle_plan, yes);
            print(purge_result, json_output, result_file);
            curl_global_cleanup();
            return purge_result.value("ok", false) ? 0 : 11;
        } else {
            if (catalog_option->count() == 0) {
                const fs::path retained = paths.data_root / "release" / "catalog.json";
                if (fs::is_regular_file(retained)) {
                    catalog_path = retained;
                }
            }
            if (metadata_option->count() == 0) {
                const fs::path retained = paths.data_root / "release" / "tuf";
                if (fs::is_directory(retained)) {
                    metadata_directory = retained;
                }
            }
            const json catalog = SetupEngine::load_catalog(
                catalog_path, metadata_directory, allow_unsigned_development_catalog,
                paths.data_root / "trust" / "tuf-versions.json",
                paths.data_root / "release" /
                    "development-trust.json");
            json feature_reconciliation;
            if (!bundle_features.empty()) {
                feature_reconciliation =
                    SetupEngine::resolve_feature_selection(catalog, bundle_features);
                preset = "custom";
                components = feature_reconciliation.at("selected_roots")
                                 .get<std::vector<std::string>>();
            }
            const json resolved = SetupEngine::plan(
                catalog, paths, preset, components, offline, download_missing_knowledge,
                download_missing_books, download_missing_tablebases,
                repair_command->parsed(), upgrade_command->parsed());
            if (*plan_command) {
                print(resolved, json_output, result_file);
                curl_global_cleanup();
                return resolved.at("blockers").empty() ? 0 : 3;
            }
            static_cast<void>(SetupEngine::validate_release_media(
                catalog,
                catalog_path,
                metadata_directory,
                catalog.value("_catalog_trust", "") != "tuf"));
            const bool feature_reconcile = !bundle_features.empty();
            const bool restart_manager = paths.restart_manager_service;
            bool manager_was_running = false;
            if (feature_reconcile && restart_manager) {
                manager_was_running = manager_service_running();
                if (manager_was_running &&
                    arena::platform::perform_service_action(
                        manager_service_name(), "stop") != 0) {
                    throw std::runtime_error(
                        "Manager service could not be stopped for component transition");
                }
                paths.restart_manager_service = false;
            }
            json result;
            try {
                result = SetupEngine::apply(catalog, resolved, paths, yes,
                                            *repair_command || *upgrade_command);
                if (feature_reconcile && result.value("state", "") == "committed") {
                    std::vector<std::string> remove;
                    for (const auto& value :
                         feature_reconciliation.at("removal_candidates")) {
                        const std::string id = value.get<std::string>();
                        if (fs::is_regular_file(paths.data_root / "receipts" /
                                                (id + ".json"))) {
                            remove.push_back(id);
                        }
                    }
                    if (!remove.empty()) {
                        const json uninstall_plan =
                            SetupEngine::plan_uninstall(paths, remove, false);
                        result["removed_components"] =
                            SetupEngine::uninstall(paths, uninstall_plan, true);
                    } else {
                        result["removed_components"] = json::array();
                    }
                }
            } catch (...) {
                if (feature_reconcile && restart_manager &&
                    manager_was_running) {
                    static_cast<void>(arena::platform::perform_service_action(
                        manager_service_name(), "start"));
                }
                throw;
            }
            if (feature_reconcile && restart_manager &&
                manager_was_running &&
                arena::platform::perform_service_action(
                    manager_service_name(), "start") != 0) {
                throw std::runtime_error(
                    "Manager service prior running state could not be restored "
                    "after component transition");
            }
            if (feature_reconcile && restart_manager) {
                result["manager_service_state_preserved"] =
                    manager_was_running ? "running" : "stopped";
            }
            if (result.value("state", "") == "committed") {
                result["retained_release_media"] = SetupEngine::persist_release_media(
                    catalog,
                    paths,
                    catalog_path,
                    metadata_directory,
                    catalog.value("_catalog_trust", "") != "tuf");
            }
            print(result, json_output, result_file);
        }
        curl_global_cleanup();
        return 0;
    } catch (const CLI::ParseError& error) {
        curl_global_cleanup();
        return error.get_exit_code();
    } catch (const std::exception& error) {
        if (!result_file.empty()) {
            try {
                print(
                    {
                        {"schema", "arena_setup_problem_v1"},
                        {"code", "setup_failed"},
                        {"detail", error.what()},
                    },
                    true,
                    result_file);
            } catch (...) {
            }
        }
        std::cerr << "arena-setup: " << error.what() << '\n';
        curl_global_cleanup();
        return 2;
    }
}
