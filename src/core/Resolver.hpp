#pragma once

#include "core/Profile.hpp"

#include <bes/sdk/Types.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace bes::core {

/// A function pointer resolved from the profile, plus how it was found.
struct ResolvedFunction {
    std::uintptr_t address = 0;
    std::string origin;

    explicit operator bool() const noexcept { return address != 0; }
};

/// Owns all signature resolution and offset probing for the session.
class Resolver {
public:
    static Resolver& get();

    /// Resolves every signature category. Safe to call more than once.
    bool resolveAll(std::string_view libraryName = "libminecraftpe.so");

    /// Fast path called from hooks/tick callbacks, no locking.
    std::uintptr_t address(SigId id) const;

    /// Offset chosen for a category, or the profile default when nothing was
    /// confirmed yet.
    std::size_t offset(OffId id) const;

    /// Records a probe result once a live object confirms an offset. Calling it
    /// twice with the same value is a no-op.
    void confirmOffset(OffId id, std::size_t value, std::string_view evidence);

    /// True when at least one offset candidate exists for the category.
    bool hasOffsetCandidates(OffId id) const;

    const std::string& summary() const noexcept { return mSummary; }

    /// Rebuilds the human readable report (used by the diagnostics dump).
    void refreshSummary();

private:
    Resolver() = default;

    void resolveOne(SigId id, std::string_view libraryName);
    std::uintptr_t resolveVtableCandidate(const SigCandidate& candidate, std::string_view libraryName);

    std::uintptr_t mAddresses[SigCount]{};
    std::string mOrigins[SigCount];
    std::size_t mOffsets[OffCount]{};
    std::string mOffOrigins[OffCount];
    std::string mSummary;
    bool mResolved = false;
};

}
