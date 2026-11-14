#include "launcher/Picker.hpp"

#include "core/Logging.hpp"
#include "launcher/HudNotice.hpp"
#include "core/Runtime.hpp"
#include "modules/SelectorModule.hpp"
#include "selection/Scanner.hpp"
#include "selection/Selection.hpp"

#include <bes/sdk/Memory.hpp>

#include <pl/ModMenu.hpp>

#include <algorithm>
#include <cmath>

namespace bes::launcher {
namespace {

constexpr const char* kButtonId = "blockentityselector.select";

/// Upper bound for the crosshair ray, clamped by the configured entity range so
/// the picker can never grab something the scanner would refuse to highlight.
constexpr float kMaxPickDistance = 128.f;
constexpr float kMinPickDistance = 8.f;

} // namespace

Picker& Picker::get() {
    static Picker instance;
    return instance;
}

bool Picker::install(const std::string& modId) {
    if (mInstalled) return true;

    pl::modmenu::ButtonBuilder button(kButtonId, "Select");
    button.moduleId(modules::SelectorModule::get().moduleId())
        .modId(modId)
        .label("SEL")
        .behavior(pl::modmenu::ButtonBehavior::Click)
        .defaultVisible(true)
        .stylePreset(pl::modmenu::ButtonStylePreset::Accent)
        .sizeScale(1.0f, 1.0f)
        .onEvent([](std::string_view, pl::modmenu::ButtonEvent event, float) {
            if (event == pl::modmenu::ButtonEvent::Click ||
                event == pl::modmenu::ButtonEvent::StateChanged) {
                Picker::get().pickUnderCrosshair();
            }
        });

    if (!button.registerButton()) {
        core::log::warn("picker: on-screen button rejected by the launcher");
        return false;
    }

    mInstalled = true;
    core::log::info("picker: on-screen Select button registered");
    return true;
}

void Picker::uninstall() {
    if (!mInstalled) return;
    pl::modmenu::unregisterButton(kButtonId);
    mInstalled = false;
}

std::string Picker::pickEntityAtCrosshair() {
    auto& world = world::World::get();
    const auto snapshot = world.snapshot();
    if (!snapshot.valid) return {};

    const auto origin = snapshot.cameraPosition;
    const auto direction = snapshot.lookDirection;

    const auto range = std::clamp(selection::Scanner::get().settings().entityRange, kMinPickDistance,
                                 kMaxPickDistance);

    const auto actors = world.actors();
    std::string bestClass;
    float bestDistance = range;

    for (const auto& actor : actors) {
        if (!actor.bounds.valid()) continue;
        if (!actor.bounds.expanded(0.15f).intersectsRay(origin, direction, range)) continue;
        const auto center = actor.bounds.center();
        const float distance = (center - origin).length();
        if (distance < bestDistance) {
            bestDistance = distance;
            bestClass = actor.className;
        }
    }
    return bestClass;
}

std::string Picker::pickBlockAtFeet() {
    auto& world = world::World::get();
    const auto snapshot = world.snapshot();
    if (!snapshot.valid) return {};

    // Start slightly below the camera so a block the player is standing in wins
    // over the block they are looking at.
    auto probe = snapshot.playerPosition;
    probe.y -= 0.2f;

    auto hit = selection::Scanner::get().nearestBlock(snapshot, probe, 2);
    if (!hit.hasBlock) return {};
    return hit.blockId;
}

void Picker::pickUnderCrosshair() {
    const auto now = core::nowMilliseconds();
    auto& selector = modules::SelectorModule::get();

    // Every tap now ends in a visible message. The old version returned silently
    // on each failure path, so a tap that did nothing was indistinguishable from a
    // button that was never wired up -- which is exactly how it was reported.
    if (!selector.enabled()) {
        core::log::info("picker: module is off, enabling it");
        selector.setEnabled(true);
        // Reflect the change in the launcher's toggle, which the picker changed
        // outside of the menu.
        pl::modmenu::setModuleEnabled(selector.moduleId(), true);
        launcher::HudNotice::get().show("Selector enabled", now);
    }

    auto& selection = selection::Selection::get();
    const selection::Target current = selection.current();

    // A pick is only possible against a live world snapshot. Say so explicitly:
    // the reason is almost always an unproven offset, not a missing world, and
    // naming the blocker is what turns "nothing happens" into a next step.
    const auto snapshot = world::World::get().snapshot();
    if (!snapshot.valid) {
        launcher::HudNotice::get().show("No world data yet - see Diagnostics", now);
        if (!mLoggedNoSnapshot) {
            mLoggedNoSnapshot = true;
            core::log::warn("picker: no world snapshot yet -- the game hook is not ticking "
                            "or an offset is unproven (see the diagnostics dump: 'startup' section)");
        }
        return;
    }
    if (mLoggedNoSnapshot) {
        mLoggedNoSnapshot = false;
        core::log::info("picker: world snapshot is now available");
    }

    const auto entityClass = pickEntityAtCrosshair();
    const auto blockId = pickBlockAtFeet();

    // Cycle: entity -> block -> clear, skipping whichever is unavailable.
    if (!entityClass.empty() && !(current.kind == selection::Kind::Entity && current.id == entityClass)) {
        // The picker observes a live class, which is evidence in its own right.
        selection.selectEntityFromClass(entityClass);
        selection::Scanner::get().reset();
        core::log::info("picker: entity " + entityClass);
        launcher::HudNotice::get().show(entityClass + " selected", now);
        return;
    }

    if (!blockId.empty() && !(current.kind == selection::Kind::Block && current.id == blockId)) {
        selection.selectBlock(blockId);
        selection::Scanner::get().reset();
        core::log::info("picker: block " + blockId);
        launcher::HudNotice::get().show(blockId + " selected", now);
        return;
    }

    if (current) {
        selection.clear();
        selection::Scanner::get().reset();
        core::log::info("picker: selection cleared");
        launcher::HudNotice::get().show("Selection cleared", now);
        return;
    }

    // Nothing under the crosshair and nothing at the feet. Distinguish the two
    // reasons rather than printing one sentence for both: an empty entity list can
    // mean no actors are loaded, while a broken block lookup means the block half
    // of the mod has no path at all.
    const bool entityPipeline = world::World::get().entityPipelineReady();
    const bool blockPipeline = world::World::get().blockPipelineReady();
    std::string reason = "Nothing to select here";
    if (!entityPipeline && !blockPipeline) {
        reason = "No entity or block access yet - see Diagnostics";
    } else if (!blockPipeline) {
        reason = "No block access yet - see Diagnostics";
    } else if (!entityPipeline) {
        reason = "No entity access yet - see Diagnostics";
    } else if (snapshot.blockSource == nullptr) {
        reason = "Block source not resolved - see Diagnostics";
    }
    core::log::info("picker: nothing under the crosshair or at the player's feet");
    launcher::HudNotice::get().show(std::move(reason), now);
}

} // namespace bes::launcher
