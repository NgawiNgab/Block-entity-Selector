#pragma once

#include "core/Profile.hpp"

#include <bes/sdk/Types.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
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

    /// How many signatures resolved, and how many exist. Reported separately so
    /// the diagnostics can state resolution as a number instead of leaving it to
    /// be inferred from whether a later line is present.
    std::size_t resolvedCount() const noexcept { return mResolvedCount; }
    static constexpr std::size_t totalCount() noexcept { return SigCount; }

    /// A usable offset for a category, or 0 when nothing has proven one.
    ///
    /// This used to return the profile's first candidate from the moment the
    /// profile loaded, which meant every offset in signatures.json was live from
    /// the first frame whether or not it had ever matched this game build. That is
    /// how a wrong `actor_level` turned into a SIGSEGV inside game code: the
    /// caller had no way to tell "proven on a live object" from "guess from a file
    /// with zero validated entries". Now an unprobed category reads as 0, and 0
    /// never points at anything, so callers skip instead of faulting.
    ///
    /// Callers that must distinguish "no candidate exists" from "not proven yet"
    /// use `offsetState()`.
    std::size_t offset(OffId id) const;

    /// True only once a live object has confirmed the offset for this category.
    bool offsetValidated(OffId id) const;

    /// The profile's first candidate, whether proven or not. Diagnostics only:
    /// never dereference through this.
    std::size_t offsetCandidate(OffId id) const;

    /// Records a probe result once a live object confirms an offset. Calling it
    /// twice with the same value is a no-op.
    void confirmOffset(OffId id, std::size_t value, std::string_view evidence);

    /// True when at least one offset candidate exists for the category.
    bool hasOffsetCandidates(OffId id) const;

    /// The report body, copied under the lock.
    ///
    /// This used to return a reference. `summary()` is read from the launcher UI
    /// thread (diagnostics dump) while `confirmOffset()` rebuilds the string from
    /// the render thread, so a reference handed one thread a buffer the other was
    /// rewriting. A copy costs a string and cannot tear.
    std::string summary() const;

    /// Rebuilds the human readable report (used by the diagnostics dump).
    void refreshSummary();

private:
    Resolver() = default;

    void resolveOne(SigId id, std::string_view libraryName);
    /// Rebuilds mSummary. Caller must hold mMutex.
    void buildSummaryLocked();
    std::uintptr_t resolveVtableCandidate(const SigCandidate& candidate, std::string_view libraryName);

    std::uintptr_t mAddresses[SigCount]{};
    std::string mOrigins[SigCount];
    /// The profile's first candidate per category: a hypothesis, not a fact.
    std::size_t mOffCandidates[OffCount]{};
    /// The proven value per category, and whether anything proved it.
    std::size_t mOffsets[OffCount]{};
    bool mOffValidated[OffCount]{};
    std::string mOffOrigins[OffCount];
    std::string mSummary;
    std::size_t mResolvedCount = 0;
    bool mResolved = false;
    /// Guards the offset tables and mSummary. Offsets are confirmed from the render
    /// thread and read from the frame, scan and UI threads.
    mutable std::mutex mMutex;
};

}
