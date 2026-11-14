#include "core/Runtime.hpp"

#include "diag/BlockTypeProbe.hpp"

#include "core/ConfigStore.hpp"
#include "core/GameEvidence.hpp"
#include "core/GameHooks.hpp"
#include "core/Logging.hpp"
#include "core/Profile.hpp"
#include "core/Resolver.hpp"
#include "core/Rtti.hpp"
#include "launcher/HudNotice.hpp"
#include "launcher/Picker.hpp"
#include "modules/SelectorModule.hpp"
#include "render/HighlightRenderer.hpp"
#include "selection/Identifier.hpp"
#include "selection/Scanner.hpp"
#include "selection/Selection.hpp"
#include "world/World.hpp"

#include <bes/Version.hpp>
#include <bes/sdk/AddressSpace.hpp>

#include <pl/Input.hpp>
#include <pl/ModMenu.hpp>

#include <pl/memory/Hook.hpp>

#include <dlfcn.h>

#include <atomic>

#include <chrono>
#include <fstream>
#include <sstream>

namespace bes::core {
namespace {

/// True when the game's image is mapped into this process.
///
/// This used to compare argv[0] against two hardcoded strings. That gate was
/// wrong twice over: Android sets `cmdline` inconsistently across zygote forks
/// and may leave it empty, and the string it was asked to match has no bearing on
/// whether the library is loaded. The evidence is now the mapping itself.
bool runningInsideMinecraft() {
    auto& space = sdk::AddressSpace::get();
    if (space.regionCount() == 0) space.refresh();
    return space.moduleLoaded("libminecraftpe.so");
}

using SwapBuffersFn = unsigned int (*)(void*, void*);

SwapBuffersFn gSwapBuffersOriginal = nullptr;
pl::memory::HookHandle gSwapBuffersHook;

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
    //
    // No log::report of describe() here: dumpDiagnostics prints the profile and
    // the resolver summary itself, so reporting them into the log buffer as well
    // made every dump carry ~40 duplicated lines. The log is for events, the
    // report body is for state.
    Profile::get().load(mBundledProfilePath, mProfileOverridePath);

    const auto evidence = EvidenceCollector::collect();
    log::info(std::string("load: libminecraftpe.so ") +
              (evidence.libraryMapped ? "mapped" : "NOT mapped") + " (maps=" +
              (evidence.libraryPath.empty() ? std::string("-") : evidence.libraryPath) + ", dlopen=" +
              (evidence.loaderSeesLibrary ? "yes" : "no") + ", cmdline=\"" + evidence.processName +
              "\", comm=\"" + evidence.processCom + "\")");

    if (!evidence.libraryMapped) {
        log::info("load: game library not present; hooks deferred to enable()");
        return true;
    }

