#include "core/GameEvidence.hpp"

#include <bes/sdk/AddressSpace.hpp>

#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstring>

namespace bes::core {
namespace {

constexpr const char* kGameLibrary = "libminecraftpe.so";

/// Reads a short file into a string, never retaining the descriptor.
std::string readShortFile(const char* path, std::size_t limit) {
    const int descriptor = open(path, O_RDONLY);
    if (descriptor < 0) return {};

    std::string buffer;
    buffer.resize(limit);
    const auto size = read(descriptor, buffer.data(), limit);
    close(descriptor);

    if (size <= 0) return {};
    buffer.resize(static_cast<std::size_t>(size));

    // Trim every NUL and trailing newline; cmdline is NUL-separated argv and comm
    // carries a trailing newline, and neither is meaningful to a human reader.
    const auto nul = buffer.find('\0');
    if (nul != std::string::npos) buffer.resize(nul);
    while (!buffer.empty() && (buffer.back() == '\n' || buffer.back() == '\r')) buffer.pop_back();
    return buffer;
}

} // namespace

std::string EvidenceCollector::processName() {
    return readShortFile("/proc/self/cmdline", 256);
}

std::string EvidenceCollector::processCom() {
    return readShortFile("/proc/self/comm", 64);
}

bool EvidenceCollector::loaderSees(const char* name, std::string& note) {
    note.clear();
    if (!name || !*name) {
        note = "no library name";
        return false;
    }

    // RTLD_NOLOAD: "do not actually load the library; test whether it is already
    // loaded". RTLD_NOW is what the platform uses anyway (bionic ignores
    // RTLD_LAZY), and asking for it keeps the call identical to a real open.
    void* handle = dlopen(name, RTLD_NOW | RTLD_NOLOAD);
    if (!handle) {
        const char* error = dlerror();
        note = error ? error : "dlopen returned null";
        return false;
    }

    // The handle owns a reference even in the NOLOAD case, so it must be released.
    // This cannot unload the game: its own link-map entry holds a reference, and
    // bionic logs "not unloading - decrementing ref_count" while that stays > 0.
    dlclose(handle);
    return true;
}

GameEvidence EvidenceCollector::collect() {
    GameEvidence evidence;

    // 1. Presence, primary signal: what is actually mapped.
    auto& space = sdk::AddressSpace::get();
    space.refresh();
    evidence.libraryPath = space.modulePath(kGameLibrary);
    evidence.libraryMapped = !evidence.libraryPath.empty();

    // Presence, corroboration only. A null here proves nothing on its own -- see
    // the header -- which is exactly why it is a separate field.
    evidence.loaderSeesLibrary = loaderSees(kGameLibrary, evidence.loaderNote);

    // Identity, for the report only.
    evidence.processName = processName();
    evidence.processCom = processCom();

    return evidence;
}

} // namespace bes::core
