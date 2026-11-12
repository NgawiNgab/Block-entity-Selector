#include "launcher/Picker.hpp"

#include "core/Logging.hpp"
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
    const auto& snapshot = world.snapshot();
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
    const auto& snapshot = world.snapshot();
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
    auto& selector = modules::SelectorModule::get();
    if (!selector.enabled()) {
        core::log::info("picker: module is off, enabling it");
        selector.setEnabled(true);
        // Reflect the change in the launcher's toggle, which the picker changed
        // outside of the menu.
        pl::modmenu::setModuleEnabled(selector.moduleId(), true);
    }

    auto& selection = selection::Selection::get();
    const selection::Target current = selection.current();

    const auto entityClass = pickEntityAtCrosshair();
    const auto blockId = pickBlockAtFeet();

    // Cycle: entity -> block -> clear, skipping whichever is unavailable.
    if (!entityClass.empty() && !(current.kind == selection::Kind::Entity && current.id == entityClass)) {
        selection.toggleEntity(entityClass, entityClass);
        selection::Scanner::get().reset();
        core::log::info("picker: entity " + entityClass);
        return;
    }

    if (!blockId.empty() && !(current.kind == selection::Kind::Block && current.id == blockId)) {
        selection.toggleBlock(blockId, blockId);
        selection::Scanner::get().reset();
        core::log::info("picker: block " + blockId);
        return;
    }

    if (current) {
        selection.clear();
        selection::Scanner::get().reset();
        core::log::info("picker: selection cleared");
    }
}

} // namespace bes::launcher
