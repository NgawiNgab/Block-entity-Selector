#pragma once

#include "world/World.hpp"

#include <string>

namespace bes::launcher {

/// The in-game picking affordance.
///
/// There is no direct hook for "what is under the crosshair" that survives
/// every game build, so the picker resolves targets from data the mod already
/// has: the actor boxes it scanned this tick, and the block right under the
/// player's feet (or under the crosshair when a hit result is available).
class Picker {
public:
    static Picker& get();

    /// Registers the on-screen Select button plus its menu entries.
    bool install(const std::string& modId);

    void uninstall();

    /// Cycles the current selection: next matching class under the crosshair,
    /// then the block the player is standing in, then clears.
    void pickUnderCrosshair();

private:
    Picker() = default;

    std::string pickEntityAtCrosshair();
    std::string pickBlockAtFeet();

    bool mInstalled = false;
};

}
