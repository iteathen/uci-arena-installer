#include <catch2/catch_test_macros.hpp>

#include "related_bundle_plan_guard.hpp"

using arena::installer::windows::RelatedBundlePlanGuard;

TEST_CASE("related bundle plan claims one action per exact identity") {
  RelatedBundlePlanGuard guard;

  CHECK(guard.claim(L"{A23EAB9E-93D2-4412-B00F-28D4472CBE2E}"));
  CHECK_FALSE(guard.claim(L"{a23eab9e-93d2-4412-b00f-28d4472cbe2e}"));
  CHECK(guard.claim(L"{F9B3CA6E-F1EC-40A6-AE88-9C8EE40BDE21}"));
}

TEST_CASE("related bundle plan starts a fresh identity ledger") {
  RelatedBundlePlanGuard guard;
  constexpr auto identity = L"{E945C3DD-8264-4E8C-823B-7B05F81401D7}";

  CHECK(guard.claim(identity));
  CHECK_FALSE(guard.claim(identity));
  guard.begin_plan();
  CHECK(guard.claim(identity));
}
