#include "arena/application/installer/install_layout.hpp"

#include "arena/application/installer/setup.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#ifdef _WIN32
#include "arena/platform/windows_package_layout.hpp"
#include <windows.h>
#include <objbase.h>
#endif

namespace {

namespace fs = std::filesystem;
namespace installer = arena::application::installer;
namespace ports = arena::ports;
using json = nlohmann::json;

constexpr std::string_view policy_digest =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr std::string_view descriptor_digest =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
constexpr std::string_view installation_id = "123e4567-e89b-12d3-a456-426614174000";

[[nodiscard]] std::string path_text(const fs::path& value) {
    const auto encoded = value.u8string();
    return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

[[nodiscard]] json active_policy() {
    const auto path = fs::path{ARENA_INSTALLER_SOURCE_DIR} /
                      "packaging" / "install-layout.json";
    std::ifstream input{path};
    if (!input) throw std::runtime_error("could not open install-layout policy fixture");
    json result;
    input >> result;
    return result;
}

class FakeAnchors final : public ports::InstallLayoutAnchorResolver {
  public:
    fs::path user{"C:\\Users\\Example\\AppData\\Local"};
    fs::path program_files{"C:\\Program Files"};
    fs::path program_data{"C:\\ProgramData"};
    std::optional<ports::LayoutAnchor> failing;
    mutable std::vector<ports::LayoutAnchor> calls;

    [[nodiscard]] fs::path resolve(const ports::LayoutAnchor anchor) const override {
        calls.push_back(anchor);
        if (failing == anchor) throw std::runtime_error("secret-anchor-canary");
        switch (anchor) {
        case ports::LayoutAnchor::user_local_app_data:
            return user;
        case ports::LayoutAnchor::machine_program_files:
            return program_files;
        case ports::LayoutAnchor::machine_program_data:
            return program_data;
        }
        throw std::runtime_error("unknown fixture anchor");
    }
};

[[nodiscard]] std::string scope_text(const ports::InstallScope scope) {
    return scope == ports::InstallScope::per_user ? "per-user" : "per-machine";
}

[[nodiscard]] json descriptor_for(const installer::ResolvedInstallLayout& layout,
                                  const std::string_view digest = policy_digest) {
    const auto& roots = layout.roots;
    const auto& stable = layout.stable_paths;
    return {
        {"schema", "arena_installed_layout_v1"},
        {"schema_version", 1},
        {"installation_id", installation_id},
        {"layout_generation", 7},
        {"platform", "windows-x86_64"},
        {"scope", scope_text(layout.scope)},
        {"policy",
         {
             {"policy_id", layout.policy_id},
             {"policy_version", layout.policy_version},
             {"path", path_text(stable.policy)},
             {"sha256", digest},
         }},
        {"integration_root", path_text(roots.integration)},
        {"roots",
         {
             {"data", path_text(roots.data)},
             {"install", path_text(roots.install)},
             {"cache", path_text(roots.cache)},
             {"workspace", path_text(roots.workspace)},
             {"knowledge", path_text(roots.knowledge)},
             {"release", path_text(roots.release)},
             {"receipts", path_text(roots.receipts)},
             {"transactions", path_text(roots.transactions)},
             {"preservation", path_text(roots.preservation)},
         }},
        {"stable_paths",
         {
             {"descriptor", path_text(stable.descriptor)},
             {"scope_marker", path_text(stable.scope_marker)},
             {"setup_launcher", path_text(stable.setup_launcher)},
             {"installer_runtime", path_text(stable.installer_runtime)},
             {"path_directory", path_text(stable.path_directory)},
         }},
    };
}

[[nodiscard]] std::string upper_ascii(std::string value) {
    for (char& byte : value) {
        if (byte >= 'a' && byte <= 'z') byte = static_cast<char>(byte - ('a' - 'A'));
    }
    return value;
}

template <typename Mutator>
void check_invalid_policy(const Mutator& mutate) {
    auto candidate = active_policy();
    mutate(candidate);
    CHECK_THROWS_AS(installer::validate_install_layout_policy(candidate),
                    std::invalid_argument);
}

template <typename Mutator>
void check_invalid_descriptor(const installer::ResolvedInstallLayout& resolved,
                              FakeAnchors& anchors, const Mutator& mutate) {
    auto candidate = descriptor_for(resolved);
    mutate(candidate);
    CHECK_THROWS_AS(installer::validate_installed_layout(
                        active_policy(), policy_digest, candidate, anchors),
                    std::invalid_argument);
}

TEST_CASE("install layout resolves the exact Node golden paths") {
    const auto policy = active_policy();
    FakeAnchors personal_anchors;
    const auto personal = installer::resolve_install_layout(
        policy, ports::InstallScope::per_user, personal_anchors);
    CHECK(personal_anchors.calls ==
          std::vector{ports::LayoutAnchor::user_local_app_data});
    CHECK(path_text(personal.roots.integration) ==
          "C:\\Users\\Example\\AppData\\Local\\Programs\\UCI Arena Manager");
    CHECK(path_text(personal.roots.data) ==
          "C:\\Users\\Example\\AppData\\Local\\UCI Arena Manager");
    CHECK(path_text(personal.roots.install) ==
          "C:\\Users\\Example\\AppData\\Local\\Programs\\UCI Arena Manager\\installed");
    CHECK(path_text(personal.roots.cache) ==
          "C:\\Users\\Example\\AppData\\Local\\UCI Arena Manager\\cache");
    CHECK(path_text(personal.roots.workspace) ==
          "C:\\Users\\Example\\AppData\\Local\\UCI Arena Manager\\workspaces");
    CHECK(path_text(personal.roots.knowledge) ==
          "C:\\Users\\Example\\AppData\\Local\\UCI Arena Manager\\knowledge");
    CHECK(path_text(personal.roots.release) ==
          "C:\\Users\\Example\\AppData\\Local\\UCI Arena Manager\\release");
    CHECK(path_text(personal.roots.receipts) ==
          "C:\\Users\\Example\\AppData\\Local\\UCI Arena Manager\\receipts");
    CHECK(path_text(personal.roots.transactions) ==
          "C:\\Users\\Example\\AppData\\Local\\UCI Arena Manager\\transactions");
    CHECK(path_text(personal.roots.preservation) ==
          "C:\\Users\\Example\\AppData\\Local\\UCI Arena Manager\\preservation");
    CHECK(path_text(personal.stable_paths.policy) ==
          "C:\\Users\\Example\\AppData\\Local\\Programs\\UCI Arena Manager\\share\\uci-arena\\layout-policies\\arena_windows_standard-v2.json");
    CHECK(path_text(personal.stable_paths.descriptor) ==
          "C:\\Users\\Example\\AppData\\Local\\Programs\\UCI Arena Manager\\share\\uci-arena\\installed-layout.json");
    CHECK(path_text(personal.stable_paths.scope_marker) ==
          "C:\\Users\\Example\\AppData\\Local\\Programs\\UCI Arena Manager\\share\\uci-arena\\install-scope.txt");
    CHECK(path_text(personal.stable_paths.setup_launcher) ==
          "C:\\Users\\Example\\AppData\\Local\\Programs\\UCI Arena Manager\\bin\\arena-setup.exe");
    CHECK(path_text(personal.stable_paths.installer_runtime) ==
          "C:\\Users\\Example\\AppData\\Local\\Programs\\UCI Arena Manager\\libexec\\uci-arena-installer");
    CHECK(path_text(personal.stable_paths.path_directory) ==
          "C:\\Users\\Example\\AppData\\Local\\Programs\\UCI Arena Manager\\bin");

    FakeAnchors machine_anchors;
    const auto machine = installer::resolve_install_layout(
        policy, ports::InstallScope::per_machine, machine_anchors);
    CHECK(machine_anchors.calls ==
          std::vector{ports::LayoutAnchor::machine_program_files,
                      ports::LayoutAnchor::machine_program_data});
    CHECK(path_text(machine.roots.integration) ==
          "C:\\Program Files\\UCI Arena Manager");
    CHECK(path_text(machine.roots.data) == "C:\\ProgramData\\UCI Arena Manager");
    CHECK(path_text(machine.roots.install) ==
          "C:\\Program Files\\UCI Arena Manager\\installed");
    CHECK(path_text(machine.stable_paths.policy) ==
          "C:\\Program Files\\UCI Arena Manager\\share\\uci-arena\\layout-policies\\arena_windows_standard-v2.json");
    CHECK(path_text(machine.stable_paths.setup_launcher) ==
          "C:\\Program Files\\UCI Arena Manager\\bin\\arena-setup.exe");
}

TEST_CASE("policy validation returns an independent exact clone") {
    auto policy = active_policy();
    const auto before = policy;
    auto validated = installer::validate_install_layout_policy(policy);
    CHECK(policy == before);
    validated["policy_id"] = "changed";
    CHECK(policy == before);
}

TEST_CASE("policy validation rejects structural and semantic invalid classes") {
    const std::vector<std::function<void(json&)>> mutations{
        [](json& p) { p["extra"] = true; },
        [](json& p) { p.erase("platform"); },
        [](json& p) { p["schema"] = "wrong"; },
        [](json& p) { p["schema_version"] = 1.0; },
        [](json& p) { p["policy_id"] = "bad id"; },
        [](json& p) { p["policy_version"] = 0; },
        [](json& p) { p["platform"] = "linux-x86_64"; },
        [](json& p) { p["registration"]["extra"] = true; },
        [](json& p) { p["registration"]["subkey"] = "Software\\Other"; },
        [](json& p) { p["registration"]["values"].erase("scope"); },
        [](json& p) {
            p["registration"]["values"]["scope"] =
                p["registration"]["values"]["schema"].get<std::string>();
        },
        [](json& p) { p["stable_paths"]["extra"] = "bin/x"; },
        [](json& p) { p["stable_paths"]["setup_launcher"] = "bin\\x"; },
        [](json& p) { p["stable_paths"]["setup_launcher"] = "bin/a b"; },
        [](json& p) { p["stable_paths"]["setup_launcher"] = "bin/../x"; },
        [](json& p) { p["stable_paths"]["setup_launcher"] = "NUL/x"; },
        [](json& p) { p["stable_paths"]["descriptor"] = "BIN"; },
        [](json& p) {
            p["stable_paths"]["policy"] =
                "share/uci-arena/layout-policies/wrong-v2.json";
        },
        [](json& p) { p["scopes"]["extra"] = json::object(); },
        [](json& p) { p["scopes"]["per-user"]["privilege"] = "machine"; },
        [](json& p) {
            p["scopes"]["per-user"]["roots"]["integration"]["anchor"] =
                "machine_program_files";
        },
        [](json& p) {
            p["scopes"]["per-user"]["roots"]["cache"]["relative_to"] = "cache";
        },
        [](json& p) {
            p["scopes"]["per-user"]["roots"]["cache"]["segments"] = "cache";
        },
        [](json& p) {
            p["scopes"]["per-user"]["roots"]["install"]["relative_to"] = "data";
        },
        [](json& p) {
            p["scopes"]["per-user"]["roots"]["cache"]["segments"] = {"WORKSPACES"};
        },
    };
    for (const auto& mutate : mutations) check_invalid_policy(mutate);

    for (const auto* invalid : {"", ".", "..", "a/b", "a\\b", "a:", "x.",
                                "x ", "CON", "com1.txt", "bad\x01"}) {
        check_invalid_policy([&](json& p) {
            p["scopes"]["per-user"]["roots"]["cache"]["segments"] = {invalid};
        });
    }
}

TEST_CASE("Windows paths are parsed independently of the build host") {
    const auto policy = active_policy();
    FakeAnchors unc;
    unc.user = "\\\\server\\share\\Profiles\\Example\\AppData\\Local";
    const auto resolved = installer::resolve_install_layout(
        policy, ports::InstallScope::per_user, unc);
    CHECK(path_text(resolved.roots.integration) ==
          "\\\\server\\share\\Profiles\\Example\\AppData\\Local\\Programs\\UCI Arena Manager");

    for (const auto* invalid : {
             "relative", "C:relative", "\\rooted", "C:/forward", "C:\\",
             "C:\\x\\..\\y", "C:\\x\\.\\y", "C:\\x\\\\y",
             "C:\\x.", "C:\\NUL", "\\\\?\\C:\\x", "\\\\.\\C:\\x",
             "\\\\server\\share"}) {
        FakeAnchors anchors;
        anchors.user = invalid;
        CHECK_THROWS_AS(installer::resolve_install_layout(
                            policy, ports::InstallScope::per_user, anchors),
                        std::invalid_argument);
    }

    auto overlapping = policy;
    overlapping["scopes"]["per-user"]["roots"]["data"]["segments"] =
        {"Programs", "UCI Arena Manager", "data"};
    FakeAnchors anchors;
    CHECK_THROWS_AS(installer::resolve_install_layout(
                        overlapping, ports::InstallScope::per_user, anchors),
                    std::invalid_argument);
}

TEST_CASE("anchor resolution is scope-minimal and never falls back") {
    FakeAnchors anchors;
    anchors.failing = ports::LayoutAnchor::user_local_app_data;
    try {
        (void)installer::resolve_install_layout(
            active_policy(), ports::InstallScope::per_user, anchors);
        FAIL("expected resolution failure");
    } catch (const std::invalid_argument& error) {
        CHECK(anchors.calls == std::vector{ports::LayoutAnchor::user_local_app_data});
        CHECK(std::string_view{error.what()}.find("secret-anchor-canary") ==
              std::string_view::npos);
        CHECK(std::string_view{error.what()}.size() <= 512);
    }
}

TEST_CASE("installed descriptors validate both scopes and preserve canonical spelling") {
    for (const auto scope : {ports::InstallScope::per_user,
                             ports::InstallScope::per_machine}) {
        FakeAnchors anchors;
        const auto resolved = installer::resolve_install_layout(active_policy(), scope, anchors);
        auto descriptor = descriptor_for(resolved);
        descriptor["integration_root"] =
            upper_ascii(descriptor["integration_root"].get<std::string>());
        descriptor["roots"]["cache"] =
            upper_ascii(descriptor["roots"]["cache"].get<std::string>());
        const auto installed = installer::validate_installed_layout(
            active_policy(), policy_digest, descriptor, anchors);
        CHECK(installed.installation_id == installation_id);
        CHECK(installed.layout_generation == 7);
        CHECK(installed.policy_sha256 == policy_digest);
        CHECK(installed.resolved == resolved);
        CHECK(path_text(installed.resolved.roots.integration) ==
              path_text(resolved.roots.integration));
    }
}

TEST_CASE("installed descriptor rejects malformed identity policy and path evidence") {
    FakeAnchors anchors;
    const auto resolved = installer::resolve_install_layout(
        active_policy(), ports::InstallScope::per_user, anchors);
    const std::vector<std::function<void(json&)>> mutations{
        [](json& d) { d["extra"] = true; },
        [](json& d) { d.erase("platform"); },
        [](json& d) { d["schema"] = "wrong"; },
        [](json& d) { d["schema_version"] = 2; },
        [](json& d) { d["installation_id"] = "not-a-uuid"; },
        [](json& d) { d["installation_id"] = "123E4567-e89b-12d3-a456-426614174000"; },
        [](json& d) { d["layout_generation"] = 0; },
        [](json& d) { d["platform"] = "linux-x86_64"; },
        [](json& d) { d["scope"] = "auto"; },
        [](json& d) { d["policy"]["extra"] = true; },
        [](json& d) { d["policy"]["policy_id"] = "other"; },
        [](json& d) { d["policy"]["policy_version"] = 1; },
        [](json& d) { d["policy"]["path"] = "C:\\Other\\policy.json"; },
        [](json& d) { d["policy"]["sha256"] = std::string(64, 'c'); },
        [](json& d) { d["integration_root"] = "C:/forward"; },
        [](json& d) { d["roots"]["extra"] = "C:\\Extra\\x"; },
        [](json& d) { d["roots"].erase("data"); },
        [](json& d) { d["roots"]["cache"] = d["roots"]["workspace"]; },
        [](json& d) { d["roots"]["knowledge"] = "\\\\?\\C:\\forbidden"; },
        [](json& d) { d["stable_paths"]["extra"] = "C:\\Extra\\x"; },
        [](json& d) { d["stable_paths"].erase("descriptor"); },
        [](json& d) { d["stable_paths"]["descriptor"] = d["stable_paths"]["scope_marker"]; },
    };
    for (const auto& mutate : mutations) {
        check_invalid_descriptor(resolved, anchors, mutate);
    }
    CHECK_THROWS_AS(installer::validate_installed_layout(
                        active_policy(), "ABC", descriptor_for(resolved), anchors),
                    std::invalid_argument);
}

TEST_CASE("an older exact installed policy is not redirected by current defaults") {
    auto prior_policy = active_policy();
    prior_policy["policy_id"] = "arena_windows_previous";
    prior_policy["policy_version"] = 1;
    prior_policy["stable_paths"]["policy"] =
        "share/uci-arena/layout-policies/arena_windows_previous-v1.json";
    FakeAnchors anchors;
    const auto prior_resolved = installer::resolve_install_layout(
        prior_policy, ports::InstallScope::per_user, anchors);
    const auto descriptor = descriptor_for(prior_resolved);
    const auto installed = installer::validate_installed_layout(
        prior_policy, policy_digest, descriptor, anchors);
    CHECK(installed.resolved.policy_id == "arena_windows_previous");
    CHECK(installed.resolved.policy_version == 1);
    CHECK_THROWS_AS(installer::validate_installed_layout(
                        active_policy(), policy_digest, descriptor, anchors),
                    std::invalid_argument);
}

TEST_CASE("locator and SetupPaths are exact projections of installed authority") {
    FakeAnchors anchors;
    const auto resolved = installer::resolve_install_layout(
        active_policy(), ports::InstallScope::per_machine, anchors);
    const auto installed = installer::validate_installed_layout(
        active_policy(), policy_digest, descriptor_for(resolved), anchors);
    const auto locator =
        installer::locator_from_installed_layout(installed, descriptor_digest);
    CHECK(locator.schema == "arena_install_layout_locator_v1");
    CHECK(locator.scope == ports::InstallScope::per_machine);
    CHECK(locator.installation_id == installation_id);
    CHECK(locator.layout_generation == 7);
    CHECK(locator.integration_root == resolved.roots.integration);
    CHECK(locator.policy_path == resolved.stable_paths.policy);
    CHECK(locator.policy_sha256 == policy_digest);
    CHECK(locator.descriptor_path == resolved.stable_paths.descriptor);
    CHECK(locator.descriptor_sha256 == descriptor_digest);
    CHECK(locator.setup_launcher_path == resolved.stable_paths.setup_launcher);
    CHECK_THROWS_AS(installer::locator_from_installed_layout(installed, "ABC"),
                    std::invalid_argument);

    const auto paths = installer::setup_paths_from_layout(
        installed, "fixture-owner", true, false);
    CHECK(paths.install_root == resolved.roots.install);
    CHECK(paths.data_root == resolved.roots.data);
    CHECK(paths.cache_root == resolved.roots.cache);
    CHECK(paths.workspace_root == resolved.roots.workspace);
    CHECK(paths.knowledge_root == resolved.roots.knowledge);
    CHECK(paths.bundle_owner == "fixture-owner");
    CHECK(paths.restart_manager_service);
    CHECK_FALSE(paths.manager_service_access);
}

TEST_CASE("package-selected roots reuse policy children and reject overlap", "[package-layout]") {
    const json selection{
        {"schema", "arena_package_layout_selection_v1"}, {"scope", "per-user"},
        {"integration_root", "E:\\Programs\\UCI Arena Manager"},
        {"data_root", "E:\\Data\\UCI Arena Manager"},
    };
    const auto layout = installer::resolve_package_layout_selection(active_policy(), selection);
    CHECK(layout.roots.integration == fs::path{"E:\\Programs\\UCI Arena Manager"});
    CHECK(layout.roots.install == fs::path{"E:\\Programs\\UCI Arena Manager\\installed"});
    CHECK(layout.roots.workspace == fs::path{"E:\\Data\\UCI Arena Manager\\workspaces"});
    CHECK(layout.roots.cache == fs::path{"E:\\Data\\UCI Arena Manager\\cache"});
    CHECK(layout.roots.knowledge == fs::path{"E:\\Data\\UCI Arena Manager\\knowledge"});
    for (const auto field : {"schema", "scope", "integration_root", "data_root"}) {
        auto bad = selection;
        bad.erase(field);
        CHECK_THROWS_AS(installer::resolve_package_layout_selection(active_policy(), bad), std::invalid_argument);
    }
    for (const auto invalid : {"E:\\", "relative", "e:\\programs\\uci arena manager\\data", "E:\\Data\\..\\Arena", "E:\\Data\\CON"}) {
        auto bad = selection;
        bad["data_root"] = invalid;
        CHECK_THROWS_AS(installer::resolve_package_layout_selection(active_policy(), bad), std::invalid_argument);
    }
    auto bad = selection;
    bad["extra"] = true;
    CHECK_THROWS_AS(installer::resolve_package_layout_selection(active_policy(), bad), std::invalid_argument);
}

#ifdef _WIN32
TEST_CASE("Windows package selection distinguishes absence from partial typed and wrong-scope records", "[package-layout]") {
    GUID uuid{};
    REQUIRE(CoCreateGuid(&uuid) == S_OK);
    wchar_t text[40]{};
    REQUIRE(StringFromGUID2(uuid, text, 40) > 0);
    const std::wstring base = L"Software\\UCI Arena\\Tests\\PackageLayout-" + std::wstring{text};
    struct Cleanup {
        std::wstring key;
        ~Cleanup() { RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str()); }
    } cleanup{base};
    REQUIRE_FALSE(arena::platform::read_windows_package_layout_selection(ports::InstallScope::per_user, base).has_value());
    HKEY key{};
    REQUIRE(RegCreateKeyExW(HKEY_CURRENT_USER, (base + L"\\per-user").c_str(), 0, nullptr, 0,
        KEY_ALL_ACCESS | KEY_WOW64_64KEY, nullptr, &key, nullptr) == ERROR_SUCCESS);
    struct Close { HKEY key; ~Close() { RegCloseKey(key); } } close{key};
    const auto set = [&](const wchar_t* name, const wchar_t* value) {
        REQUIRE(RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value),
            static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t))) == ERROR_SUCCESS);
    };
    CHECK_THROWS(arena::platform::read_windows_package_layout_selection(ports::InstallScope::per_user, base));
    set(L"Schema", L"arena_package_layout_selection_v1");
    set(L"Scope", L"per-user");
    set(L"IntegrationRoot", L"E:\\Programs\\Arena\\");
    set(L"DataRoot", L"E:\\Data\\Arena");
    const auto loaded = arena::platform::read_windows_package_layout_selection(ports::InstallScope::per_user, base);
    REQUIRE(loaded.has_value());
    CHECK(loaded->at("integration_root") == "E:\\Programs\\Arena");
    CHECK(loaded->at("data_root") == "E:\\Data\\Arena");
    set(L"Scope", L"per-machine");
    CHECK_THROWS(arena::platform::read_windows_package_layout_selection(ports::InstallScope::per_user, base));
    set(L"Scope", L"per-user");
    const DWORD wrong_type = 4;
    REQUIRE(RegSetValueExW(key, L"DataRoot", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&wrong_type), sizeof(wrong_type)) == ERROR_SUCCESS);
    CHECK_THROWS(arena::platform::read_windows_package_layout_selection(ports::InstallScope::per_user, base));
    set(L"DataRoot", L"E:\\Data\\Arena");
    set(L"Extra", L"unexpected");
    CHECK_THROWS(arena::platform::read_windows_package_layout_selection(ports::InstallScope::per_user, base));
}
#endif

TEST_CASE("layout diagnostics are bounded and do not disclose rejected fields") {
    auto policy = active_policy();
    const std::string canary = "secret-" + std::string(600, 'x');
    policy[canary] = true;
    try {
        (void)installer::validate_install_layout_policy(policy);
        FAIL("expected policy rejection");
    } catch (const std::invalid_argument& error) {
        const std::string_view message{error.what()};
        CHECK(message.size() <= 512);
        CHECK(message.find(canary) == std::string_view::npos);
    }
}

}  // namespace
