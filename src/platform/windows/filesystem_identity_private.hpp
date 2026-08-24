#pragma once

#include <cstdint>

namespace arena::platform::windows_detail {

[[noreturn]] void
throw_stable_identity_unavailable(std::uint32_t native_code);

} // namespace arena::platform::windows_detail
