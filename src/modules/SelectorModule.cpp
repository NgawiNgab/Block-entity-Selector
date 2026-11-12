#include "modules/SelectorModule.hpp"

#include "core/Logging.hpp"
#include "core/Resolver.hpp"
#include "core/Runtime.hpp"
#include "selection/Scanner.hpp"
#include "world/World.hpp"

#include <bes/Version.hpp>

#include <pl/ModMenu.hpp>

#include <algorithm>
#include <sstream>

namespace bes::modules {
namespace {

std::uint32_t parseColor(std::string_view text, std::uint32_t fallback) {
    if (text.size() < 7 || text.front() != '#') return fallback;
    try {
        const auto value = std::stoul(std::string(text.substr(1)), nullptr, 16);
        if (text.size() == 7) {
            // #RRGGBB: assume fully opaque.
            return 0xFF000000u | static_cast<std::uint32_t>(value);
        }
        return static_cast<std::uint32_t>(value);
    } catch (...) {
        return fallback;
    }
}

std::string formatColor(const bes::sdk::Color& color) {
    const auto toByte = [](float value) {
        return static_cast<unsigned>(std::clamp(value, 0.f, 1.f) * 255.f + 0.5f);
    };
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X%02X", toByte(color.a), toByte(color.r), toByte(color.g),
                  toByte(color.b));
    return buffer;
}

bool parseBool(std::string_view value, bool fallback) {
    if (value == "true" || value == "1") return true;
    if (value == "false" || value == "0") return false;
    return fallback;
}

float parseFloat(std::string_view value, float fallback) {
    try {
        return std::stof(std::string(value));
    } catch (...) {
        return fallback;
    }
}

std::string targetSummary() {
    const auto target = selection::Selection::get().current();
    if (!target) return "Nothing selected";
    return (target.kind == selection::Kind::Block ? "Block: " : "Entity: ") + target.prettyName;
}

} // namespace

SelectorModule& SelectorModule::get() {
    static SelectorModule instance;
    return instance;
}

void SelectorModule::applyStyle() {
    // Keep the renderer style and the scanner settings in sync with the config
    // in one place so menu edits and config loads behave identically.
    style.seeThrough = seeThrough;
    style.drawFill = !style.outlineOnly;
    render::HighlightRenderer::get().setStyle(style);

    selection::Scanner::Settings settings;
    settings.scanBlocks = showBlocks;
    settings.includePlayers = includePlayers;
    settings.radius = std::clamp(scanRadius, 4.f, 128.f);
    settings.verticalRadius = std::clamp(scanRadius * 0.8f, 4.f, 64.f);
    settings.entityRange = std::clamp(scanRadius * 2.f + 40.f, 48.f, 256.f);
    selection::Scanner::get().setSettings(settings);
}

