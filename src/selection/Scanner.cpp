#include "selection/Scanner.hpp"

#include "core/Logging.hpp"
#include "core/Resolver.hpp"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <sstream>

namespace bes::selection {
namespace {

/// How many block lookups per tick. Each lookup is one native call.
constexpr std::size_t kBlockLookupBudget = 512;
/// Results closer than this are rendered; the rest are dropped so a wide area
/// scan cannot drown the frame.
constexpr std::size_t kMaxBlockTargets = 4096;
}

Scanner& Scanner::get() {
    static Scanner instance;
    return instance;
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
    std::lock_guard lock(mMutex);
    mCursorValid = false;
    mBlocks.clear();
    mTargets.blocks.clear();
    mDirty = true;
}

Scanner::Settings Scanner::settings() const {
    std::lock_guard lock(mSettingsMutex);
    return mSettings;
}

void Scanner::reset() {
    std::lock_guard lock(mMutex);
    mBlocks.clear();
    mTargets.blocks.clear();
    mTargets.entityBoxes.clear();
    mTargets.entityIsPlayer.clear();
    mCursorValid = false;
    mDirty = true;
}

FrameTargets Scanner::targets() const {
    std::lock_guard lock(mMutex);
    return mTargets;
}

bool Scanner::update(const world::Snapshot& snapshot, std::uint64_t nowMs) {
    auto& selectionStore = Selection::get();
    const auto target = selectionStore.current();
    if (!target || !snapshot.valid) {
        bool hadAnything = false;
        {
            std::lock_guard lock(mMutex);
            hadAnything = !mTargets.blocks.empty() || !mTargets.entityBoxes.empty();
            mTargets.blocks.clear();
            mTargets.entityBoxes.clear();
            mTargets.entityIsPlayer.clear();
            mBlocks.clear();
            mCursorValid = false;
        }
        return hadAnything;
    }

    // Dimension changes invalidate cached block results.
    {
        std::lock_guard lock(mMutex);
        if (snapshot.dimensionId != 0 && target.dimensionId != snapshot.dimensionId) {
            mBlocks.clear();
            mCursorValid = false;
        }
    }

    if (target.kind == Kind::Entity) {
        scanEntities(snapshot, target, nowMs);
        bool hadBlocks = false;
        {
            std::lock_guard lock(mMutex);
            hadBlocks = !mTargets.blocks.empty();
            mTargets.blocks.clear();
            mBlocks.clear();
        }
        return hadBlocks;
    }

    bool hadEntities = false;
    {
        std::lock_guard lock(mMutex);
        hadEntities = !mTargets.entityBoxes.empty();
        mTargets.entityBoxes.clear();
        mTargets.entityIsPlayer.clear();
    }

    const auto added = scanBlocks(snapshot, target, kBlockLookupBudget);
    return added > 0 || hadEntities || mDirty;
}

void Scanner::scanEntities(const world::Snapshot& snapshot, const Target& target, std::uint64_t nowMs) {
    // Entity scans are cheap; throttle to ~10 Hz so the actor list is not walked
    // on every single tick.
    if (nowMs - mLastEntityScan < 100) return;
    mLastEntityScan = nowMs;

    const auto settings = this->settings();
    if (!settings.scanEntities) {
        Selection::get().setMatchedClasses({});
        std::lock_guard lock(mMutex);
        mTargets.entityBoxes.clear();
        mTargets.entityIsPlayer.clear();
        return;
    }

    auto& world = world::World::get();
    if (!world.entityPipelineReady()) {
        // Only report once per session; the diagnostic dump carries the detail.
        static bool warned = false;
        if (!warned) {
            warned = true;
            bes::core::log::warn("scanner: actor_manager_list unresolved, entity highlighting disabled");
        }
        return;
    }
    const auto actors = world.actors();

    std::vector<bes::sdk::AABB> boxes;
    std::vector<char> playerFlags;
    std::vector<std::string> classes;
    classes.reserve(actors.size());

    const auto origin = snapshot.playerPosition;
    const float maxDistance = settings.entityRange;
    for (const auto& actor : actors) {
        if (!entityMatches(target, actor.className)) continue;
        if (actor.isPlayer && actor.actor == snapshot.localPlayer) {
            // Highlight the player too when they selected a player class; the
            // renderer draws world geometry, which includes the local player's
            // own box in third person.
        }
        if (actor.isPlayer && !settings.includePlayers) continue;
        if (!actor.bounds.valid()) continue;
        if (actor.bounds.distanceSquaredTo(origin) > maxDistance * maxDistance) continue;

        boxes.push_back(actor.bounds);
        playerFlags.push_back(actor.isPlayer ? 1 : 0);

        if (std::find(classes.begin(), classes.end(), actor.className) == classes.end()) {
            classes.push_back(actor.className);
        }
    }

    Selection::get().setMatchedClasses(classes);

    {
        std::lock_guard lock(mMutex);
        mTargets.entityBoxes = std::move(boxes);
        mTargets.entityIsPlayer = std::move(playerFlags);
        mTargets.anchor = bes::sdk::BlockPos::fromCorner(origin);
        mTargets.hasAnchor = true;
        mDirty = false;
    }
}

std::size_t Scanner::scanBlocks(const world::Snapshot& snapshot, const Target& target, std::size_t budget) {
    const auto settings = this->settings();
    if (!settings.scanBlocks) return 0;

    auto& world = world::World::get();
    if (!world.blockPipelineReady()) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            bes::core::log::warn("scanner: block_source_get_block unresolved, block highlighting disabled");
        }
        return 0;
    }
    void* blockSource = world.currentBlockSource();
    if (!blockSource) return 0;

    const int radius = std::clamp(static_cast<int>(settings.radius), 4, 128);
    const int vertical = std::clamp(static_cast<int>(settings.verticalRadius), 4, 128);

    const int centerX = static_cast<int>(std::floor(snapshot.playerPosition.x));
    const int centerY = static_cast<int>(std::floor(snapshot.playerPosition.y));
    const int centerZ = static_cast<int>(std::floor(snapshot.playerPosition.z));

    const int minY = std::max(centerY - vertical, snapshot.minHeight);
    const int maxY = std::min(centerY + vertical, snapshot.maxHeight);

    std::size_t lookups = 0;
    std::size_t newMatches = 0;
    std::vector<BlockEntry> found;

    while (lookups < budget) {
        if (!mCursorValid) {
            mCursor = {centerX - radius, minY, centerZ - radius};
            mCursorValid = true;
        }

        // One lookup per iteration; the cursor walks Y fastest so a vertical
        // column is finished before moving to the next XZ cell (cache friendly
        // for the game and keeps partial results visually coherent).
        bes::sdk::BlockPos pos = mCursor;
        const std::string name = world.blockName(blockSource, pos);
        ++lookups;

        if (!name.empty() && name == target.id) {
            const float dx = static_cast<float>(pos.x) + 0.5f - snapshot.playerPosition.x;
            const float dy = static_cast<float>(pos.y) + 0.5f - snapshot.playerPosition.y;
            const float dz = static_cast<float>(pos.z) + 0.5f - snapshot.playerPosition.z;
            found.push_back({pos, static_cast<double>(dx * dx + dy * dy + dz * dz)});
            ++newMatches;
        }

        // Advance cursor.
        if (++mCursor.y > maxY - 1) {
            mCursor.y = minY;
            if (++mCursor.z > centerZ + radius) {
                mCursor.z = centerZ - radius;
                if (++mCursor.x > centerX + radius) {
                    mCursorValid = false; // full pass done
                    break;
                }
            }
        }
    }

    {
        std::lock_guard lock(mMutex);
        if (!found.empty()) {
            for (auto& entry : found) {
                mBlocks.push_back(entry);
            }
            if (mBlocks.size() > kMaxBlockTargets) {
                // Keep the nearest ones; the far tail is what players ignore anyway.
                std::sort(mBlocks.begin(), mBlocks.end(), [](const BlockEntry& left, const BlockEntry& right) {
                    return left.distanceSquared < right.distanceSquared;
                });
                mBlocks.resize(kMaxBlockTargets);
            }
            mTargets.blocks.clear();
            mTargets.blocks.reserve(mBlocks.size());
            for (const auto& entry : mBlocks) mTargets.blocks.push_back(entry.pos);
            mTargets.anchor = bes::sdk::BlockPos::fromCorner(snapshot.playerPosition);
            mTargets.hasAnchor = true;
            mDirty = false;
        }
    }

    return newMatches;
}

