#pragma once

#include <algorithm>
#include <cwctype>
#include <string>
#include <string_view>
#include <unordered_set>

namespace arena::installer::windows {

class RelatedBundlePlanGuard final {
public:
  void begin_plan() noexcept { claimed_bundle_codes_.clear(); }

  [[nodiscard]] bool claim(std::wstring_view bundle_code) {
    std::wstring normalized(bundle_code);
    std::ranges::transform(normalized, normalized.begin(), [](wchar_t value) {
      return static_cast<wchar_t>(std::towupper(value));
    });
    return claimed_bundle_codes_.insert(std::move(normalized)).second;
  }

private:
  std::unordered_set<std::wstring> claimed_bundle_codes_;
};

}  // namespace arena::installer::windows