bool SelectorModule::registerInMenu(const std::string& modId) {
    if (mMenuRegistered) return true;

    pl::modmenu::ModuleBuilder builder(mModuleId, "Block / Entity Selector");
    builder.description("Highlights every instance of the selected block or entity type, visible through walls.")
        .modId(modId)
        .defaultEnabled(false)
        .hideInHudEditor(true)
        .onToggle([](std::string_view, bool enabled) { SelectorModule::get().setEnabled(enabled); })
        .onConfigChanged([](std::string_view, std::string_view key, std::string_view value) {
            SelectorModule::get().onConfigChanged(key, value);
        })
        .onKeybind([](std::string_view, std::string_view key, bool isDown) {
            SelectorModule::get().onKeybind(key, isDown);
        });

    // The legacy config API has no read-only node type, so the status line is a
    // plain Text entry that the module rewrites whenever the target changes.
    builder.config("status", "Selected", pl::modmenu::ConfigType::Text, targetSummary());
    builder.config("seeThrough", "See Through Walls", pl::modmenu::ConfigType::Toggle, "true");
    builder.config("showBlocks", "Scan Blocks", pl::modmenu::ConfigType::Toggle, "true");
    builder.config("includePlayers", "Include Players", pl::modmenu::ConfigType::Toggle, "true");
    builder.config("scanRadius", "Scan Radius", pl::modmenu::ConfigType::SliderFloat, "40", "8", "80");
    builder.config("blockColor", "Block Color", pl::modmenu::ConfigType::Color, formatColor(style.blockColor));
    builder.config("entityColor", "Entity Color", pl::modmenu::ConfigType::Color, formatColor(style.entityColor));
    builder.config("playerColor", "Player Color", pl::modmenu::ConfigType::Color,
                   formatColor(style.entityPlayerColor));
    builder.config("outlineOnly", "Outline Only", pl::modmenu::ConfigType::Toggle, "true");
    builder.config("fillAlpha", "Fill Opacity", pl::modmenu::ConfigType::SliderFloat, "0.18", "0.02", "0.6");
    builder.config("clearSelection", "Clear Selection", pl::modmenu::ConfigType::Button, "false");
    builder.config("rescan", "Rescan Now", pl::modmenu::ConfigType::Button, "false");
    builder.config("diagnostics", "Diagnostics Dump", pl::modmenu::ConfigType::Button, "false");

    if (!builder.registerModule()) {
        core::log::error("menu: registration rejected by the launcher");
        return false;
    }

    // Rich schema (V2) so the menu groups things sensibly; optional, the legacy
    // entries above keep the module usable if the launcher ignores it.
    pl::modmenu::ConfigSchemaBuilder schema;
    schema.defaultCategory("selection")
        .category("selection", "Selection", "Pick what to highlight with the on-screen Select button.")
        .category("visual", "Visual", "Colours and draw style.")
        .category("scan", "Scanning", "How much of the world is scanned.")
        .category("debug", "Diagnostics", "Tools for updating the mod for a new game build.");

    auto node = [](std::string id, std::string key, std::string category, std::string title,
                   pl::modmenu::ConfigControlTypeV2 type) {
        pl::modmenu::ConfigNodeV2 value;
        value.id = std::move(id);
        value.key = std::move(key);
        value.category = std::move(category);
        value.title = std::move(title);
        value.type = type;
        return value;
    };

    auto toggle = [&](const char* key, const char* category, const char* title, const char* value) {
        auto entry = node(key, key, category, title, pl::modmenu::ConfigControlTypeV2::Toggle);
        entry.defaultValue = value;
        schema.node(std::move(entry));
    };
    auto color = [&](const char* key, const char* title, const bes::sdk::Color& current) {
        auto entry = node(key, key, "visual", title, pl::modmenu::ConfigControlTypeV2::Color);
        entry.defaultValue = formatColor(current);
        entry.colorAlpha = true;
        schema.node(std::move(entry));
    };
    auto slider = [&](const char* key, const char* category, const char* title, const char* value,
                      const char* minValue, const char* maxValue, const char* step) {
        auto entry = node(key, key, category, title, pl::modmenu::ConfigControlTypeV2::SliderFloat);
        entry.defaultValue = value;
        entry.minValue = minValue;
        entry.maxValue = maxValue;
        entry.step = step;
        schema.node(std::move(entry));
    };
    auto button = [&](const char* key, const char* category, const char* title) {
        auto entry = node(key, key, category, title, pl::modmenu::ConfigControlTypeV2::Button);
        entry.actionValue = "true";
        schema.node(std::move(entry));
    };

    {
        auto info = node("status", "", "selection", "Selected", pl::modmenu::ConfigControlTypeV2::Info);
        info.description = targetSummary();
        schema.node(std::move(info));
    }
    toggle("seeThrough", "visual", "See Through Walls", "true");
    color("blockColor", "Block Colour", style.blockColor);
    color("entityColor", "Entity Colour", style.entityColor);
    color("playerColor", "Player Colour", style.entityPlayerColor);
    toggle("outlineOnly", "visual", "Outline Only", "true");
    slider("fillAlpha", "visual", "Fill Opacity", "0.18", "0.02", "0.6", "0.02");
    slider("scanRadius", "scan", "Scan Radius (blocks)", "40", "8", "80", "1");
    toggle("showBlocks", "scan", "Scan Blocks", "true");
    toggle("includePlayers", "scan", "Include Players", "true");
    button("clearSelection", "selection", "Clear Selection");
    button("rescan", "scan", "Rescan Now");
    button("diagnostics", "debug", "Dump Diagnostics");
    schema.node(node("clearSelection", "clearSelection", "selection", "Clear Selection",
                     pl::modmenu::ConfigControlTypeV2::Button));
    schema.node(node("rescan", "rescan", "scan", "Rescan Now", pl::modmenu::ConfigControlTypeV2::Button));
    schema.node(node("diagnostics", "diagnostics", "debug", "Dump Diagnostics",
                     pl::modmenu::ConfigControlTypeV2::Button));

    if (!pl::modmenu::setConfigSchemaJson(mModuleId, schema.toJson())) {
        core::log::warn("menu: rich config schema rejected, legacy entries remain active");
    }

    mMenuRegistered = true;
    core::log::info("menu: module registered");
    return true;
}

void SelectorModule::setEnabled(bool enabled) {
    if (mEnabled == enabled) {
        // Still make sure the renderer is attached the first time round.
        if (enabled) installRenderer();
        return;
    }
    mEnabled = enabled;
    if (enabled) {
        applyStyle();
        installRenderer();
        selection::Scanner::get().reset();
        core::log::info("selector enabled");
    } else {
        selection::Scanner::get().reset();
        core::log::info("selector disabled");
    }
}

bool SelectorModule::installRenderer() {
    if (mRendererInstalled) return true;
    mRendererInstalled = render::HighlightRenderer::get().install();
    return mRendererInstalled;
}

std::string SelectorModule::targetLabel() const {
    return targetSummary();
}

void SelectorModule::refreshTargetLabel() {
    const auto label = targetLabel();
    if (label == mLastTargetLabel) return;
    mLastTargetLabel = label;
    if (mMenuRegistered) {
        // Info nodes are re-read from the schema on open; nothing to push here.
        core::log::debug("menu: target label -> " + label);
    }
}

