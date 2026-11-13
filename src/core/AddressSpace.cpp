#include "bes/sdk/AddressSpace.hpp"

#include <cstdio>
#include <cstring>

namespace bes::sdk {

AddressSpace& AddressSpace::get() {
    static AddressSpace instance;
    return instance;
}

bool AddressSpace::refresh() {
    mCount = 0;
    mLoaded = false;

    std::FILE* file = std::fopen("/proc/self/maps", "r");
    if (!file) return false;

    char line[512];
    while (mCount < kMaxRegions && std::fgets(line, sizeof(line), file)) {
        // Format: start-end perms offset dev inode pathname
        unsigned long long begin = 0;
        unsigned long long end = 0;
        char permissions[8]{};
        if (std::sscanf(line, "%llx-%llx %7s", &begin, &end, permissions) != 3) continue;
        // Only regions the process may read are candidates.
        if (permissions[0] != 'r') continue;
        if (end <= begin) continue;
        mRegions[mCount].begin = static_cast<std::uintptr_t>(begin);
        mRegions[mCount].end = static_cast<std::uintptr_t>(end);
        ++mCount;
    }
    std::fclose(file);

    mLoaded = mCount > 0;
    return mLoaded;
}

bool AddressSpace::readable(const void* address, std::size_t size) const {
    if (!mLoaded || !address || size == 0) return false;

    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    // Reject a range that would wrap around the address space.
    if (begin > UINTPTR_MAX - size) return false;
    const auto end = begin + size;

    for (std::size_t index = 0; index < mCount; ++index) {
        const auto& region = mRegions[index];
        if (begin >= region.begin && end <= region.end) return true;
    }
    return false;
}

} // namespace bes::sdk
