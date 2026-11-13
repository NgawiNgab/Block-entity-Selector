#pragma once

#include "render/HighlightRenderer.hpp"
#include "selection/Selection.hpp"

#include <bes/sdk/Types.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>

namespace bes::modules {

/// The single user-facing module. Everything else in the mod exists to support it.
class SelectorModule {
public:
    static SelectorModule& get();

    /// Registers the module with LeviLauncher's Mod Menu. Safe to call twice.
    bool registerInMenu(const std::string& modId);

    /// Applies a config document (from the launcher or a menu edit).
    void loadConfig(const nlohmann::json& config);

    /// Serialises the current config back out.
    nlohmann::json saveConfig() const;

    /// Called from the launcher callbacks.
    void setEnabled(bool enabled);
    void onConfigChanged(std::string_view key, std::string_view value);
    void onKeybind(std::string_view key, bool isDown);

    /// Per-tick work: refresh the world snapshot and advance the scan.
    void onTick(void* localPlayer, std::uint64_t nowMs, const bes::sdk::Vec3& cameraPosition = {});

    /// Hooks the world renderer. Returns false when a required signature is missing.
    bool installRenderer();

    bool enabled() const { return mEnabled; }

    const std::string& moduleId() const noexcept { return mModuleId; }

    /// Human-readable target description for the module menu label.
    std::string targetLabel() const;

    // ---- config ---------------------------------------------------------
    bes::render::Style style{};
    bool seeThrough = true;
    bool showBlocks = true;
    bool includePlayers = true;
    bool includeOnlyMobs = true;
    float scanRadius = 40.f;
    bool autoPickNearestOnEmpty = false;
    int keybind = 0;
    /// Last text typed into the identifier fields. Kept here because the launcher
    /// reports text edits and button presses as separate config events.
    std::string mPendingBlockId;
    std::string mPendingEntityId;

private:
    SelectorModule() = default;

    void applyStyle();
    void refreshTargetLabel();

    std::string mModuleId{"blockentityselector.selector"};
    bool mEnabled = false;
    bool mMenuRegistered = false;
    bool mRendererInstalled = false;
    std::string mLastTargetLabel;
};

}
