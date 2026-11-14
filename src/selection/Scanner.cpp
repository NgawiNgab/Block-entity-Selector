#include "selection/Scanner.hpp"

#include "core/Logging.hpp"
#include "core/Resolver.hpp"
#include "selection/Identifier.hpp"

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
            mCursor.invalidate();
        }
        return hadAnything;
    }

    // Track the dimension the current block cache belongs to. Comparing against
    // a value stored here (not against the selection) is what makes this fire
    // exactly once per dimension change; the first pass only records it.
    {
        std::lock_guard lock(mMutex);
        if (mBlockDimension < 0) {
            mBlockDimension = snapshot.dimensionId;
        } else if (mBlockDimension != snapshot.dimensionId) {
            mBlockDimension = snapshot.dimensionId;
            mBlocks.clear();
            mTargets.blocks.clear();
            mCursor.invalidate();
            mProgressReported = false;
            mDirty = true;
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
    bool dirty = false;
    {
        std::lock_guard lock(mMutex);
        dirty = mDirty;
    }
    return added > 0 || hadEntities || dirty;
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
    // entityPipelineReady() only reports whether the actor-list signature resolved,
    // which it does. The real gate is Level::mActorManager being proven, and
    // World::actors() returns nothing until it is.
    if (!world.entityPipelineReady()) return;
    const auto actors = world.actors();

    std::vector<bes::sdk::AABB> boxes;
    std::vector<char> playerFlags;
    std::vector<std::string> classes;
    classes.reserve(actors.size());

    const auto origin = snapshot.playerPosition;
    const float maxDistance = settings.entityRange;
    for (const auto& actor : actors) {
        if (!entityMatches(target, actor.className)) continue;
        // The local player is included on purpose: the highlight is world
        // geometry, so their own box makes sense in third person.
        if (actor.isPlayer && !settings.includePlayers) continue;
        if (!actor.bounds.valid()) continue;
        if (actor.bounds.distanceSquaredTo(origin) > maxDistance * maxDistance) continue;

        boxes.push_back(actor.bounds);
        playerFlags.push_back(actor.isPlayer ? 1 : 0);

        // A class that matched and has a valid hitbox is proven to exist in this
        // build. Bind it so `minecraft:zombie` keeps working across sessions even
        // if the documented table is later trimmed.
        if (target.kind == Kind::Entity && target.confidence < identifier::Confidence::ObservedLive) {
            identifier::ClassIndex::get().bind(identifier::Domain::Entity, target.id, actor.className,
                                               identifier::Confidence::ObservedLive,
                                               "observed as a live actor");
        }

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
    // Note what is NOT checked here: whether block_source_get_block resolved.
    // That signature is one of the 13 that do resolve, so a "signature missing"
    // warning would be false. Block scanning is gated on the block source pointer,
    // which only exists once Actor::mLevel, Actor::mDimension and
    // Dimension::mBlockSource have been PROVEN on a live object. Until then there
    // is genuinely nothing to scan, and the diagnostics dump reports which of the
    // two reasons applies ("offsets proven: n/11").
    void* blockSource = world.currentBlockSource();
    if (!blockSource) return 0;

    const int radius = std::clamp(static_cast<int>(settings.radius), 4, 128);
    const int vertical = std::clamp(static_cast<int>(settings.verticalRadius), 4, 128);

    const int centerX = static_cast<int>(std::floor(snapshot.playerPosition.x));
    const int centerY = static_cast<int>(std::floor(snapshot.playerPosition.y));
    const int centerZ = static_cast<int>(std::floor(snapshot.playerPosition.z));

    if (!mCursor.active()) {
        mCursor.reset(centerX, centerY, centerZ, radius, vertical, snapshot.minHeight, snapshot.maxHeight);
        // A build-limit clamp can make the box empty; report rather than spin.
        if (!mCursor.active()) return 0;
    }

    std::size_t lookups = 0;
    std::size_t newMatches = 0;
    std::vector<BlockEntry> found;

    while (lookups < budget && mCursor.active()) {
        const sdk::BlockPos pos = mCursor.position();
        const std::string name = world.blockName(blockSource, pos);
        ++lookups;

        if (!name.empty() && name == target.id) {
            // Seeing the identifier on a real block is evidence, and it is the
            // only evidence the mod can produce on its own. Recorded once per
            // session so the confidence label can move off "unverified".
            // Re-asserted on every hit (the index ignores non-upgrades), so this
            // cannot latch on the wrong identifier if the player re-selects.
            identifier::ClassIndex::get().note(identifier::Domain::Block, target.id,
                                               identifier::Confidence::ObservedLive,
                                               "observed on a live block in this session");
            const float dx = static_cast<float>(pos.x) + 0.5f - snapshot.playerPosition.x;
            const float dy = static_cast<float>(pos.y) + 0.5f - snapshot.playerPosition.y;
            const float dz = static_cast<float>(pos.z) + 0.5f - snapshot.playerPosition.z;
            found.push_back({pos, static_cast<double>(dx * dx + dy * dy + dz * dz)});
            ++newMatches;
        }

        mCursor.advance();
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
            mTargets.anchor = sdk::BlockPos::fromCorner(snapshot.playerPosition);
            mTargets.hasAnchor = true;
            mDirty = false;
        }
        // Progress reporting: a finished pass is a state change even when it
        // matched nothing, and so is learning a single new match.
        if (!mCursor.active() && !mProgressReported) {
            mProgressReported = true;
            mDirty = true;
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
