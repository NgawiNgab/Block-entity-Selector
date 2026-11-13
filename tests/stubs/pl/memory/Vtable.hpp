#pragma once

/// Host-side stand-in for the preloader vtable resolver.
///
/// Returns 0 for every request, which is the same answer the real resolver gives
/// when the class is not present in a loaded library. Tests must never depend on
/// a vtable resolution succeeding.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace pl::memory {

inline std::uintptr_t resolveVtableFunction(std::string_view, std::size_t, std::string_view) {
    return 0;
}

} // namespace pl::memory
