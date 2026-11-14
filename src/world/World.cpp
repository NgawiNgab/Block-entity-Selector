#include "world/World.hpp"

#include "core/Logging.hpp"
#include "core/Resolver.hpp"
#include "core/Rtti.hpp"

#include <bes/sdk/AddressSpace.hpp>
#include <bes/sdk/LibcxxString.hpp>
#include <bes/sdk/Memory.hpp>

#include <algorithm>
#include <cstring>
#include <cmath>
#include <sstream>

namespace bes::world {
namespace {

using namespace bes::core;
using bes::sdk::Vec3;

using GetRuntimeActorListFn = std::vector<void*> (*)(void*);
using BlockSourceGetBlockFn = const void* (*)(void*, const bes::sdk::BlockPos*, int);

GetRuntimeActorListFn gGetActorList = nullptr;
BlockSourceGetBlockFn gGetBlock = nullptr;

/// Block name layout: Block -> BlockType -> NameInfo -> HashedString -> std::string.
/// These are tolerant probes rather than fixed magic numbers; a failed probe
/// simply reports "unknown" and the filter falls back to positions only.
constexpr std::size_t kBlockTypeOffsetCandidates[] = {0x68, 0x70, 0x60, 0x58};
constexpr std::size_t kBlockTypeNameInfoCandidates[] = {0x88, 0x90, 0x80, 0x98};
constexpr std::size_t kNameInfoFullNameCandidates[] = {0x40, 0x38, 0x48, 0x30};
constexpr std::size_t kHashedStringValueCandidates[] = {0x8, 0x10, 0x0};

/// Actor rotation component, read as (pitch, yaw) in degrees.
///
/// Not a profile entry, because nothing has ever proven it: it is a literal that
/// was already in this file. Keeping it named makes it obvious that it has no
/// validation path beyond the mapping check at its use site.
constexpr std::size_t kActorRotationOffset = 0x238;

bool looksLikeIdentifier(const std::string& text) {
    if (text.size() < 2 || text.size() > 64) return false;
    if (text.find(':') == std::string::npos && text.find('_') == std::string::npos) return false;
    for (const char character : text) {
        const auto value = static_cast<unsigned char>(character);
        if (!(std::islower(value) || std::isdigit(value) || character == ':' || character == '_' ||
              character == '.' || character == '/')) {
            return false;
        }
    }
    return true;
}

/// Reads a std::string at an address only if it looks like a namespace-qualified id.
///
/// The size is NOT the raw first byte. libc++ selects between two forms with the
/// low bit: small-string stores `size << 1` in byte 0, long-string stores
/// `(size << 1) | 1` and keeps {size, pointer} at +8. Reading byte 0 as the size
/// happens to work for a few short names and silently mangles everything else,
/// which is the worst possible failure for a block identifier -- "stone" would
/// come back as a half-length prefix rather than as an error. sdk::LibcxxString
/// already implements this correctly, so the duplicate is gone.
std::string readIdentifier(std::uintptr_t address) {
    if (address < 0x1000) return {};
    auto& space = sdk::AddressSpace::get();
    if (space.regionCount() == 0) space.refresh();
    // The caller may hand this any candidate offset, so the address itself is
    // checked before the reader dereferences it.
    if (!space.readable(reinterpret_cast<const void*>(address), 24)) return {};
    const auto candidate = sdk::readLibcxxString(reinterpret_cast<const void*>(address), 64,
                                                 [&space](std::uintptr_t value) {
                                                     return space.readable(
                                                         reinterpret_cast<const void*>(value), 1);
                                                 });
    if (looksLikeIdentifier(candidate)) return candidate;
    return {};
}

std::string blockNameFromBlock(const void* block) {
    if (!block) return {};
    for (const auto blockTypeOffset : kBlockTypeOffsetCandidates) {
        const auto blockType = sdk::fieldPointer(block, blockTypeOffset);
        if (!sdk::plausiblePointer(blockType)) continue;
        for (const auto nameInfoOffset : kBlockTypeNameInfoCandidates) {
            const auto nameInfo = reinterpret_cast<std::uintptr_t>(blockType) + nameInfoOffset;
            for (const auto fullNameOffset : kNameInfoFullNameCandidates) {
                for (const auto valueOffset : kHashedStringValueCandidates) {
                    const auto name = readIdentifier(
                        *reinterpret_cast<const std::uintptr_t*>(nameInfo + fullNameOffset + valueOffset));
                    if (!name.empty()) return name;
                }
            }
        }
    }
    return {};
}

bool plausibleActor(void* candidate) {
    return sdk::plausiblePointer(candidate);
}

/// Loads the memory map if it has not been loaded yet.
///
/// Every hand-written range check in this file goes through `AddressSpace`, which
/// fails closed while the map is absent — so the map has to be loaded before the
/// first check, not after it.
void ensureMappingLoaded(sdk::AddressSpace& space) {
    if (space.regionCount() == 0) space.refresh();
}

} // namespace

World& World::get() {
    static World instance;
    return instance;
}

Snapshot World::snapshot() const {
    std::lock_guard lock(mMutex);
    return mSnapshot;
}

void World::update(void* localPlayer, const Vec3& cameraPosition) {
    // Held for the whole refresh: the probes read and write both the snapshot and
    // the mProbed* flags, and a second thread entering mid-probe would see a
    // half-written state and then dereference a candidate the first thread had
    // already rejected.
    std::lock_guard lock(mMutex);

    mSnapshot.localPlayer = localPlayer;
    mSnapshot.cameraPosition = cameraPosition;
    if (!localPlayer) {
        mSnapshot.valid = false;
        return;
    }

    auto& resolver = Resolver::get();
    auto& space = sdk::AddressSpace::get();
    ensureMappingLoaded(space);

    // Probing has to run before anything reads through the offsets, otherwise a
    // rejected candidate would be used once and poison the snapshot.
    probeOffsets(localPlayer);

    // Nothing below may dereference an offset that no live object has confirmed.
    // `offset()` returns 0 for an unproven category, and reading through 0 is
    // still a fault, so the requirement is stated explicitly instead of relying
    // on a null check after the fact.
    if (!resolver.offsetValidated(OffId::ActorLevel) ||
        !resolver.offsetValidated(OffId::ActorDimension)) {
        mSnapshot.level = nullptr;
        mSnapshot.dimension = nullptr;
        mSnapshot.blockSource = nullptr;
        mSnapshot.valid = false;
        return;
    }

    mSnapshot.level = sdk::fieldPointer(localPlayer, resolver.offset(OffId::ActorLevel));
    mSnapshot.dimension = sdk::fieldPointer(localPlayer, resolver.offset(OffId::ActorDimension));

    if (!mSnapshot.level || !sdk::plausiblePointer(mSnapshot.level)) {
        mSnapshot.level = nullptr;
        mSnapshot.valid = false;
        return;
    }

    // Player position comes from the state-vector probe above, so a rejected
    // offset cannot leave the snapshot with a garbage position.

    // Dimension -> BlockSource candidate offsets from the profile, validated by
    // the fact that the block source must expose a readable dimension id.
    mSnapshot.blockSource = nullptr;
    if (sdk::plausiblePointer(mSnapshot.dimension)) {
        const auto& candidates = Profile::get().candidates(OffId::DimensionBlockSource);
        for (const auto& candidate : candidates) {
            if (!space.readable(mSnapshot.dimension, candidate.value + sizeof(void*))) continue;
            const auto blockSource = sdk::fieldPointer(mSnapshot.dimension, candidate.value);
            if (!sdk::plausiblePointer(blockSource)) continue;
            // A vtable read and a virtual call are both dereferences of
            // memory we did not allocate. Confirming the mapping first is what
            // separates "this candidate is wrong" from a SIGSEGV.
            if (!space.readable(blockSource, sizeof(void*))) continue;
            void* const vtable = *reinterpret_cast<void* const*>(blockSource);
            if (!sdk::plausiblePointer(vtable) || !space.readable(vtable, 19 * sizeof(void*))) continue;
            const auto slot = static_cast<void* const*>(vtable)[18];
            if (!slot) continue;
            const auto dimensionId = sdk::virtualCall<int>(blockSource, 18);
            if (dimensionId < -1 || dimensionId > 8) continue;
            resolver.confirmOffset(OffId::DimensionBlockSource, candidate.value, candidate.origin);
            mSnapshot.blockSource = blockSource;
            break;
        }
        // Dimension identity from RTTI ("Overworld"/"Nether"/"TheEnd") rather
        // than a raw field offset, which survives layout shuffles.
        const auto dimensionClass = bes::core::rtti::className(mSnapshot.dimension);
        if (dimensionClass.find("Nether") != std::string::npos) mSnapshot.dimensionId = 1;
        else if (dimensionClass.find("TheEnd") != std::string::npos ||
                 dimensionClass.find("End") != std::string::npos) mSnapshot.dimensionId = 2;
        else if (!dimensionClass.empty()) mSnapshot.dimensionId = 0;
    }

    mSnapshot.valid = true;

    if (!cameraPosition.finite() || cameraPosition.lengthSquared() < 1.f) {
        // First person fallback: eye height above the player's feet.
        mSnapshot.cameraPosition = {mSnapshot.playerPosition.x, mSnapshot.playerPosition.y + 1.62f,
                                    mSnapshot.playerPosition.z};
    }

    // Look direction from the player rotation component (yaw, pitch in degrees).
    //
    // 0x238 is a hardcoded literal, not a profile entry, so it has no proof path
    // and cannot be allowed to dereference blind -- it was the last unchecked read
    // in this function. Require the mapping, then require the values to be a
    // plausible rotation before trusting them.
    void* rotationComponent = nullptr;
    if (space.readable(localPlayer, kActorRotationOffset + sizeof(void*))) {
        const auto raw = sdk::fieldPointer(localPlayer, kActorRotationOffset);
        if (sdk::plausiblePointer(raw) && space.readable(raw, sizeof(bes::sdk::Vec2))) {
            rotationComponent = raw;
        }
    }
    if (rotationComponent) {
        const auto* rotation = reinterpret_cast<const bes::sdk::Vec2*>(rotationComponent);
        if (std::isfinite(rotation->x) && std::isfinite(rotation->y) && std::fabs(rotation->x) <= 90.5f) {
            constexpr float kDegToRad = 3.14159265358979323846f / 180.f;
            const float yaw = rotation->y * kDegToRad;
            const float pitch = rotation->x * kDegToRad;
            mSnapshot.lookDirection = {-std::sin(yaw) * std::cos(pitch), -std::sin(pitch),
                                       std::cos(yaw) * std::cos(pitch)};
        }
    }

}

void World::probeOffsets(void* localPlayer) {
    auto& resolver = Resolver::get();
    if (!localPlayer) return;

    auto& space = sdk::AddressSpace::get();
    ensureMappingLoaded(space);
    if (space.regionCount() == 0) return;

    // Every read below goes through the map first. These candidates are
    // hypotheses; a wrong one has to fail the check rather than fault the game.
    // The flag is set only on success, so a probe that fails while the world is
    // still loading is retried on the next frame instead of being abandoned.
    if (!resolver.offsetValidated(OffId::ActorStateVector) && !mProbedStateVector) {
        for (const auto& candidate : Profile::get().candidates(OffId::ActorStateVector)) {
            if (!space.readable(localPlayer, candidate.value + sizeof(void*))) continue;
            const auto pointer = sdk::fieldPointer(localPlayer, candidate.value);
            if (!sdk::plausiblePointer(pointer) || !space.readable(pointer, sizeof(Vec3))) continue;
            const auto* position = reinterpret_cast<const Vec3*>(pointer);
            if (!position->finite()) continue;
            if (std::fabs(position->y) > 4000.f) continue;
            resolver.confirmOffset(OffId::ActorStateVector, candidate.value, candidate.origin);
            mSnapshot.playerPosition = *position;
            mProbedStateVector = true;
            break;
        }
    }

    if (!resolver.offsetValidated(OffId::ActorAabbComponent) && !mProbedAabb) {
        for (const auto& candidate : Profile::get().candidates(OffId::ActorAabbComponent)) {
            if (!space.readable(localPlayer, candidate.value + sizeof(void*))) continue;
            const auto component = sdk::fieldPointer(localPlayer, candidate.value);
            if (!sdk::plausiblePointer(component) || !space.readable(component, sizeof(sdk::AABB))) continue;
            const auto* bounds = reinterpret_cast<const sdk::AABB*>(component);
            if (!bounds->valid()) continue;
            // A real hitbox is small and sits on the player: anything taller than
            // a few blocks or far from the player's feet is some other struct.
            const auto extent = bounds->extent();
            if (!extent.finite() || extent.y < 0.5f || extent.y > 8.f) continue;
            if (extent.x < 0.05f || extent.z < 0.05f || extent.x > 8.f || extent.z > 8.f) continue;
            const auto center = bounds->center();
            if ((center - mSnapshot.playerPosition).length() > 16.f) continue;
            resolver.confirmOffset(OffId::ActorAabbComponent, candidate.value, candidate.origin);
            mProbedAabb = true;
            break;
        }
    }

    probeActorLevelAndDimension(localPlayer);
}

/// Proves `Actor::mLevel` and `Actor::mDimension` before either is dereferenced.
///
/// Both are confirmed by RTTI rather than by a value heuristic, because a class
/// name survives a layout shuffle and "the pointer is 8-byte aligned" does not.
/// An unproven offset blocks the whole snapshot: that turns a SIGSEGV inside game
/// code into a mod that does nothing, which is the only acceptable failure here.
void World::probeActorLevelAndDimension(void* localPlayer) {
    auto& resolver = Resolver::get();
    auto& space = sdk::AddressSpace::get();
    ensureMappingLoaded(space);
    if (space.regionCount() == 0) return;

    // A pointer field can only be read if the field itself lies inside a mapped
    // region, and the object it points at must have a readable vptr: every real
    // polymorphic object on this ABI starts with one.
    const auto readableObject = [&space](void* pointer) {
        if (!sdk::plausiblePointer(pointer)) return false;
        if (!space.readable(pointer, sizeof(void*))) return false;
        void* vtable = nullptr;
        std::memcpy(&vtable, pointer, sizeof(vtable));
        return sdk::plausiblePointer(vtable) && space.readable(vtable, sizeof(void*));
    };

    const auto classNamed = [](void* pointer, const char* needle) {
        const auto name = bes::core::rtti::className(pointer);
        return !name.empty() && name.find(needle) != std::string::npos;
    };

    if (!resolver.offsetValidated(OffId::ActorLevel) && !mProbedLevel) {
        for (const auto& candidate : Profile::get().candidates(OffId::ActorLevel)) {
            if (!space.readable(localPlayer, candidate.value + sizeof(void*))) continue;
            void* const level = sdk::fieldPointer(localPlayer, candidate.value);
            if (!readableObject(level)) continue;
            // "Level" also matches LevelRenderer / LevelChunk, so the negatives
            // matter as much as the positive.
            if (!classNamed(level, "Level")) continue;
            if (classNamed(level, "Renderer") || classNamed(level, "Chunk")) continue;
            resolver.confirmOffset(OffId::ActorLevel, candidate.value, candidate.origin);
            mProbedLevel = true;
            break;
        }
    }

    if (!resolver.offsetValidated(OffId::ActorDimension) && !mProbedDimension) {
        for (const auto& candidate : Profile::get().candidates(OffId::ActorDimension)) {
            if (!space.readable(localPlayer, candidate.value + sizeof(void*))) continue;
            void* const dimension = sdk::fieldPointer(localPlayer, candidate.value);
            if (!readableObject(dimension)) continue;
            const auto name = bes::core::rtti::className(dimension);
            if (name.empty()) continue;
            const bool isDimension = name.find("Dimension") != std::string::npos ||
                                     name.find("Overworld") != std::string::npos ||
                                     name.find("Nether") != std::string::npos ||
                                     name.find("TheEnd") != std::string::npos;
            if (!isDimension) continue;
            resolver.confirmOffset(OffId::ActorDimension, candidate.value, candidate.origin);
            mProbedDimension = true;
            break;
        }
    }

    // Level -> ActorManager. Proof is the strongest available without Zaphkiel:
    // only the real ActorManager is accepted by the resolved getRuntimeActorList
    // entry point, and an unrelated object will not be. So the probe asks the
    // game and keeps the offset only if a plausible actor comes back.
    if (!resolver.offsetValidated(OffId::LevelActorManager) && !mProbedActorManager &&
        resolver.offsetValidated(OffId::ActorLevel) && gGetActorList) {
        void* const level = sdk::fieldPointer(localPlayer, resolver.offset(OffId::ActorLevel));
        if (readableObject(level)) {
            for (const auto& candidate : Profile::get().candidates(OffId::LevelActorManager)) {
                if (!space.readable(level, candidate.value + sizeof(void*))) continue;
                void* const manager = sdk::fieldPointer(level, candidate.value);
                if (!readableObject(manager)) continue;
                // This hands `manager` to a game function, so "mapped" is not
                // enough: a wrong candidate that happens to point at any readable
                // object would be called as a Level. The RTTI check is what tells
                // a Level from a std::string body, and it is the only proof
                // available without Zaphkiel.
                if (!classNamed(manager, "Level")) continue;
                if (classNamed(manager, "Renderer") || classNamed(manager, "Chunk")) continue;
                std::vector<void*> list;
                try {
                    list = gGetActorList(manager);
                } catch (...) {
                    continue;
                }
                // An empty list is not proof: the wrong object could also yield
                // nothing. Require at least one entry that looks like a live,
                // polymorphic actor.
                if (list.empty()) continue;
                bool anyActor = false;
                for (void* entry : list) {
                    if (readableObject(entry) && !bes::core::rtti::className(entry).empty()) {
                        anyActor = true;
                        break;
                    }
                }
                if (!anyActor) continue;
                resolver.confirmOffset(OffId::LevelActorManager, candidate.value, candidate.origin);
                mProbedActorManager = true;
                break;
            }
        }
    }
}

std::vector<ActorEntry> World::actors() {
    std::vector<ActorEntry> result;
    auto& resolver = Resolver::get();

    if (!gGetActorList) {
        const auto address = resolver.address(SigId::ActorManagerList);
        if (address) gGetActorList = reinterpret_cast<GetRuntimeActorListFn>(address);
    }
    if (!gGetActorList) return result;

    // Read the level through the lock. This runs on the picker/scan thread while
    // the frame thread may be rewriting the snapshot, so touching mSnapshot
    // directly here was the same race the mutex was added to close -- it would
    // have left one path still reading the live struct.
    const auto snapshot = this->snapshot();
    if (!snapshot.level) return result;

    // An unproven offset reads as 0, and reading a pointer field at offset 0
    // yields the object's own vptr -- a real mapped address that would then be
    // passed to the game as if it were an ActorManager. Require the proof.
    if (!resolver.offsetValidated(OffId::LevelActorManager)) return result;
    const auto actorManager = sdk::fieldPointer(snapshot.level, resolver.offset(OffId::LevelActorManager));
    if (!sdk::plausiblePointer(actorManager)) return result;
    // The offset is proven, but the object it points at is read once more here and
    // then handed to game code: a Level that has since been torn down, or a field
    // that changed meaning, would fault inside getRuntimeActorList.
    if (!sdk::AddressSpace::get().hasVtableSlot0(actorManager, 0)) return result;

    std::vector<void*> list;
    try {
        list = gGetActorList(actorManager);
    } catch (...) {
        return result;
    }

    using ActorIsPlayerFn = bool (*)(void*);
    ActorIsPlayerFn isPlayer = nullptr;
    if (const auto address = resolver.address(SigId::ActorIsPlayer)) {
        isPlayer = reinterpret_cast<ActorIsPlayerFn>(address);
    }

    // 0 means "not proven", not "field at zero": reading either of these at
    // offset 0 would hand back the actor's vptr and write it into the snapshot as
    // a position. Callers that see 0 fall back to the AABB centre.
    const auto aabbOffset = resolver.offsetValidated(OffId::ActorAabbComponent)
                                ? resolver.offset(OffId::ActorAabbComponent)
                                : 0;
    const auto stateOffset = resolver.offsetValidated(OffId::ActorStateVector)
                                 ? resolver.offset(OffId::ActorStateVector)
                                 : 0;

    result.reserve(list.size());
    for (void* candidate : list) {
        if (!plausibleActor(candidate)) continue;

        ActorEntry entry;
        entry.actor = candidate;
        entry.className = bes::core::rtti::className(candidate);
        if (entry.className.empty()) continue;

        entry.isPlayer = isPlayer ? isPlayer(candidate)
                                  : bes::core::rtti::classifyActor(entry.className) ==
                                        bes::core::rtti::ActorKind::Player;

        // Both offsets are 0 unless proven, and `fieldPointer(x, 0)` is never
        // what we want here -- it aliases the vptr. The `!= 0` is load-bearing,
        // not defensive style.
        if (aabbOffset != 0) {
            if (const auto boundsComponent = sdk::fieldPointer(candidate, aabbOffset);
                sdk::plausiblePointer(boundsComponent) &&
                sdk::AddressSpace::get().readable(boundsComponent, sizeof(sdk::AABB))) {
                entry.bounds = *reinterpret_cast<const sdk::AABB*>(boundsComponent);
            }
        }
        bool havePosition = false;
        if (stateOffset != 0) {
            if (const auto stateVector = sdk::fieldPointer(candidate, stateOffset);
                sdk::plausiblePointer(stateVector) &&
                sdk::AddressSpace::get().readable(stateVector, sizeof(Vec3))) {
                entry.position = *reinterpret_cast<const Vec3*>(stateVector);
                havePosition = true;
            }
        }
        // Fall back on "no position was read", not on "the position looks like
        // zero": an actor standing at the origin has a legitimate zero position.
        if (!havePosition && entry.bounds.valid()) entry.position = entry.bounds.center();

        entry.displayName = entry.className;
        result.push_back(std::move(entry));
    }
    return result;
}

std::string World::blockName(void* blockSource, const bes::sdk::BlockPos& pos) {
    if (!blockSource) return {};
    if (!gGetBlock) return {};
    const void* block = gGetBlock(blockSource, &pos, 0);
    return blockNameFromBlock(block);
}

void* World::currentBlockSource() {
    // Called from the scan thread while the frame thread owns update(); read it
    // under the same lock as everything else that touches the snapshot.
    std::lock_guard lock(mMutex);
    return mSnapshot.blockSource;
}

void* World::blockAt(void* blockSource, const bes::sdk::BlockPos& pos) {
    if (!blockSource || !gGetBlock) return nullptr;
    return const_cast<void*>(gGetBlock(blockSource, &pos, 0));
}

void* World::blockTypeOf(const void* block) {
    if (!block) return nullptr;

    // This path reads a live game object at offsets that static RE has not
    // pinned, so every dereference is checked against the real memory map: a
    // wrong candidate must fail the check, not fault the game.
    auto& space = sdk::AddressSpace::get();
    // Load the map first: every readiness check below is meaningless until it
    // exists, and `readable` fails closed while it does not.
    if (space.regionCount() == 0 && !space.refresh()) return nullptr;
    if (!space.readable(block, 8)) return nullptr;

    for (const auto offset : kBlockTypeOffsetCandidates) {
        if (!space.readable(static_cast<const std::byte*>(block) + offset, 8)) continue;

        const auto candidate = sdk::fieldPointer(block, offset);
        if (!space.readable(candidate, 16)) continue;

        std::uintptr_t vtable = 0;
        std::memcpy(&vtable, candidate, sizeof(vtable));
        if (!space.readable(reinterpret_cast<const void*>(vtable), 8)) continue;

        std::uintptr_t firstEntry = 0;
        std::memcpy(&firstEntry, reinterpret_cast<const void*>(vtable), sizeof(firstEntry));
        if (!space.readable(reinterpret_cast<const void*>(firstEntry), 4)) continue;
        return candidate;
    }
    return nullptr;
}

void World::installBlockLookup() {
    auto& resolver = Resolver::get();
    if (const auto address = resolver.address(SigId::ActorManagerList); address && !gGetActorList) {
        gGetActorList = reinterpret_cast<GetRuntimeActorListFn>(address);
    }
    if (!gGetBlock) {
        const auto address = resolver.address(SigId::BlockSourceGetBlock);
        if (address) gGetBlock = reinterpret_cast<BlockSourceGetBlockFn>(address);
    }
}

bool World::entityPipelineReady() const {
    return gGetActorList != nullptr;
}

bool World::blockPipelineReady() const {
    return gGetBlock != nullptr;
}

} // namespace bes::world