    prepareGame();
    return true;
}

bool Runtime::prepareGame() {
    if (mInstalled) return true;

    Resolver::get().resolveAll("libminecraftpe.so");
    // No log::report(summary()) here: the summary is state, not an event, and
    // dumpDiagnostics already prints it. Reporting it as well duplicated the
    // whole block (~40 lines) into every dump. resolveAll() itself logs the
    // one-line outcome that the log genuinely needs.
    world::World::get().installBlockLookup();

    // RTTI reads dereference a vptr and the typeinfo it points at. Back that with
    // the memory map so a wrong object yields an empty name rather than a fault:
    // every offset probe in World.cpp now gates on a class name, so this is on
    // the path of a candidate being rejected, which is exactly where bad pointers
    // arrive.
    rtti::gTypeInfoReadable = [](const void* address, std::size_t size) {
        auto& space = sdk::AddressSpace::get();
        if (space.regionCount() == 0) space.refresh();
        return space.readable(address, size);
    };

    const bool hooksInstalled = installHooks();
    // Only claim to be installed when the per-tick hook actually landed. A wrong
    // flag here hides the single most consequential failure in the whole mod.
    mInstalled = hooksInstalled;
    return mInstalled;
}

bool Runtime::enable(pl::mod::ModContext&) {
    mEnabled = true;

    // Menu registration and the on-screen button are launcher services: they
    // talk to the Mod Menu bridge and nothing else, and touch no game state.
    // They therefore run before the game-presence check below. Registration used
    // to sit after that check, which meant a process the check rejected silently
    // registered nothing at all.
    modules::SelectorModule::get().registerInMenu(mModId);
    launcher::Picker::get().install(mModId);

    // 1. Presence. Decided by the mapping, not by a process name.
    const auto evidence = EvidenceCollector::collect();
    if (!evidence.libraryMapped) {
        // Nothing past this point can work, and the reason is presence, not
        // resolution -- say which, so the report is not read as a pattern failure.
        log::warn("enable: libminecraftpe.so is not mapped in this process (cmdline=\"" +
                  evidence.processName + "\", comm=\"" + evidence.processCom + "\", dlopen=" +
                  (evidence.loaderSeesLibrary ? "yes" : "no") + "); hooks deferred");
        log::info("enable: game hooks NOT installed (library absent)");
        return true;
    }

    // 2. Resolution + 3. hooks. prepareGame() performs both and reports its own
    //   outcome; the flag it sets reflects the hook, never the library.
    if (!mInstalled) {
        prepareGame();
    }

    log::info(std::string("enable: game hooks ") + (mInstalled ? "installed" : "NOT installed"));
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
    // Order matters: stop the tick before publishing "disabled", or the frame
    // thread can enter onTick between the two and read state this is tearing down.
    persistConfig();
    modules::SelectorModule::get().setEnabled(false);
    // The frame hook is the one tick driver left, so leaving it installed while
    // disabled means the render thread keeps entering our detour every frame for
    // nothing. It used to be removed only in unload(), which is why toggling off
    // and on again behaved differently from a fresh start.
    uninstallFrameTick();
    // A stale player pointer from a world that has since unloaded must not
    // survive into the next enable(): the offsets it supports were proven against
    // that world's objects.
    mLastPlayer = nullptr;
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

bool Runtime::installHooks() {
    // Per-tick game hook: the only source of a valid local player pointer.
    // Its failure mode is the important one -- without it there is no player, no
    // snapshot, no scan and no blocks -- so the result is propagated instead of
    // discarded.
    const bool tickHookInstalled =
        gamehooks::install([](void* player) { Runtime::get().onLocalPlayerTick(player); });

    // The keybind is delivered through the mod menu's own onKeybind hook, which
    // is registered in SelectorModule::registerInMenu and only fires for the key
    // the user bound to this module. Do *not* also listen on
    // pl::input::registerKeyCallback here: that callback sees every key the user
    // types, which would toggle the mod on any keystroke.
    return tickHookInstalled;
}

void Runtime::onLocalPlayerTick(void* player) {
    if (!player) return;
    mLastPlayer = player;
    // Deliberately does NOT tick the module. See onFrame().
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

std::size_t Runtime::provenOffsets() {
    std::size_t proven = 0;
    for (std::size_t index = 0; index < OffCount; ++index) {
        if (Resolver::get().offsetValidated(static_cast<OffId>(index))) ++proven;
    }
    return proven;
}

void Runtime::onFrame() {
    // The single tick driver.
    //
    // This used to be driven from two places: the ClientInstance update hook (game
    // thread) and this frame hook (render thread). Both called into World::update,
    // so one thread could rewrite the snapshot while the other was reading it.
    // The tombstone shows the result -- the crash landed on "Rendering Pool" in
    // World::update, with the update-thread tick as the other candidate driver.
    //
    // The frame hook is the surviving driver: it runs on one thread, it is the
    // frame the highlight is drawn against, and it keeps ticking while the game is
    // paused, which the update hook does not.
    // The HUD notice is driven from the frame hook, not from the tick below it.
    // The launcher's draw-command list is per-frame state, and this hook runs on
    // the thread the overlay is composited on, so text submitted here lands in the
    // frame it was submitted for. Gating it behind `mEnabled` would also mean a
    // disabled module could never explain itself.
    if (!mLastPlayer) return;

    const auto now = nowMilliseconds();
    launcher::HudNotice::get().poll(modules::SelectorModule::get().moduleId(), now);

    if (!mEnabled) return;

    // Sample the camera here, on the frame being drawn. `cameraPosition()` is the
    // only consumer of the ClientInstance -> LevelRenderer -> LevelRendererPlayer
    // -> cam-pos chain, so it is also where those three offsets prove themselves.
    // It returns false until the whole chain validates, and the world layer then
    // falls back to the player's eye position, which is correct in first person.
    float camera[3]{};
    const bool hasCamera = gamehooks::cameraPosition(camera);
    modules::SelectorModule::get().onTick(mLastPlayer, now,
                                         hasCamera ? bes::sdk::Vec3{camera[0], camera[1], camera[2]}
                                                   : bes::sdk::Vec3{});
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
    // The three states are reported separately and in order, because each one can
    // fail on its own and they are not interchangeable: a mapped library does not
    // mean the signatures resolved, and resolved signatures do not mean a hook
    // installed. Reading them top to bottom localises the break in one glance.
    const auto evidence = EvidenceCollector::collect();

    report << "\n== startup ==\n";
    report << "1. library mapped:   " << (evidence.libraryMapped ? "yes" : "NO") << '\n';
    if (!evidence.libraryPath.empty()) report << "   path:             " << evidence.libraryPath << '\n';
    report << "   loader (NOLOAD):  " << (evidence.loaderSeesLibrary ? "found" : "not found");
    if (!evidence.loaderSeesLibrary && !evidence.loaderNote.empty()) {
        report << "  (" << evidence.loaderNote << ')';
    }
    report << '\n';
    report << "2. signatures:       " << Resolver::get().resolvedCount() << '/' << SigCount << " resolved\n";
    // Offsets are a separate question from signatures and a separate question from
    // hooks. An unproven offset is not dereferenced at all now, so this number is
    // the difference between "the mod is idle because nothing matched" and "the
    // mod is idle because nothing has been proven on this build yet".
    report << "   offsets proven:   " << provenOffsets() << '/' << OffCount << '\n';
    // A truncated memory map makes every guarded read answer "unmapped", which is
    // indistinguishable from a wrong offset unless it is stated. Say it out loud.
    {
        auto& space = sdk::AddressSpace::get();
        if (space.regionCount() == 0) space.refresh();
        report << "   memory map:       " << space.regionCount() << " region(s)";
        if (space.truncated()) {
            report << "  TRUNCATED -- probe results below are unreliable";
        }
        report << '\n';
    }
    // Name the blocker. "offsets proven: 0/11" is a number; what a reader needs is
    // which of the three goals it currently blocks, so that a dump with no visible
    // effect is not mistaken for a mod that is doing something and finding nothing.
    {
        auto& resolver = Resolver::get();
        const bool actorsReady = resolver.offsetValidated(OffId::ActorLevel) &&
                                 resolver.offsetValidated(OffId::ActorDimension) &&
                                 resolver.offsetValidated(OffId::LevelActorManager);
        const bool blocksReady = actorsReady && resolver.offsetValidated(OffId::DimensionBlockSource);
        const bool drawReady = resolver.offsetValidated(OffId::ScreenContextTessellator) &&
                               resolver.offsetValidated(OffId::ScreenContextColorHolder) &&
                               resolver.offsetValidated(OffId::LevelRendererPlayer) &&
                               resolver.offsetValidated(OffId::LevelRendererPlayerCamPos);
        // RTTI is the gate every offset probe passes through: level, dimension,
        // actor-manager and every entity's class name all come from it. When it
        // stops working nothing reports an error -- the probes simply never
        // confirm -- so the fact that the reader can see a live class at all is
        // worth stating outright.
        const auto playerForRtti = mLastPlayer;
        const auto playerClass = playerForRtti ? rtti::className(playerForRtti) : std::string{};
        report << "   rtti read:        "
               << (playerForRtti ? (playerClass.empty() ? "FAILED (no class name from the local player)"
                                                      : "ok (" + playerClass + ")")
                                 : std::string("no local player yet"))
               << '\n';
        report << "   blocked:          ";
        if (actorsReady && blocksReady && drawReady) {
            report << "nothing -- all three goals have their offsets\n";
        } else {
            if (!actorsReady) report << "entities(actor offsets) ";
            if (!blocksReady) report << "blocks(block source) ";
            if (!drawReady) report << "drawing(tessellator/camera) ";
            report << '\n';
        }
    }
    report << "3. game hooks:       " << (mInstalled ? "installed" : "NOT installed") << '\n';
    report << "   frame tick:       " << (mFrameTickInstalled ? "installed" : "NOT installed") << '\n';
    report << "   local player:     " << (mLastPlayer ? "yes" : "NOT yet seen") << '\n';

    // Identity only: these never gate anything, they explain a surprising result.
    report << "   cmdline:          \"" << evidence.processName << "\"\n";
    report << "   comm:             \"" << evidence.processCom << "\"\n";
    report << "\nrenderer installed: " << (render::HighlightRenderer::get().installed() ? "yes" : "no") << '\n';
    {
        auto& renderer = render::HighlightRenderer::get();
        report << "renderer frames: " << renderer.frames() << '\n';
        report << "last draw vertices: " << renderer.lastDrawVertices() << '\n';
        // frames() only counts successful draws, so on its own it cannot say
        // whether rendering is broken or simply has nothing to draw. The last
        // stage says where the attempt stopped.
        report << "renderer stage: " << render::renderStageName(renderer.lastStage()) << '\n';
        // Read once: install() and selfTest() may update it between two calls.
        const char* error = renderer.lastError();
        report << "renderer error: " << (error[0] ? error : "none") << '\n';
    }
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

    // peek, not take: a diagnostics dump is read-only. Taking the buffer emptied
    // it, so a second dump lost the startup lines that explain the first one.
    report << '\n' << log::peekReport();

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
