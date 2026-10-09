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

TEST_CASE("related bundles missing from cache receive no execute or restore action") {
  RelatedBundlePlanGuard guard;
  constexpr auto missing = L"{43A6C14D-4B5C-4E8B-B967-A62200DB7035}";
  constexpr auto cached = L"{3857CC73-F47D-45B6-A419-25F7FBD4745F}";

  guard.begin_detect();
  guard.observe(missing, true);
  guard.observe(cached, false);
  guard.begin_plan();

  CHECK_FALSE(guard.claim(missing));
  CHECK_FALSE(guard.allow_restore(missing));
  CHECK(guard.claim(cached));
  CHECK(guard.allow_restore(cached));
}

TEST_CASE("a new detection pass clears stale missing-cache observations") {
  RelatedBundlePlanGuard guard;
  constexpr auto identity = L"{F0C731FA-6AD6-4D36-AE12-9B645D9945F8}";

  guard.begin_detect();
  guard.observe(identity, true);
  CHECK_FALSE(guard.allow_restore(identity));

  guard.begin_detect();
  guard.observe(identity, false);
  guard.begin_plan();
  CHECK(guard.claim(identity));
  CHECK(guard.allow_restore(identity));
}

TEST_CASE("historical upgrade identity has no execute or restore authority") {
  RelatedBundlePlanGuard guard;
  constexpr auto identity = L"{F0C731FA-6AD6-4D36-AE12-9B645D9945F8}";
  guard.exclude_upgrade(identity);
  CHECK_FALSE(guard.claim(identity));
  CHECK_FALSE(guard.claim(identity));
  CHECK_FALSE(guard.allow_restore(L"{f0c731fa-6ad6-4d36-ae12-9b645d9945f8}"));
  guard.begin_plan();
  CHECK(guard.claim(identity));
  CHECK(guard.allow_restore(identity));
  guard.exclude_upgrade(identity);
  guard.begin_detect();
  CHECK(guard.allow_restore(identity));
}