void SelectorModule::onTick(void* localPlayer, std::uint64_t nowMs, const bes::sdk::Vec3& cameraPosition) {
    if (!mEnabled) return;

    auto& world = world::World::get();
    world.update(localPlayer, cameraPosition);

    selection::Scanner::get().update(world.snapshot(), nowMs);
    refreshTargetLabel();
}

void SelectorModule::onKeybind(std::string_view key, bool isDown) {
    // The launcher only routes the module's bound key here, so no key matching is
    // needed; filtering on a hardcoded name would silently ignore the bind.
    if (!isDown) return;
    core::log::debug("keybind pressed: " + std::string(key));

    auto& selectionStore = selection::Selection::get();
    // The keybind is an escape hatch and a mode switch in one: with something
    // selected it clears the selection, otherwise it turns highlighting on or
    // off. Picking a target stays on the on-screen Select button, which is the
    // only thing that knows what the crosshair is pointing at.
    if (selectionStore.current()) {
        selectionStore.clear();
        selection::Scanner::get().reset();
        refreshTargetLabel();
        core::log::info("keybind: selection cleared");
        return;
    }

    const bool next = !mEnabled;
    setEnabled(next);
    // Keep the launcher's toggle in sync with a state change that did not come
    // from the menu itself.
    pl::modmenu::setModuleEnabled(mModuleId, next);
}

void SelectorModule::onConfigChanged(std::string_view key, std::string_view value) {
    if (key == "seeThrough") {
        seeThrough = parseBool(value, seeThrough);
    } else if (key == "showBlocks") {
        showBlocks = parseBool(value, showBlocks);
    } else if (key == "includePlayers") {
        includePlayers = parseBool(value, includePlayers);
    } else if (key == "outlineOnly") {
        style.outlineOnly = parseBool(value, style.outlineOnly);
        style.drawFill = !style.outlineOnly;
    } else if (key == "scanRadius") {
        scanRadius = parseFloat(value, scanRadius);
    } else if (key == "fillAlpha") {
        style.fillAlpha = parseFloat(value, style.fillAlpha);
    } else if (key == "blockColor") {
        style.blockColor = bes::sdk::Color::fromArgb(parseColor(value, 0xFF2ED573));
    } else if (key == "entityColor") {
        style.entityColor = bes::sdk::Color::fromArgb(parseColor(value, 0xFFFFA502));
    } else if (key == "playerColor") {
        style.entityPlayerColor = bes::sdk::Color::fromArgb(parseColor(value, 0xFF3B82F6));
    } else if (key == "clearSelection") {
        selection::Selection::get().clear();
        selection::Scanner::get().reset();
        return;
    } else if (key == "rescan") {
        selection::Scanner::get().reset();
        return;
    } else if (key == "diagnostics") {
        core::Runtime::get().dumpDiagnostics();
        return;
    } else {
        return;
    }
    applyStyle();
}

void SelectorModule::loadConfig(const nlohmann::json& config) {
    if (config.contains("seeThrough")) seeThrough = config["seeThrough"].get<bool>();
    if (config.contains("showBlocks")) showBlocks = config["showBlocks"].get<bool>();
    if (config.contains("includePlayers")) includePlayers = config["includePlayers"].get<bool>();
    if (config.contains("outlineOnly")) {
        style.outlineOnly = config["outlineOnly"].get<bool>();
        style.drawFill = !style.outlineOnly;
    }
    if (config.contains("scanRadius")) scanRadius = config["scanRadius"].get<float>();
    if (config.contains("fillAlpha")) style.fillAlpha = config["fillAlpha"].get<float>();
    if (config.contains("keybind")) keybind = config["keybind"].get<int>();

    if (config.contains("blockColor") && config["blockColor"].is_string()) {
        style.blockColor = bes::sdk::Color::fromArgb(parseColor(config["blockColor"].get<std::string>(), 0xFF2ED573));
    }
    if (config.contains("entityColor") && config["entityColor"].is_string()) {
        style.entityColor =
            bes::sdk::Color::fromArgb(parseColor(config["entityColor"].get<std::string>(), 0xFFFFA502));
    }
    if (config.contains("playerColor") && config["playerColor"].is_string()) {
        style.entityPlayerColor =
            bes::sdk::Color::fromArgb(parseColor(config["playerColor"].get<std::string>(), 0xFF3B82F6));
    }

    applyStyle();
}

nlohmann::json SelectorModule::saveConfig() const {
    nlohmann::json config;
    config["seeThrough"] = seeThrough;
    config["showBlocks"] = showBlocks;
    config["includePlayers"] = includePlayers;
    config["outlineOnly"] = style.outlineOnly;
    config["scanRadius"] = scanRadius;
    config["fillAlpha"] = style.fillAlpha;
    config["keybind"] = keybind;
    config["blockColor"] = formatColor(style.blockColor);
    config["entityColor"] = formatColor(style.entityColor);
    config["playerColor"] = formatColor(style.entityPlayerColor);
    config["status"] = targetSummary();
    return config;
}

} // namespace bes::modules
