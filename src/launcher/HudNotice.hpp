#pragma once

#include <cstdint>
#include <mutex>
#include <string>

namespace bes::launcher {

/// A short-lived line of text drawn in the middle of the screen.
///
/// The mod's only user-facing feedback was the diagnostics file, and every
/// failure along the way -- no world yet, nothing under the crosshair, an offset
/// that has not proven itself -- ended in a log line nobody could see. Tapping
/// the Select button therefore looked like tapping a dead control, and the
/// natural conclusion was that the button was broken rather than that it had
/// nothing to work with.
///
/// Text is submitted through pl::modmenu::submitDrawCommands, the same overlay
/// channel BedrockTools' HUD modules use, and has to be re-submitted every frame:
/// the launcher's command list is per-frame state, so poll() is called from the
/// frame hook and re-sends whatever is still within its lifetime.
class HudNotice {
public:
    static HudNotice& get();

    /// Queue a message. Later calls replace the current one rather than stacking,
    /// because two of these on screen at once is noise, not information.
    void show(std::string text, std::uint64_t nowMs, std::uint64_t durationMs = 2600);

    /// Re-submit the visible message, or clear the overlay once it has expired.
    void poll(const std::string& moduleId, std::uint64_t nowMs);

    /// Drop the message immediately.
    void clear();

private:
    HudNotice() = default;

    mutable std::mutex mMutex;
    std::string mText;
    std::uint64_t mExpiresAtMs = 0;
    /// Whether the last frame submitted anything, so expiry submits one empty
    /// command list to erase the text instead of leaving it on screen forever.
    bool mVisible = false;
};

} // namespace bes::launcher
