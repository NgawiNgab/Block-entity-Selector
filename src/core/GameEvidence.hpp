#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace bes::core {

/// The three independent answers that decide whether the mod can do anything.
///
/// They must never be collapsed into one boolean. "The game library is mapped"
/// does not imply "the signatures resolved", and "the signatures resolved" does
/// not imply "the hooks installed". Collapsing them is what turned a single
/// broken link into an undiagnosable report, so they are recorded separately and
/// reported separately.
struct GameEvidence {
    /// 1. Presence: is the game's image mapped into this process?
    ///
    /// Primary signal is `/proc/self/maps`, because that reports what is actually
    /// mapped. `dlopen(RTLD_NOLOAD)` is only corroboration: the linker has to
    /// find the file by name in the calling namespace and returns null when it
    /// cannot, even though the library is loaded.
    bool libraryMapped = false;
    std::string libraryPath;      ///< Path column from maps, when found.
    bool loaderSeesLibrary = false; ///< dlopen(RTLD_NOLOAD) corroboration.
    std::string loaderNote;       ///< Why dlopen failed, short form.

    /// Process identity. Recorded for diagnosis only -- never used as a gate.
    ///
    /// Android sets `cmdline` inconsistently across zygote forks and may leave it
    /// empty, which is why this cannot be trusted to decide anything.
    std::string processName;      ///< argv[0] from /proc/self/cmdline.
    std::string processCom;       ///< /proc/self/comm, the kernel's own name.

    /// 2. Resolution: how many of the mod's signatures were found in the image.
    bool resolved = false;
    std::size_t resolvedCount = 0;
    std::size_t signatureCount = 0;

    /// 3. Hooks: did they actually install? This is the only state that proves
    /// the mod can observe the world.
    bool tickHookInstalled = false;
    bool frameTickInstalled = false;
};

/// Collects the evidence. Every probe is safe to call outside the game: failure
/// yields a false/empty field rather than an exception or a crash.
class EvidenceCollector {
public:
    /// Reads `/proc/self/maps`, `/proc/self/cmdline` and `/proc/self/comm` and
    /// probes the loader. Returns the presence half of the evidence.
    static GameEvidence collect();

    /// Reads argv[0] without retaining the file descriptor.
    static std::string processName();

    /// Reads `/proc/self/comm`.
    static std::string processCom();

    /// True when `dlopen(name, RTLD_NOLOAD)` succeeds.
    ///
    /// The returned handle owns a reference even for RTLD_NOLOAD -- bionic's
    /// find_library() calls increment_ref_count() before returning -- so the
    /// handle is always released here with dlclose(). Because the game's own
    /// link-map entry holds a reference, that dlclose decrements back to a
    /// non-zero count and cannot unload the library. The handle is never stored.
    static bool loaderSees(const char* name, std::string& note);
};

} // namespace bes::core
