#include "selection/Scanner.hpp"

namespace bes::selection {

// The parts of Scanner that touch nothing but its own state. Split out of
// Scanner.cpp so the host unit tests can link them without the game-facing
// parts (World, Resolver, RTTI) that only exist inside the injected module.

Scanner& Scanner::get() {
    static Scanner instance;
    return instance;
}

Scanner::Settings Scanner::settings() const {
    std::lock_guard lock(mSettingsMutex);
    return mSettings;
}

void Scanner::setSettings(const Settings& settings) {
    {
        std::lock_guard lock(mSettingsMutex);
        if (settings.scanBlocks == mSettings.scanBlocks && settings.scanEntities == mSettings.scanEntities &&
            settings.includePlayers == mSettings.includePlayers &&
            settings.radius == mSettings.radius && settings.verticalRadius == mSettings.verticalRadius &&
            settings.entityRange == mSettings.entityRange) {
            return;
        }
        mSettings = settings;
    }
    // A radius change invalidates the walk cursor so the next pass starts clean.
    // The dimension stays recorded: results are re-collected in the same world.
    std::lock_guard lock(mMutex);
    mCursor.invalidate();
    mProgressReported = false;
    mBlocks.clear();
    mTargets.blocks.clear();
    mDirty = true;
}

void Scanner::reset() {
    std::lock_guard lock(mMutex);
    mBlocks.clear();
    mTargets.blocks.clear();
    mTargets.entityBoxes.clear();
    mTargets.entityIsPlayer.clear();
    mCursor.invalidate();
    mBlockDimension = -1;
    mProgressReported = false;
    mDirty = true;
}

FrameTargets Scanner::targets() const {
    std::lock_guard lock(mMutex);
    return mTargets;
}

} // namespace bes::selection
