#pragma once

/// Host-side stand-in for the preloader signature scanner.
///
/// The tests target compiles Resolver.cpp on a desktop toolchain where the real
/// preloader SDK is unavailable. `Resolver` is exercised for its *offset* state
/// machine, which is pure bookkeeping; pattern scanning needs a live Android
/// process and returns nothing here, exactly as it would for a library that is
/// not loaded.

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pl::memory {

/// Always empty: no library is mapped in the host test process.
inline std::unordered_map<std::string, std::uintptr_t>
resolveSignatures(const std::vector<std::string>&, std::string_view) {
    return {};
}

} // namespace pl::memory
