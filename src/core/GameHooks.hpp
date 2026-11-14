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

/// Records the LevelRenderer passed to the renderLevel hook.
///
/// This argument is the corroborated route to LevelRendererPlayer: BedrockTools
/// reaches the camera from it directly, without a ClientInstance hop, so it lets
/// the camera chain prove itself while skipping the one offset in that path that
/// no external source confirms.
void noteRenderLevelReceiver(void* levelRenderer);
void* renderLevelReceiver();

}