Scanner::CursorHit Scanner::nearestBlock(const world::Snapshot& snapshot, const bes::sdk::Vec3& around,
                                        int radiusBlocks) const {
    CursorHit hit;
    auto& world = world::World::get();
    void* blockSource = world.currentBlockSource();
    if (!blockSource) return hit;

    const int centerX = static_cast<int>(std::floor(around.x));
    const int centerY = static_cast<int>(std::floor(around.y));
    const int centerZ = static_cast<int>(std::floor(around.z));

    double bestDistance = 0.0;
    for (int dy = -radiusBlocks; dy <= radiusBlocks; ++dy) {
        for (int dx = -radiusBlocks; dx <= radiusBlocks; ++dx) {
            for (int dz = -radiusBlocks; dz <= radiusBlocks; ++dz) {
                const bes::sdk::BlockPos pos{centerX + dx, centerY + dy, centerZ + dz};
                const std::string name = world.blockName(blockSource, pos);
                if (name.empty() || name == "minecraft:air") continue;
                const double distance = static_cast<double>(dx * dx + dy * dy + dz * dz);
                if (!hit.hasBlock || distance < bestDistance) {
                    bestDistance = distance;
                    hit.hasBlock = true;
                    hit.pos = pos;
                    hit.blockId = name;
                }
            }
        }
    }
    return hit;
}

} // namespace bes::selection
