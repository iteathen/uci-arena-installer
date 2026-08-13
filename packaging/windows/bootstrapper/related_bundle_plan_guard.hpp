#pragma once

#include <algorithm>
#include <cwctype>
#include <string>
#include <string_view>
#include <unordered_set>

namespace arena::installer::windows {

class RelatedBundlePlanGuard final {
public:
  void begin_detect() noexcept {
    claimed_bundle_codes_.clear();
    missing_cache_bundle_codes_.clear();
  }

  void begin_plan() noexcept { claimed_bundle_codes_.clear(); }

  void observe(std::wstring_view bundle_code, bool missing_from_cache) {
    if (missing_from_cache) {
      missing_cache_bundle_codes_.insert(normalize(bundle_code));
    }
  }

  [[nodiscard]] bool claim(std::wstring_view bundle_code) {
    std::wstring normalized = normalize(bundle_code);
    if (missing_cache_bundle_codes_.contains(normalized)) {
      return false;
    }
    return claimed_bundle_codes_.insert(std::move(normalized)).second;
  }

  [[nodiscard]] bool allow_restore(std::wstring_view bundle_code) const {
    return !missing_cache_bundle_codes_.contains(normalize(bundle_code));
  }

private:
  [[nodiscard]] static std::wstring normalize(std::wstring_view bundle_code) {
    std::wstring normalized(bundle_code);
    std::ranges::transform(normalized, normalized.begin(), [](wchar_t value) {
      return static_cast<wchar_t>(std::towupper(value));
    });
    return normalized;
  }

  std::unordered_set<std::wstring> claimed_bundle_codes_;
  std::unordered_set<std::wstring> missing_cache_bundle_codes_;
};

}  // namespace arena::installer::windows
