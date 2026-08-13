#include <windows.h>
#include <objbase.h>

#include <new>

#include <dutil.h>
#include <dictutil.h>
#include <BootstrapperApplicationBase.h>
#include <BAFunctions.h>

#include "related_bundle_plan_guard.hpp"

namespace {

struct BootstrapperContext final {
  arena::installer::windows::RelatedBundlePlanGuard related_bundles;
};

HRESULT WINAPI ArenaBAFunctionsProc(
    BA_FUNCTIONS_MESSAGE message,
    const LPVOID arguments,
    LPVOID results,
    LPVOID opaque_context) {
  auto* context = static_cast<BootstrapperContext*>(opaque_context);
  if (context == nullptr) {
    return E_POINTER;
  }

  switch (message) {
    case BA_FUNCTIONS_MESSAGE_ONDESTROY:
      delete context;
      return S_OK;
    case BA_FUNCTIONS_MESSAGE_ONDETECTBEGIN:
      context->related_bundles.begin_detect();
      return S_OK;
    case BA_FUNCTIONS_MESSAGE_ONDETECTRELATEDBUNDLE: {
      const auto* detect_arguments =
          static_cast<BA_ONDETECTRELATEDBUNDLE_ARGS*>(arguments);
      if (detect_arguments == nullptr ||
          detect_arguments->wzBundleCode == nullptr) {
        return E_INVALIDARG;
      }
      context->related_bundles.observe(
          detect_arguments->wzBundleCode,
          detect_arguments->fMissingFromCache != FALSE);
      return S_OK;
    }
    case BA_FUNCTIONS_MESSAGE_ONPLANBEGIN:
      context->related_bundles.begin_plan();
      return S_OK;
    case BA_FUNCTIONS_MESSAGE_ONPLANRELATEDBUNDLE: {
      const auto* plan_arguments =
          static_cast<BA_ONPLANRELATEDBUNDLE_ARGS*>(arguments);
      auto* plan_results = static_cast<BA_ONPLANRELATEDBUNDLE_RESULTS*>(results);
      if (plan_arguments == nullptr || plan_arguments->wzBundleCode == nullptr
          || plan_results == nullptr) {
        return E_INVALIDARG;
      }
      if (!context->related_bundles.claim(plan_arguments->wzBundleCode)) {
        plan_results->requestedState = BOOTSTRAPPER_REQUEST_STATE_NONE;
      }
      return S_OK;
    }
    case BA_FUNCTIONS_MESSAGE_ONPLANRESTORERELATEDBUNDLE: {
      const auto* restore_arguments =
          static_cast<BA_ONPLANRESTORERELATEDBUNDLE_ARGS*>(arguments);
      auto* restore_results =
          static_cast<BA_ONPLANRESTORERELATEDBUNDLE_RESULTS*>(results);
      if (restore_arguments == nullptr ||
          restore_arguments->wzBundleCode == nullptr ||
          restore_results == nullptr) {
        return E_INVALIDARG;
      }
      if (!context->related_bundles.allow_restore(
              restore_arguments->wzBundleCode)) {
        restore_results->requestedState = BOOTSTRAPPER_REQUEST_STATE_NONE;
      }
      return S_OK;
    }
    default:
      return E_NOTIMPL;
  }
}

}  // namespace

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void*) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(instance);
  }
  return TRUE;
}

extern "C" HRESULT WINAPI BAFunctionsCreate(
    const BA_FUNCTIONS_CREATE_ARGS* arguments,
    BA_FUNCTIONS_CREATE_RESULTS* results) {
  if (arguments == nullptr || results == nullptr) {
    return E_INVALIDARG;
  }
  auto* context = new (std::nothrow) BootstrapperContext();
  if (context == nullptr) {
    return E_OUTOFMEMORY;
  }
  results->pfnBAFunctionsProc = ArenaBAFunctionsProc;
  results->pvBAFunctionsProcContext = context;
  return S_OK;
}

extern "C" void WINAPI BAFunctionsDestroy(
    const BA_FUNCTIONS_DESTROY_ARGS*,
    BA_FUNCTIONS_DESTROY_RESULTS*) {}
