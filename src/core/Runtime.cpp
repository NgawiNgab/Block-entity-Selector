#include "core/Runtime.hpp"

#include "diag/BlockTypeProbe.hpp"

#include "core/ConfigStore.hpp"
#include "core/GameHooks.hpp"
#include "core/Logging.hpp"
#include "core/Profile.hpp"
#include "core/Resolver.hpp"
#include "launcher/Picker.hpp"
#include "modules/SelectorModule.hpp"
#include "render/HighlightRenderer.hpp"
#include "selection/Identifier.hpp"
#include "selection/Scanner.hpp"
#include "selection/Selection.hpp"
#include "world/World.hpp"

#include <bes/Version.hpp>

#include <pl/Input.hpp>
#include <pl/ModMenu.hpp>

#include <pl/memory/Hook.hpp>

#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

#include <atomic>

#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>

namespace bes::core {
namespace {

/// The mod runs in the launcher process; only the Minecraft child has the game
/// loaded, so everything is gated on that.
bool runningInsideMinecraft() {
    const int descriptor = open("/proc/self/cmdline", O_RDONLY);
    if (descriptor < 0) return false;
    char command[256]{};
    const auto size = read(descriptor, command, sizeof(command) - 1);
    close(descriptor);
    if (size <= 0) return false;
    return std::strcmp(command, "com.mojang.minecraftpe") == 0 ||
           std::strcmp(command, "org.levimc.launcher:minecraft") == 0;
}

using SwapBuffersFn = unsigned int (*)(void*, void*);

SwapBuffersFn gSwapBuffersOriginal = nullptr;
pl::memory::HookHandle gSwapBuffersHook;

std::uint64_t nowMilliseconds() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

} // namespace

Runtime& Runtime::get() {
    static Runtime instance;
    return instance;
}

bool Runtime::load(pl::mod::ModContext& context) {
    mModId = context.id();
    mDataDirectory = context.dataDir();
    mConfigPath = context.configDir() / "config.json";
    mProfileOverridePath = context.configDir() / "signatures.json";
    mBundledProfilePath = context.resourceDir() / "signatures.json";

    ConfigStore::get().setPath(mConfigPath);

    // The profile must be ready before anything resolves a signature.
    Profile::get().load(mBundledProfilePath, mProfileOverridePath);
    log::report(Profile::get().describe());

    if (!runningInsideMinecraft()) {
        log::info("loaded outside the game process; hooks deferred");
        return true;
    }

    if (void* game = dlopen("libminecraftpe.so", RTLD_NOW | RTLD_NOLOAD)) {
        prepareGame();
        dlclose(game);
    } else {
        log::warn("libminecraftpe.so not mapped yet; will resolve on enable");
    }
    return true;
}

bool Runtime::prepareGame() {
    if (mInstalled) return true;

    Resolver::get().resolveAll("libminecraftpe.so");
    log::report(Resolver::get().summary());
    world::World::get().installBlockLookup();
    installHooks();
    mInstalled = true;
    return true;
}

bool Runtime::enable(pl::mod::ModContext&) {
    mEnabled = true;

    // Menu registration and the on-screen button are launcher services: they
    // talk to the Mod Menu bridge and nothing else, and touch no game state.
    // They therefore run before the game-process check below. Registration used
    // to sit after that check, which meant any process whose argv[0] did not
    // match the two expected strings silently registered nothing at all.
    modules::SelectorModule::get().registerInMenu(mModId);
    launcher::Picker::get().install(mModId);

    if (!runningInsideMinecraft()) {
        // Everything past this point needs libminecraftpe.so: signature
        // resolution, the update hook, the frame tick and the renderer. Say so
        // out loud, because an unregistered module now has exactly one remaining
        // explanation and it should not have to be guessed.
        log::warn("enable: not the Minecraft process; menu registered, game hooks deferred");
        return true;
    }

    if (!mInstalled) {
        if (void* game = dlopen("libminecraftpe.so", RTLD_NOW | RTLD_NOLOAD)) {
            prepareGame();
            dlclose(game);
        }
    }

    installFrameTick();

    ConfigStore::get().load([this](const nlohmann::json& document) {
        if (document.contains("selector") && document["selector"].is_object()) {
            modules::SelectorModule::get().loadConfig(document["selector"]);
        }
        if (document.contains("selection") && document["selection"].is_object()) {
            const auto& saved = document["selection"];
            const auto kind = saved.value("kind", std::string{});
            const auto id = saved.value("id", std::string{});
            // Re-resolve through the identifier layer instead of restoring the
            // saved Target verbatim: the class expansion may have grown since the
            // file was written, and the confidence label must be re-derived rather
            // than trusted from disk.
            if (kind == "block") {
                selection::Selection::get().selectBlock(id);
            } else if (kind == "entity") {
                selection::Selection::get().selectEntity(id);
            }
        }
    });

    // Restore the enabled state so the highlight survives a restart.
    modules::SelectorModule::get().setEnabled(true);
    return true;
}

bool Runtime::disable(pl::mod::ModContext&) {
    mEnabled = false;
    persistConfig();
    modules::SelectorModule::get().setEnabled(false);
    return true;
}

bool Runtime::unload(pl::mod::ModContext&) {
    mEnabled = false;
    persistConfig();
    modules::SelectorModule::get().setEnabled(false);
    launcher::Picker::get().uninstall();
    render::HighlightRenderer::get().uninstall();
    uninstallFrameTick();
    return true;
}

void Runtime::persistConfig() {
    nlohmann::json document;
    document["selector"] = modules::SelectorModule::get().saveConfig();

    const auto target = selection::Selection::get().current();
    if (target) {
        document["selection"]["kind"] = target.kind == selection::Kind::Block ? "block" : "entity";
        document["selection"]["id"] = target.id;
        document["selection"]["name"] = target.prettyName;
        // Recorded for support, not read back as authority.
        document["selection"]["confidence"] = identifier::confidenceName(target.confidence);
    }

    document["profile"] = Profile::get().gameVersion();
    ConfigStore::get().save(document);
}

void Runtime::installHooks() {
    // Per-tick game hook: the only source of a valid local player pointer.
    gamehooks::install([](void* player) { Runtime::get().onLocalPlayerTick(player); });

    // The keybind is delivered through the mod menu's own onKeybind hook, which
    // is registered in SelectorModule::registerInMenu and only fires for the key
    // the user bound to this module. Do *not* also listen on
    // pl::input::registerKeyCallback here: that callback sees every key the user
    // types, which would toggle the mod on any keystroke.
}

void Runtime::onLocalPlayerTick(void* player) {
    if (!player) return;
    mLastPlayer = player;

    // Camera position lives on the player renderer; when it is not reachable
    // yet the world layer falls back to the player's eye position, which is
    // correct in first person anyway.
    float camera[3]{};
    const bool hasCamera = gamehooks::cameraPosition(camera);
    modules::SelectorModule::get().onTick(player, nowMilliseconds(),
                                         hasCamera ? bes::sdk::Vec3{camera[0], camera[1], camera[2]}
                                                   : bes::sdk::Vec3{});
}

void swapBuffersDetour(void* display, void* surface) {
    static std::atomic<bool> inside{false};
    if (!inside.exchange(true, std::memory_order_acq_rel)) {
        Runtime::get().onFrame();
        inside.store(false, std::memory_order_release);
    }
    if (gSwapBuffersOriginal) gSwapBuffersOriginal(display, surface);
}

bool Runtime::installFrameTick() {
    if (mFrameTickInstalled) return true;
    // eglSwapBuffers is the frame boundary the mod draws against; ticking here
    // keeps the scan moving while the game is paused and guarantees the camera
    // sample is from the frame actually being rendered.
    void* library = dlopen("libEGL.so", RTLD_NOW | RTLD_NOLOAD);
    if (!library) {
        log::debug("frame tick: libEGL.so not loaded yet");
        return false;
    }
    const auto address = reinterpret_cast<void*>(dlsym(library, "eglSwapBuffers"));
    dlclose(library);
    if (!address) return false;

    auto* detour = reinterpret_cast<void*>(&swapBuffersDetour);
    if (pl::memory::hook(address, detour, reinterpret_cast<void**>(&gSwapBuffersOriginal)) != 0) {
        log::warn("frame tick: eglSwapBuffers hook rejected");
        return false;
    }
    gSwapBuffersHook = pl::memory::HookHandle(address, detour, reinterpret_cast<void**>(&gSwapBuffersOriginal));
    mFrameTickInstalled = true;
    log::info("frame tick: eglSwapBuffers hooked");
    return true;
}

void Runtime::uninstallFrameTick() {
    if (!mFrameTickInstalled) return;
    gSwapBuffersHook.reset();
    gSwapBuffersOriginal = nullptr;
    mFrameTickInstalled = false;
}

void Runtime::onFrame() {
    if (!mEnabled || !mLastPlayer) return;
    modules::SelectorModule::get().onTick(mLastPlayer, nowMilliseconds());
}

std::string Runtime::dumpDiagnostics() {
    std::ostringstream report;
    report << "Block Entity Selector " << bes::Version << " (" << bes::TargetGameVersion << ")\n";
    report << "mod id: " << mModId << '\n';
    report << "data dir: " << mDataDirectory.string() << '\n';
    report << "config: " << mConfigPath.string() << '\n';
    report << "bundled profile: " << mBundledProfilePath.string() << '\n';
    report << "override profile: " << mProfileOverridePath.string() << '\n';
    report << '\n' << Profile::get().describe();
    report << '\n' << Resolver::get().summary();
    report << "\nrenderer installed: " << (render::HighlightRenderer::get().installed() ? "yes" : "no") << '\n';
    report << "renderer frames: " << render::HighlightRenderer::get().frames() << '\n';
    report << "last draw vertices: " << render::HighlightRenderer::get().lastDrawVertices() << '\n';
    report << "selector enabled: " << (modules::SelectorModule::get().enabled() ? "yes" : "no") << '\n';
    report << "target: " << modules::SelectorModule::get().targetLabel() << '\n';

    // Scan state is what distinguishes "nothing found yet" from "nothing can be
    // found": a full pass with zero matches is a real answer.
    const auto& scanner = selection::Scanner::get();
    const auto settings = scanner.settings();
    const auto targets = scanner.targets();
    report << "scan: blocks=" << (settings.scanBlocks ? "on" : "off") << " entities="
           << (settings.scanEntities ? "on" : "off") << " players=" << (settings.includePlayers ? "yes" : "no")
           << " radius=" << settings.radius << " vertical=" << settings.verticalRadius
           << " entityRange=" << settings.entityRange << '\n';
    report << "matches: " << targets.blocks.size() << " block(s), " << targets.entityBoxes.size()
           << " entity box(es)\n";

    report << "blocktype probe runs: " << diag::BlockTypeProbe::get().dumpCount();
    if (!diag::BlockTypeProbe::get().lastReportPath().empty()) {
        report << "  last: " << diag::BlockTypeProbe::get().lastReportPath();
    }
    report << '\n';

    report << '\n' << identifier::ClassIndex::get().describe();

    const auto matched = selection::Selection::get().matchedClasses();
    if (!matched.empty()) {
        report << "matched classes:";
        for (const auto& name : matched) report << ' ' << name;
        report << '\n';
    }

    report << '\n' << log::takeReport();

    const auto path = mDataDirectory / "diagnostics.txt";
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (output) {
        const auto text = report.str();
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        log::info("diagnostics written to " + path.string());
    } else {
        log::warn("failed to write diagnostics to " + path.string());
    }
    return path.string();
}

} // namespace bes::core
