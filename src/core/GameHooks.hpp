#pragma once

#include <cstdint>
#include <functional>

namespace bes::core::gamehooks {

/// Called once per ClientInstance::update with the local player (may be null
/// while a world is loading).
using TickCallback = std::function<void(void* localPlayer)>;

/// Hooks ClientInstance::update, which is the cheapest reliable per-tick entry
/// point that also hands over the ClientInstance pointer.
bool install(TickCallback callback);
void uninstall();

void* clientInstance();
void* localPlayer();

/// Last camera position read from the player renderer, or false when the level
/// renderer is not reachable yet.
bool cameraPosition(float out[3]);

}
