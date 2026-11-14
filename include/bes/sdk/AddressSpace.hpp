#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace bes::sdk {

/// True when a `/proc/self/maps` path column refers to `soname`.
///
/// Split out as a pure predicate so it can be tested without the library being
/// present: a host test cannot map `libminecraftpe.so`, so a check that only went
/// through the public query would never exercise the matching rule itself.
///
/// Handles the two shapes the path column takes: a plain absolute path, and a
/// path with the loader's ` (deleted)` suffix when the file was replaced on disk.
/// Matching is by whole path component, so `libfoo.so` must not match
/// `libbarfoo.so`.
bool matchesLibraryPath(std::string_view path, std::string_view soname);

/// Answers "may I read this address?" by consulting `/proc/self/maps`.
///
/// The mod reads objects the game owns, and those objects contain fields whose
/// meaning is still unknown. A heuristic test ("non-null, 8-byte aligned") is not
/// enough: a garbage field can look exactly like a pointer and point at an
/// unmapped page, and a diagnostic that takes the game down is worse than no
/// diagnostic at all.
///
/// The map is read once and cached; Android does not remap the game's own image
/// or heap in normal play, so a stale table only ever costs a missed dump, never
/// a wild read. Call `refresh()` if a long-lived probe needs a fresh view.
class AddressSpace {
public:
    static AddressSpace& get();

    /// Re-reads `/proc/self/maps`. Returns false when it could not be parsed, in
    /// which case `readable` rejects everything and the probes stay safe.
    bool refresh();

    /// True only when the whole range lies inside mapped, readable regions.
    bool readable(const void* address, std::size_t size) const;

    bool readable(std::uintptr_t address, std::size_t size) const {
        return readable(reinterpret_cast<const void*>(address), size);
    }

    /// Number of mapped readable regions found, for diagnostics.
    std::size_t regionCount() const noexcept { return mCount; }

    /// True when any mapped region's path ends with `libraryName`.
    ///
    /// This is the primary "is the game loaded in this process?" signal, and it
    /// is deliberately independent of the dynamic loader. `dlopen(RTLD_NOLOAD)`
    /// needs the linker to find the file by name in the calling namespace and
    /// returns null when it cannot, even though the library is mapped -- so it
    /// reports false negatives. `/proc/self/maps` reports what is actually there.
    bool moduleLoaded(std::string_view libraryName) const;

    /// The mapped path recorded for `libraryName`, or empty when absent.
    std::string modulePath(std::string_view libraryName) const;

    /// How many mapped regions carry a pathname.
    ///
    /// This exists as a positive control: if the path column stops being parsed,
    /// every "is it loaded?" query silently answers no, and a test built only from
    /// negative assertions would still pass. Any real process has at least one
    /// file-backed mapping.
    std::size_t fileBackedRegionCount() const;

    /// True when the last refresh stopped at the region cap.
    ///
    /// A truncated map does not fail loudly: every region past the cut simply
    /// reads as unmapped, so every guarded check on it answers "not readable" and
    /// the probes quietly reject correct candidates forever. Callers that see this
    /// must treat a negative answer as "unknown", not as "no".
    bool truncated() const noexcept { return mTruncated; }

    /// Region capacity.
    ///
    /// This was 4096, and a real Minecraft process maps **6022** regions (measured
    /// from the 2026-11-13 tombstone). The library's own mappings land well past
    /// the cut, which is why every offset probe reported "unreadable" and the
    /// diagnostics read `offsets proven: 0/11`.
    ///
    /// Regions beyond the lowest library mapping would still be invisible, so the
    /// buffer is sized for the whole map rather than for what fits comfortably: at
    /// 64 bytes per region this is 1 MiB of BSS, which is nothing next to the
    /// 340 MB game image, and being incomplete is the failure mode that costs
    /// days.
    static constexpr std::size_t kMaxRegions = 16384;

private:
    AddressSpace() = default;

    struct Region {
        std::uintptr_t begin = 0;
        std::uintptr_t end = 0;
        /// Path column of the maps line; empty for anonymous mappings.
        std::string path;
    };

    Region mRegions[kMaxRegions]{};
    std::size_t mCount = 0;
    bool mLoaded = false;
    bool mTruncated = false;
};

} // namespace bes::sdk
