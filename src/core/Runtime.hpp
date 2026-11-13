#pragma once

#include <pl/Mod.hpp>

#include <cstddef>
#include <filesystem>
#include <string>

namespace bes::core {

/// Mod lifecycle entry point, mirroring the BedrockTools runtime shape so the
/// two codebases stay familiar to work on.
class Runtime {
public:
    static Runtime& get();

    bool load(pl::mod::ModContext& context);
    bool enable(pl::mod::ModContext& context);
    bool disable(pl::mod::ModContext& context);
    bool unload(pl::mod::ModContext& context);

    /// Writes the diagnostic report next to the config and returns its path.
    std::string dumpDiagnostics();

    /// Per-frame work; called by the eglSwapBuffers detour.
    void onFrame();

    /// How many offset categories a live object has proven this session.
    static std::size_t provenOffsets();

    const std::filesystem::path& dataDirectory() const noexcept { return mDataDirectory; }
    const std::string& modId() const noexcept { return mModId; }

private:
    Runtime() = default;

    bool prepareGame();
    bool installHooks();
    void onLocalPlayerTick(void* player);
    bool installFrameTick();
    void uninstallFrameTick();
    void persistConfig();

    std::filesystem::path mDataDirectory;
    std::filesystem::path mConfigPath;
    std::filesystem::path mProfileOverridePath;
    std::filesystem::path mBundledProfilePath;
    std::string mModId;
    bool mEnabled = false;
    bool mInstalled = false;
    void* mLastPlayer = nullptr;
    bool mFrameTickInstalled = false;
};

}
