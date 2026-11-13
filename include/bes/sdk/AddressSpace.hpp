#pragma once

#include <cstddef>
#include <cstdint>

namespace bes::sdk {

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

    static constexpr std::size_t kMaxRegions = 4096;

private:
    AddressSpace() = default;

    struct Region {
        std::uintptr_t begin = 0;
        std::uintptr_t end = 0;
    };

    Region mRegions[kMaxRegions]{};
    std::size_t mCount = 0;
    bool mLoaded = false;
};

} // namespace bes::sdk
