#include <windows.h>
#include <objbase.h>
#include <msi.h>

#include <BootstrapperApplicationTypes.h>
#include <BAFunctions.h>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>

namespace {
class CallbackSession final {
public:
  CallbackSession() {
    module_ = LoadLibraryExW(std::filesystem::path(ARENA_BUNDLE_BAFUNCTIONS_PATH).c_str(),
                            nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                         LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    REQUIRE(module_ != nullptr);
    const auto create = reinterpret_cast<HRESULT(WINAPI*)(
        const BA_FUNCTIONS_CREATE_ARGS*, BA_FUNCTIONS_CREATE_RESULTS*)>(
        GetProcAddress(module_, "BAFunctionsCreate"));
    REQUIRE(create != nullptr);
    BA_FUNCTIONS_CREATE_ARGS arguments{};
    arguments.cbSize = sizeof(arguments);
    results_.cbSize = sizeof(results_);
    REQUIRE(create(&arguments, &results_) == S_OK);
    REQUIRE(results_.pfnBAFunctionsProc != nullptr);
  }
  ~CallbackSession() {
    if (results_.pfnBAFunctionsProc) {
      results_.pfnBAFunctionsProc(BA_FUNCTIONS_MESSAGE_ONDESTROY, nullptr,
                                 nullptr, results_.pvBAFunctionsProcContext);
    }
    if (module_) FreeLibrary(module_);
  }
  HRESULT invoke(BA_FUNCTIONS_MESSAGE message, void* arguments, void* results) {
    return results_.pfnBAFunctionsProc(message, arguments, results,
                                     results_.pvBAFunctionsProcContext);
  }
private:
  HMODULE module_{};
  BA_FUNCTIONS_CREATE_RESULTS results_{};
};
}

TEST_CASE("actual WiX callback excludes every historical Upgrade executor and restore") {
  CallbackSession session;
  REQUIRE(session.invoke(BA_FUNCTIONS_MESSAGE_ONDETECTBEGIN, nullptr, nullptr) == S_OK);
  REQUIRE(session.invoke(BA_FUNCTIONS_MESSAGE_ONPLANBEGIN, nullptr, nullptr) == S_OK);
  for (const auto identity : {L"{2FD2FC28-3D74-4536-A2C2-A96AD05DAC35}",
                             L"{D23CE203-CEB3-4D53-BC79-887FF02FACE9}"}) {
    // Burn emits a separate callback for each related registration occurrence.
    for (int occurrence = 0; occurrence != 2; ++occurrence) {
      BA_ONPLANRELATEDBUNDLETYPE_ARGS type{};
      type.wzBundleCode = identity;
      type.recommendedType = BOOTSTRAPPER_RELATED_BUNDLE_PLAN_TYPE_UPGRADE;
      BA_ONPLANRELATEDBUNDLETYPE_RESULTS type_result{};
      type_result.requestedType = type.recommendedType;
      CHECK(session.invoke(BA_FUNCTIONS_MESSAGE_ONPLANRELATEDBUNDLETYPE,
                           &type, &type_result) == S_OK);
      CHECK(type_result.requestedType == BOOTSTRAPPER_RELATED_BUNDLE_PLAN_TYPE_NONE);
    }
    for (int occurrence = 0; occurrence != 2; ++occurrence) {
      BA_ONPLANRELATEDBUNDLE_ARGS plan{};
      plan.wzBundleCode = identity;
      plan.recommendedState = BOOTSTRAPPER_REQUEST_STATE_ABSENT;
      BA_ONPLANRELATEDBUNDLE_RESULTS plan_result{};
      plan_result.requestedState = plan.recommendedState;
      CHECK(session.invoke(BA_FUNCTIONS_MESSAGE_ONPLANRELATEDBUNDLE,
                           &plan, &plan_result) == S_OK);
      CHECK(plan_result.requestedState == BOOTSTRAPPER_REQUEST_STATE_NONE);
      // Restore is a separate phase, not implied by execute=None.
      BA_ONPLANRESTORERELATEDBUNDLE_ARGS restore{};
      restore.wzBundleCode = identity;
      restore.recommendedState = BOOTSTRAPPER_REQUEST_STATE_FORCE_PRESENT;
      BA_ONPLANRESTORERELATEDBUNDLE_RESULTS restore_result{};
      restore_result.requestedState = restore.recommendedState;
      CHECK(session.invoke(BA_FUNCTIONS_MESSAGE_ONPLANRESTORERELATEDBUNDLE,
                           &restore, &restore_result) == S_OK);
      CHECK(restore_result.requestedState == BOOTSTRAPPER_REQUEST_STATE_NONE);
    }
  }
}

TEST_CASE("actual WiX callback preserves downgrade protection and independent addon plan") {
  CallbackSession session;
  BA_ONPLANRELATEDBUNDLETYPE_ARGS type{};
  type.wzBundleCode = L"{94D06051-0E16-48AB-A430-56E3D3EA0B47}";
  type.recommendedType = BOOTSTRAPPER_RELATED_BUNDLE_PLAN_TYPE_DOWNGRADE;
  BA_ONPLANRELATEDBUNDLETYPE_RESULTS result{};
  result.requestedType = type.recommendedType;
  CHECK(session.invoke(BA_FUNCTIONS_MESSAGE_ONPLANRELATEDBUNDLETYPE, &type, &result) == S_OK);
  CHECK(result.requestedType == BOOTSTRAPPER_RELATED_BUNDLE_PLAN_TYPE_DOWNGRADE);
  type.recommendedType = BOOTSTRAPPER_RELATED_BUNDLE_PLAN_TYPE_ADDON;
  result.requestedType = type.recommendedType;
  CHECK(session.invoke(BA_FUNCTIONS_MESSAGE_ONPLANRELATEDBUNDLETYPE, &type, &result) == S_OK);
  CHECK(result.requestedType == BOOTSTRAPPER_RELATED_BUNDLE_PLAN_TYPE_ADDON);
  BA_ONPLANRELATEDBUNDLE_ARGS plan{};
  plan.wzBundleCode = type.wzBundleCode;
  BA_ONPLANRELATEDBUNDLE_RESULTS plan_result{};
  plan_result.requestedState = BOOTSTRAPPER_REQUEST_STATE_PRESENT;
  CHECK(session.invoke(BA_FUNCTIONS_MESSAGE_ONPLANRELATEDBUNDLE, &plan, &plan_result) == S_OK);
  CHECK(plan_result.requestedState == BOOTSTRAPPER_REQUEST_STATE_PRESENT);
}

TEST_CASE("actual WiX theme loaded callback succeeds without UI customization") {
  CallbackSession session;
  BA_FUNCTIONS_ONTHEMELOADED_ARGS arguments{};
  arguments.cbSize = sizeof(arguments);
  BA_FUNCTIONS_ONTHEMELOADED_RESULTS results{};
  results.cbSize = sizeof(results);
  CHECK(session.invoke(BA_FUNCTIONS_MESSAGE_ONTHEMELOADED, &arguments, &results) == S_OK);
}

TEST_CASE("actual WiX callback exclusion resets only at a new plan or detection") {
  CallbackSession session;
  constexpr auto identity = L"{94D06051-0E16-48AB-A430-56E3D3EA0B47}";
  BA_ONPLANRELATEDBUNDLETYPE_ARGS type{};
  type.wzBundleCode = identity;
  type.recommendedType = BOOTSTRAPPER_RELATED_BUNDLE_PLAN_TYPE_UPGRADE;
  BA_ONPLANRELATEDBUNDLETYPE_RESULTS type_result{};
  type_result.requestedType = type.recommendedType;
  REQUIRE(session.invoke(BA_FUNCTIONS_MESSAGE_ONPLANRELATEDBUNDLETYPE,
                         &type, &type_result) == S_OK);
  BA_ONPLANRESTORERELATEDBUNDLE_ARGS restore{};
  restore.wzBundleCode = L"{94d06051-0e16-48ab-a430-56e3d3ea0b47}";
  BA_ONPLANRESTORERELATEDBUNDLE_RESULTS restore_result{};
  restore_result.requestedState = BOOTSTRAPPER_REQUEST_STATE_FORCE_PRESENT;
  REQUIRE(session.invoke(BA_FUNCTIONS_MESSAGE_ONPLANRESTORERELATEDBUNDLE,
                         &restore, &restore_result) == S_OK);
  CHECK(restore_result.requestedState == BOOTSTRAPPER_REQUEST_STATE_NONE);
  REQUIRE(session.invoke(BA_FUNCTIONS_MESSAGE_ONPLANBEGIN, nullptr, nullptr) == S_OK);
  type.recommendedType = BOOTSTRAPPER_RELATED_BUNDLE_PLAN_TYPE_PATCH;
  type_result.requestedType = type.recommendedType;
  REQUIRE(session.invoke(BA_FUNCTIONS_MESSAGE_ONPLANRELATEDBUNDLETYPE,
                         &type, &type_result) == S_OK);
  CHECK(type_result.requestedType == BOOTSTRAPPER_RELATED_BUNDLE_PLAN_TYPE_PATCH);
  BA_ONPLANRELATEDBUNDLE_ARGS plan{};
  plan.wzBundleCode = identity;
  BA_ONPLANRELATEDBUNDLE_RESULTS plan_result{};
  plan_result.requestedState = BOOTSTRAPPER_REQUEST_STATE_PRESENT;
  REQUIRE(session.invoke(BA_FUNCTIONS_MESSAGE_ONPLANRELATEDBUNDLE, &plan, &plan_result) == S_OK);
  CHECK(plan_result.requestedState == BOOTSTRAPPER_REQUEST_STATE_PRESENT);
  plan_result.requestedState = BOOTSTRAPPER_REQUEST_STATE_PRESENT;
  REQUIRE(session.invoke(BA_FUNCTIONS_MESSAGE_ONPLANRELATEDBUNDLE, &plan, &plan_result) == S_OK);
  CHECK(plan_result.requestedState == BOOTSTRAPPER_REQUEST_STATE_NONE);
  REQUIRE(session.invoke(BA_FUNCTIONS_MESSAGE_ONDETECTBEGIN, nullptr, nullptr) == S_OK);
  plan_result.requestedState = BOOTSTRAPPER_REQUEST_STATE_PRESENT;
  REQUIRE(session.invoke(BA_FUNCTIONS_MESSAGE_ONPLANRELATEDBUNDLE, &plan, &plan_result) == S_OK);
  CHECK(plan_result.requestedState == BOOTSTRAPPER_REQUEST_STATE_PRESENT);
}

TEST_CASE("actual WiX missing cache guard still suppresses execute and independent restore") {
  CallbackSession session;
  BA_ONDETECTRELATEDBUNDLE_ARGS detect{};
  detect.wzBundleCode = L"{43A6C14D-4B5C-4E8B-B967-A62200DB7035}";
  detect.fMissingFromCache = TRUE;
  REQUIRE(session.invoke(BA_FUNCTIONS_MESSAGE_ONDETECTRELATEDBUNDLE, &detect, nullptr) == S_OK);
  REQUIRE(session.invoke(BA_FUNCTIONS_MESSAGE_ONPLANBEGIN, nullptr, nullptr) == S_OK);
  BA_ONPLANRELATEDBUNDLE_ARGS plan{};
  plan.wzBundleCode = detect.wzBundleCode;
  BA_ONPLANRELATEDBUNDLE_RESULTS plan_result{};
  plan_result.requestedState = BOOTSTRAPPER_REQUEST_STATE_ABSENT;
  REQUIRE(session.invoke(BA_FUNCTIONS_MESSAGE_ONPLANRELATEDBUNDLE, &plan, &plan_result) == S_OK);
  CHECK(plan_result.requestedState == BOOTSTRAPPER_REQUEST_STATE_NONE);
  BA_ONPLANRESTORERELATEDBUNDLE_ARGS restore{};
  restore.wzBundleCode = detect.wzBundleCode;
  BA_ONPLANRESTORERELATEDBUNDLE_RESULTS restore_result{};
  restore_result.requestedState = BOOTSTRAPPER_REQUEST_STATE_FORCE_PRESENT;
  REQUIRE(session.invoke(BA_FUNCTIONS_MESSAGE_ONPLANRESTORERELATEDBUNDLE, &restore, &restore_result) == S_OK);
  CHECK(restore_result.requestedState == BOOTSTRAPPER_REQUEST_STATE_NONE);
}

TEST_CASE("actual WiX callback rejects malformed plan input without admitting a replacement") {
  CallbackSession session;
  BA_ONPLANRELATEDBUNDLETYPE_RESULTS type_result{};
  CHECK(session.invoke(BA_FUNCTIONS_MESSAGE_ONPLANRELATEDBUNDLETYPE, nullptr,
                       &type_result) == E_INVALIDARG);
  BA_ONPLANRELATEDBUNDLETYPE_ARGS type{};
  CHECK(session.invoke(BA_FUNCTIONS_MESSAGE_ONPLANRELATEDBUNDLETYPE, &type,
                       &type_result) == E_INVALIDARG);
  type.wzBundleCode = L"{43A6C14D-4B5C-4E8B-B967-A62200DB7035}";
  CHECK(session.invoke(BA_FUNCTIONS_MESSAGE_ONPLANRELATEDBUNDLETYPE, &type,
                       nullptr) == E_INVALIDARG);
  CHECK(session.invoke(BA_FUNCTIONS_MESSAGE_ONPLANRELATEDBUNDLE, nullptr,
                       nullptr) == E_INVALIDARG);
  CHECK(session.invoke(BA_FUNCTIONS_MESSAGE_ONPLANRESTORERELATEDBUNDLE, nullptr,
                       nullptr) == E_INVALIDARG);
}
