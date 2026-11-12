#include "render/GlState.hpp"

#include "core/Logging.hpp"

#include <dlfcn.h>

#include <mutex>

namespace bes::render::gl {
namespace {

using GlEnableFn = void (*)(unsigned int);
using GlDisableFn = void (*)(unsigned int);
using GlIsEnabledFn = unsigned char (*)(unsigned int);

constexpr unsigned int kGlDepthTest = 0x0B71;

GlEnableFn gEnable = nullptr;
GlDisableFn gDisable = nullptr;
GlIsEnabledFn gIsEnabled = nullptr;
bool gResolved = false;
bool gLoggedFailure = false;
std::mutex gMutex;

void resolve() {
    std::lock_guard lock(gMutex);
    if (gResolved) return;
    gResolved = true;

    void* library = dlopen("libGLESv2.so", RTLD_NOW | RTLD_NOLOAD);
    if (!library) library = dlopen("libGLESv2.so", RTLD_NOW);
    if (!library) {
        if (!gLoggedFailure) core::log::warn("GL: libGLESv2.so unavailable, see-through mode disabled");
        gLoggedFailure = true;
        return;
    }

    gEnable = reinterpret_cast<GlEnableFn>(dlsym(library, "glEnable"));
    gDisable = reinterpret_cast<GlDisableFn>(dlsym(library, "glDisable"));
    gIsEnabled = reinterpret_cast<GlIsEnabledFn>(dlsym(library, "glIsEnabled"));

    if (!gEnable || !gDisable || !gIsEnabled) {
        if (!gLoggedFailure) core::log::warn("GL: depth entry points missing, see-through mode disabled");
        gLoggedFailure = true;
        gEnable = nullptr;
        gDisable = nullptr;
        gIsEnabled = nullptr;
        return;
    }
    core::log::debug("GL: depth entry points resolved");
}

} // namespace

bool available() {
    resolve();
    return gEnable && gDisable && gIsEnabled;
}

ScopedDepthTestDisabled::ScopedDepthTestDisabled() {
    if (!available()) return;
    mWasEnabled = gIsEnabled(kGlDepthTest) != 0;
    if (mWasEnabled) {
        gDisable(kGlDepthTest);
        mChanged = true;
    }
}

ScopedDepthTestDisabled::~ScopedDepthTestDisabled() {
    if (!mChanged) return;
    if (mWasEnabled) gEnable(kGlDepthTest);
    mChanged = false;
}

} // namespace bes::render::gl
