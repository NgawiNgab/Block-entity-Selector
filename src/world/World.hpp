#pragma once

#include "core/Profile.hpp"

#include <bes/sdk/Types.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace bes::world {

/// One live actor snapshot. Pointers are only valid for the frame they were read.
struct ActorEntry {
    void* actor = nullptr;
    std::string className;   ///< "Zombie", "Player", "ItemActor", ...
    std::string displayName; ///< nametag when present, otherwise className
    bes::sdk::AABB bounds{};
    bes::sdk::Vec3 position{};
    bool isPlayer = false;
};

/// One block position that matched the filter.
struct BlockHit {
    bes::sdk::BlockPos pos{};
    bes::sdk::AABB bounds{};
    std::string name;
};

/// Per-frame game state, refreshed from the tick hook.
struct Snapshot {
    void* level = nullptr;
    void* dimension = nullptr;
    void* blockSource = nullptr;
    void* localPlayer = nullptr;
    bes::sdk::Vec3 playerPosition{};
    bes::sdk::Vec3 cameraPosition{};
    bes::sdk::Vec3 lookDirection{0.f, 0.f, -1.f};
    int dimensionId = 0;
    int minHeight = -64;
    int maxHeight = 320;
    bool valid = false;
};

/// Reads the game through the resolved offsets. Every accessor probes candidate
/// offsets on first use, so a wrong profile entry cannot corrupt the game or
/// crash the render thread.
class World {
public:
    static World& get();

    /// Refreshes the cached snapshot. Called once per local player tick.
    void update(void* localPlayer, const bes::sdk::Vec3& cameraPosition = {});

    const Snapshot& snapshot() const noexcept { return mSnapshot; }

    /// Enumerates every loaded actor through the actor manager list.
    std::vector<ActorEntry> actors();

    /// Exact block-name lookup for a filtered set of positions.
    std::string blockName(void* blockSource, const bes::sdk::BlockPos& pos);

    /// Raw `Block*` at a position, or null when the lookup is unresolved.
    /// Exposed for diagnostics: the probe needs the object, not its name.
    void* blockAt(void* blockSource, const bes::sdk::BlockPos& pos);

    /// `BlockType*` for a live `Block*`.
    ///
    /// UNKNOWN offset: static RE could not pin which member of `Block` holds the
    /// `BlockType` pointer. Rather than assert a guess, this probes the candidate
    /// offsets the name path already trusts and returns the first one whose
    /// target has a readable vtable. Returns null when none qualifies.
    void* blockTypeOf(const void* block);

    /// Confirms offsets that need a live object (called lazily, once).
    void probeOffsets(void* localPlayer);

    /// Raw block source pointer for the current dimension.
    void* currentBlockSource();

    /// Binds the lazily-resolved function pointers once the profile is loaded.
    void installBlockLookup();

    /// True when the entity pipeline has everything it needs.
    bool entityPipelineReady() const;
    /// True when the block pipeline has everything it needs.
    bool blockPipelineReady() const;

private:
    World() = default;

    void* readCamerasAndLevel(void* levelRenderer);

    Snapshot mSnapshot{};
    bool mProbedStateVector = false;
    bool mProbedAabb = false;
};

}
