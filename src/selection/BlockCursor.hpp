#pragma once

#include <bes/sdk/Types.hpp>

#include <algorithm>
#include <cstdint>

namespace bes::selection {

/// Walks every block position in a box around the player, one cell per call, so
/// a wide scan can be spread over many ticks without any allocation or state
/// beyond what is in here.
///
/// The walk visits Y fastest: a full vertical column is finished before the next
/// horizontal cell. That keeps partially-scanned results visually coherent (whole
/// columns light up at once instead of a scattered cloud) and is friendlier to the
/// game's block cache.
class BlockCursor {
public:
    /// Restarts the walk. `verticalRadius` is in blocks above and below `center`.
    void reset(int centerX, int centerY, int centerZ, int horizontalRadius, int verticalRadius, int minHeight,
               int maxHeight) {
        const int radius = std::max(horizontalRadius, 0);
        mMinX = centerX - radius;
        mMaxX = centerX + radius;
        mMinZ = centerZ - radius;
        mMaxZ = centerZ + radius;

        // The vertical range is clamped to the dimension's build limits, so a
        // scan cannot walk into positions the level will refuse to answer for.
        mMinY = std::max(centerY - std::max(verticalRadius, 0), minHeight);
        mMaxY = std::min(centerY + std::max(verticalRadius, 0), maxHeight);

        mPosition = {mMinX, mMinY, mMinZ};
        mActive = mMinY < mMaxY && mMinX <= mMaxX && mMinZ <= mMaxZ;
    }

    /// True while there are cells left to visit.
    bool active() const noexcept { return mActive; }

    /// Abandons the walk; the next `reset` starts a fresh pass.
    void invalidate() noexcept { mActive = false; }

    /// Cell to inspect next. Only valid while `active()`.
    const sdk::BlockPos& position() const noexcept { return mPosition; }

    /// Steps to the next cell, clearing `active()` once the walk wraps.
    void advance() noexcept {
        if (!mActive) return;

        if (++mPosition.y < mMaxY) return;
        mPosition.y = mMinY;

        if (++mPosition.z <= mMaxZ) return;
        mPosition.z = mMinZ;

        if (++mPosition.x > mMaxX) {
            mActive = false;
            return;
        }
    }

    /// Number of cells a full pass visits, for progress reporting and tests.
    std::int64_t cellCount() const noexcept {
        const std::int64_t width = static_cast<std::int64_t>(mMaxX) - mMinX + 1;
        const std::int64_t depth = static_cast<std::int64_t>(mMaxZ) - mMinZ + 1;
        const std::int64_t height = static_cast<std::int64_t>(mMaxY) - mMinY;
        if (width <= 0 || depth <= 0 || height <= 0) return 0;
        return width * depth * height;
    }

private:
    sdk::BlockPos mPosition{};
    int mMinX = 0, mMaxX = -1;
    int mMinY = 0, mMaxY = 0;
    int mMinZ = 0, mMaxZ = -1;
    bool mActive = false;
};

} // namespace bes::selection
