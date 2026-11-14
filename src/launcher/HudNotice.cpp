#include "launcher/HudNotice.hpp"

#include <pl/ModMenu.hpp>

#include <array>
#include <utility>

namespace bes::launcher {
namespace {

/// Centred horizontally and placed above the hotbar rather than at the exact
/// centre of the screen, so it does not sit on the crosshair.
constexpr float kTextY = 0.78f;
constexpr float kTextSize = 1.0f;
/// Opaque white; the launcher applies its own shadow.
constexpr std::uint32_t kTextColor = 0xFFFFFFFFu;
constexpr const char* kFontId = "minecraft";

} // namespace

HudNotice& HudNotice::get() {
    static HudNotice instance;
    return instance;
}

void HudNotice::show(std::string text, std::uint64_t nowMs, std::uint64_t durationMs) {
    if (text.empty()) return;
    std::lock_guard lock(mMutex);
    mText = std::move(text);
    mExpiresAtMs = nowMs + durationMs;
    mVisible = true;
}

void HudNotice::clear() {
    std::lock_guard lock(mMutex);
    mText.clear();
    mExpiresAtMs = 0;
}

void HudNotice::poll(const std::string& moduleId, std::uint64_t nowMs) {
    std::string text;
    bool visible = false;
    {
        std::lock_guard lock(mMutex);
        if (!mText.empty() && nowMs < mExpiresAtMs) {
            text = mText;
            visible = true;
        } else if (mText.size()) {
            mText.clear();
        }
        // Only report a change, so an idle frame does not cost a bridge call.
        const bool wasVisible = mVisible;
        mVisible = visible;
        if (!visible && !wasVisible) return;
        if (!visible) {
            // Expired: send one empty list to take the old text down, then stop.
            pl::modmenu::submitDrawCommands(moduleId,
                                            std::span<const pl::modmenu::DrawCommand>{});
            return;
        }
    }

    // Position is resolved by the launcher from a centre-anchored x and a
    // fraction of screen height; DrawCommand carries both in x/y and the width
    // in w. A negative w is the launcher's own right-align convention, so a
    // centred label is expressed as x = 0 with w = -1 (see BedrockTools'
    // text-position handling for the same convention).
    std::array<pl::modmenu::DrawCommand, 1> commands{};
    auto& textCommand = commands[0];
    textCommand.type = pl::modmenu::DrawCommandType::Text;
    textCommand.x = 0.5f;
    textCommand.y = kTextY;
    textCommand.w = 1.0f;
    textCommand.h = 0.0f;
    textCommand.color = kTextColor;
    textCommand.size = kTextSize;
    textCommand.fontId = kFontId;
    textCommand.text = text;

    pl::modmenu::submitDrawCommands(moduleId,
                                    std::span<const pl::modmenu::DrawCommand>(commands));
}

} // namespace bes::launcher
