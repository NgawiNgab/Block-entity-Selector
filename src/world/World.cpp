#include "world/World.hpp"

#include "core/Logging.hpp"
#include "core/Resolver.hpp"
#include "core/Rtti.hpp"

#include <bes/sdk/Memory.hpp>

#include <algorithm>
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
std::string readIdentifier(std::uintptr_t address) {
    if (address < 0x1000) return {};
    auto* const raw = reinterpret_cast<const char*>(address);
    // libc++ std::string with SSO: size at +0, bytes inline at +1.
    const auto size = static_cast<std::size_t>(*reinterpret_cast<const std::uint8_t*>(raw));
    if (size >= 2 && size <= 64) {
        std::string candidate(raw + 1, size);
        if (looksLikeIdentifier(candidate)) return candidate;
    }
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

} // namespace

World& World::get() {
    static World instance;
    return instance;
}

void World::update(void* localPlayer, const Vec3& cameraPosition) {
    mSnapshot.localPlayer = localPlayer;
    mSnapshot.cameraPosition = cameraPosition;
    if (!localPlayer) {
        mSnapshot.valid = false;
        return;
    }

    auto& resolver = Resolver::get();

    // Probing has to run before anything reads through the offsets, otherwise a
    // rejected candidate would be used once and poison the snapshot.
    probeOffsets(localPlayer);

    mSnapshot.level = sdk::fieldPointer(localPlayer, resolver.offset(OffId::ActorLevel));
    mSnapshot.dimension = sdk::fieldPointer(localPlayer, resolver.offset(OffId::ActorDimension));

    if (!mSnapshot.level || !sdk::plausiblePointer(mSnapshot.level)) {
        mSnapshot.level = nullptr;
        mSnapshot.valid = false;
        return;
    }

    const auto stateVector = sdk::fieldPointer(localPlayer, resolver.offset(OffId::ActorStateVector));
    if (sdk::plausiblePointer(stateVector)) {
        mSnapshot.playerPosition = *reinterpret_cast<Vec3*>(stateVector);
    }

    // Dimension -> BlockSource candidate offsets from the profile, validated by
    // the fact that the block source must expose a readable dimension id.
    mSnapshot.blockSource = nullptr;
    if (sdk::plausiblePointer(mSnapshot.dimension)) {
        const auto& candidates = Profile::get().candidates(OffId::DimensionBlockSource);
        for (const auto& candidate : candidates) {
            const auto blockSource = sdk::fieldPointer(mSnapshot.dimension, candidate.value);
            if (!sdk::plausiblePointer(blockSource)) continue;
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
    const auto rotationComponent = sdk::fieldPointer(localPlayer, 0x238);
    if (sdk::plausiblePointer(rotationComponent)) {
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

    if (!mProbedStateVector && localPlayer) {
        mProbedStateVector = true;
        for (const auto& candidate : Profile::get().candidates(OffId::ActorStateVector)) {
            const auto pointer = sdk::fieldPointer(localPlayer, candidate.value);
            if (!sdk::plausiblePointer(pointer)) continue;
            const auto* position = reinterpret_cast<const Vec3*>(pointer);
            if (!position->finite()) continue;
            if (std::fabs(position->y) > 4000.f) continue;
            resolver.confirmOffset(OffId::ActorStateVector, candidate.value, candidate.origin);
            mSnapshot.playerPosition = *position;
            break;
        }
    }

    if (!mProbedAabb) {
        mProbedAabb = true;
        for (const auto& candidate : Profile::get().candidates(OffId::ActorAabbComponent)) {
            const auto component = sdk::fieldPointer(localPlayer, candidate.value);
            if (!sdk::plausiblePointer(component)) continue;
            const auto* bounds = reinterpret_cast<const sdk::AABB*>(component);
            if (!bounds->valid()) continue;
            resolver.confirmOffset(OffId::ActorAabbComponent, candidate.value, candidate.origin);
            break;
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
    if (!gGetActorList || !mSnapshot.level) return result;

    const auto actorManager = sdk::fieldPointer(mSnapshot.level, resolver.offset(OffId::LevelActorManager));
    if (!sdk::plausiblePointer(actorManager)) return result;

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

    const auto aabbOffset = resolver.offset(OffId::ActorAabbComponent);
    const auto stateOffset = resolver.offset(OffId::ActorStateVector);

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

        if (const auto boundsComponent = sdk::fieldPointer(candidate, aabbOffset);
            sdk::plausiblePointer(boundsComponent)) {
            entry.bounds = *reinterpret_cast<const sdk::AABB*>(boundsComponent);
        }
        if (const auto stateVector = sdk::fieldPointer(candidate, stateOffset);
            sdk::plausiblePointer(stateVector)) {
            entry.position = *reinterpret_cast<const Vec3*>(stateVector);
        } else if (entry.bounds.valid()) {
            entry.position = entry.bounds.center();
        }

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
    return mSnapshot.blockSource;
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
