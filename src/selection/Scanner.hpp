#pragma once

#include "selection/BlockCursor.hpp"
#include "selection/Selection.hpp"
#include "world/World.hpp"

#include <bes/sdk/Types.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace bes::selection {

/// Turns the current selection into concrete world positions/boxes.
///
/// Entity scanning is cheap (one actor-list walk) and runs every tick.
/// Block scanning has to sample the area around the player, so it is chunked:
/// `budget` bounds how many block lookups may happen in one call, and the scan
/// cursor persists between ticks to keep frame time flat.
class Scanner {
public:
    /// Runtime knobs pushed from the module config.
    struct Settings {
        bool scanBlocks = true;
        bool scanEntities = true;
        bool includePlayers = true;
        float radius = 40.f;       ///< horizontal radius in blocks
        float verticalRadius = 32.f;
        float entityRange = 160.f; ///< skip entity boxes further than this
    };

    static Scanner& get();

    void setSettings(const Settings& settings);
    Settings settings() const;

    /// Advances the scan for one tick. Returns true when the frame targets changed.
    bool update(const world::Snapshot& snapshot, std::uint64_t nowMs);

    /// Latest result for the render thread. Never blocks for long.
    FrameTargets targets() const;

    /// Drops cached block results (call when the selection or dimension changes).
    void reset();

    /// What the picker should show for the block under a position, if any.
    struct CursorHit {
        bool hasBlock = false;
        bes::sdk::BlockPos pos{};
        std::string blockId{};
    };

    /// Names the closest matching block within `radiusBlocks` of a position.
    /// Used by the "pin what I'm standing on" action, which needs no ray tracing.
    CursorHit nearestBlock(const world::Snapshot& snapshot, const bes::sdk::Vec3& around,
                           int radiusBlocks) const;

private:
    Scanner() = default;

    struct BlockEntry {
        bes::sdk::BlockPos pos{};
        double distanceSquared = 0.0;
    };

    void scanEntities(const world::Snapshot& snapshot, const Target& target, std::uint64_t nowMs);
    std::size_t scanBlocks(const world::Snapshot& snapshot, const Target& target, std::size_t budget);

    mutable std::mutex mMutex;
    FrameTargets mTargets{};
    std::vector<BlockEntry> mBlocks;
    BlockCursor mCursor;
    /// Dimension the cached block results belong to, or -1 when unknown.
    int mBlockDimension = -1;
    /// True once the current pass has been reported as finished (so a scan that
    /// matched nothing still counts as one state change, not zero).
    bool mProgressReported = false;
    bool mDirty = true;
    std::uint64_t mLastEntityScan = 0;
    Settings mSettings{};
    mutable std::mutex mSettingsMutex;
};

}
