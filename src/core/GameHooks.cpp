#include "core/GameHooks.hpp"

#include "core/Logging.hpp"
#include "core/Resolver.hpp"

#include <bes/sdk/Memory.hpp>
#include <pl/memory/Hook.hpp>

#include <atomic>
#include <cmath>
#include <mutex>

namespace bes::core::gamehooks {
namespace {

using ClientInstanceUpdateFn = void* (*)(void* clientInstance, bool);
using GetLocalPlayerFn = void* (*)(void* clientInstance);

ClientInstanceUpdateFn gUpdateOriginal = nullptr;
pl::memory::HookHandle gUpdateHook;
GetLocalPlayerFn gGetLocalPlayer = nullptr;

std::atomic<void*> gClientInstance{nullptr};
std::atomic<void*> gLocalPlayer{nullptr};
TickCallback gTickCallback;
std::mutex gInstallMutex;
bool gInstalled = false;

void* readLocalPlayer(void* clientInstance) {
    if (!clientInstance) return nullptr;

    if (gGetLocalPlayer) {
        try {
            if (void* player = gGetLocalPlayer(clientInstance)) return player;
        } catch (...) {
            // Fall through to the vtable path.
        }
    }

    auto** table = *reinterpret_cast<void***>(clientInstance);
    if (!table || !table[31]) return nullptr;
    using VtableFn = void* (*)(void*);
    return reinterpret_cast<VtableFn>(table[31])(clientInstance);
}

void* updateDetour(void* clientInstance, bool value) {
    void* result = gUpdateOriginal ? gUpdateOriginal(clientInstance, value) : nullptr;

    if (clientInstance && sdk::plausiblePointer(clientInstance)) {
        gClientInstance.store(clientInstance, std::memory_order_release);
        const auto player = readLocalPlayer(clientInstance);
        gLocalPlayer.store(player, std::memory_order_release);
        if (gTickCallback && player) {
            try {
                gTickCallback(player);
            } catch (...) {
                // A throwing tick callback must never take the game down.
            }
        }
    }
    return result;
}

} // namespace

bool install(TickCallback callback) {
    std::lock_guard lock(gInstallMutex);
    gTickCallback = std::move(callback);

    if (gInstalled) return true;

    auto& resolver = Resolver::get();
    const auto updateAddress = resolver.address(SigId::ClientInstanceUpdate);
    if (!updateAddress) {
        log::error("hooks: client_instance_update signature unresolved");
        return false;
    }

    if (!gGetLocalPlayer) {
        gGetLocalPlayer = reinterpret_cast<GetLocalPlayerFn>(resolver.address(SigId::ClientInstanceGetLocalPlayer));
    }

    auto* target = reinterpret_cast<void*>(updateAddress);
    auto* detour = reinterpret_cast<void*>(&updateDetour);
    if (pl::memory::hook(target, detour, reinterpret_cast<void**>(&gUpdateOriginal)) != 0) {
        log::error("hooks: ClientInstance::update hook rejected");
        return false;
    }
    gUpdateHook = pl::memory::HookHandle(target, detour, reinterpret_cast<void**>(&gUpdateOriginal));

    gInstalled = true;
    log::info("hooks: ClientInstance::update installed");
    return true;
}

void uninstall() {
    std::lock_guard lock(gInstallMutex);
    if (!gInstalled) return;
    gUpdateHook.reset();
    gUpdateOriginal = nullptr;
    gInstalled = false;
    gClientInstance.store(nullptr, std::memory_order_release);
    gLocalPlayer.store(nullptr, std::memory_order_release);
}

void* clientInstance() {
    return gClientInstance.load(std::memory_order_acquire);
}

void* localPlayer() {
    return gLocalPlayer.load(std::memory_order_acquire);
}

bool cameraPosition(float out[3]) {
    if (!out) return false;
    const auto client = clientInstance();
    if (!client) return false;

    auto& resolver = Resolver::get();
    const auto levelRendererOffset = resolver.offset(OffId::ClientInstanceLevelRenderer);
    if (!levelRendererOffset) return false;

    const auto levelRenderer = sdk::fieldPointer(client, levelRendererOffset);
    if (!sdk::plausiblePointer(levelRenderer)) return false;

    const auto playerRendererOffset = resolver.offset(OffId::LevelRendererPlayer);
    if (!playerRendererOffset) return false;

    const auto playerRenderer = sdk::fieldPointer(levelRenderer, playerRendererOffset);
    if (!sdk::plausiblePointer(playerRenderer)) return false;

    const auto cameraOffset = resolver.offset(OffId::LevelRendererPlayerCamPos);
    if (!cameraOffset) return false;

    const auto* camera = reinterpret_cast<const float*>(static_cast<const std::byte*>(playerRenderer) + cameraOffset);
    if (!std::isfinite(camera[0]) || !std::isfinite(camera[1]) || !std::isfinite(camera[2])) return false;
    out[0] = camera[0];
    out[1] = camera[1];
    out[2] = camera[2];
    return true;
}

} // namespace bes::core::gamehooks
