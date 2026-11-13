#include "bes/sdk/AddressSpace.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

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

    // Paths can exceed the 512-byte line buffer on Android (and a truncated line
    // is harmless: it simply yields no usable path, never a wrong one).
    char line[2048];
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

        // The pathname is the sixth whitespace-separated column. Locate it by
        // skipping the five leading fields rather than by sscanf with %s, which
        // would break on a path containing spaces.
        std::string_view view(line);
        std::size_t cursor = 0;
        int columns = 0;
        while (columns < 5 && cursor < view.size()) {
            while (cursor < view.size() && view[cursor] != ' ') ++cursor;
            while (cursor < view.size() && view[cursor] == ' ') ++cursor;
            ++columns;
        }
        std::string path;
        if (columns == 5 && cursor < view.size()) {
            path.assign(view.substr(cursor));
            while (!path.empty() && (path.back() == '\n' || path.back() == '\r')) path.pop_back();
        }
        mRegions[mCount].path = std::move(path);
        ++mCount;
    }
    std::fclose(file);

    mLoaded = mCount > 0;
    return mLoaded;
}

bool matchesLibraryPath(std::string_view path, std::string_view soname) {
    if (path.empty() || soname.empty()) return false;
    if (path.size() < soname.size()) return false;

    // Strip the loader's marker for a file replaced on disk; the mapping is still
    // the library we are asking about.
    constexpr std::string_view kDeleted = " (deleted)";
    if (path.size() > kDeleted.size() &&
        path.compare(path.size() - kDeleted.size(), kDeleted.size(), kDeleted) == 0) {
        path.remove_suffix(kDeleted.size());
    }
    if (path.size() < soname.size()) return false;

    if (path.compare(path.size() - soname.size(), soname.size(), soname) != 0) return false;

    // Whole-component match only: the character before the soname must be a path
    // separator, otherwise "libfoo.so" would match "/data/libbarfoo.so".
    const auto before = path.size() - soname.size();
    return before == 0 || path[before - 1] == '/';
}

bool AddressSpace::moduleLoaded(std::string_view libraryName) const {
    return !modulePath(libraryName).empty();
}

std::string AddressSpace::modulePath(std::string_view libraryName) const {
    if (libraryName.empty() || !mLoaded) return {};

    for (std::size_t index = 0; index < mCount; ++index) {
        // maps records an absolute path; callers pass the soname.
        if (matchesLibraryPath(mRegions[index].path, libraryName)) {
            return mRegions[index].path;
        }
    }
    return {};
}

std::size_t AddressSpace::fileBackedRegionCount() const {
    std::size_t count = 0;
    for (std::size_t index = 0; index < mCount; ++index) {
        if (!mRegions[index].path.empty()) ++count;
    }
    return count;
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
