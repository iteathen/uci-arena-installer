#pragma once

#if defined(__linux__)
#include <cstddef>
#include <sys/types.h>

namespace arena::platform::linux_identity_testing {

using PreadFunction = ssize_t (*)(int, void*, std::size_t, off_t);
using OpenAtFunction = int (*)(int, const char*, int);

void set_pread_function(PreadFunction function) noexcept;
void reset_pread_function() noexcept;
void set_openat_function(OpenAtFunction function) noexcept;
void reset_openat_function() noexcept;

} // namespace arena::platform::linux_identity_testing
#endif
