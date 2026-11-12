#pragma once

namespace bes::render {

/// Minimal GLES state access, resolved lazily through libGLESv2.so.
///
/// The mod must never assume it can include GL headers in every translation
/// unit, so the handful of entry points needed for the depth-test override are
/// dlsym'd once and cached.
namespace gl {

bool available();

/// RAII scope that disables GL_DEPTH_TEST so the highlight draws through
/// terrain, restoring the previous state on exit. No-op when GL is unreachable.
class ScopedDepthTestDisabled {
public:
    ScopedDepthTestDisabled();
    ~ScopedDepthTestDisabled();

    ScopedDepthTestDisabled(const ScopedDepthTestDisabled&) = delete;
    ScopedDepthTestDisabled& operator=(const ScopedDepthTestDisabled&) = delete;

    /// False when the scope could not change state (GL not resolvable).
    explicit operator bool() const noexcept { return mChanged; }

private:
    bool mChanged = false;
    bool mWasEnabled = true;
};

} // namespace gl

}
