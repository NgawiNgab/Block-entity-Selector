#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace bes::core {

/// Every game function this mod needs. Nothing here is hardcoded to a build:
/// each entry is backed by an ordered list of candidates from the active profile.
enum class SigId : std::uint16_t {
    RenderLevel,
    TessellatorBegin,
    TessellatorColor,
    TessellatorVertex,
    RenderMeshImmediately,
    ActorManagerList,
    ActorGetNameTag,
    ActorIsPlayer,
    LevelGetHitResult,
    HitResultGetEntity,
    BlockSourceGetBlock,
    RenderMaterialGroup,
    ClientInstanceUpdate,
    ClientInstanceGetLocalPlayer,
    Count
};

inline constexpr std::size_t SigCount = static_cast<std::size_t>(SigId::Count);

const char* sigName(SigId id);

/// One way to locate a function.
struct SigCandidate {
    enum class Kind : std::uint8_t {
        Pattern,   ///< Byte pattern with `?` wildcards, scanned inside the module.
        Vtable,    ///< RTTI typeinfo name plus vtable slot, resolved through the Itanium ABI.
    };

    Kind kind = Kind::Pattern;
    std::string pattern;  ///< Pattern text, or RTTI name for Vtable candidates.
    std::size_t slot = 0;
    std::string origin;   ///< Where the candidate came from, for diagnostics.
};

/// Numeric struct offsets. Multiple candidates are probed against live objects.
enum class OffId : std::uint16_t {
    ClientInstanceLevelRenderer,
    LevelRendererPlayer,
    LevelRendererPlayerCamPos,
    ScreenContextTessellator,
    ScreenContextColorHolder,
    ActorStateVector,
    ActorAabbComponent,
    ActorLevel,
    ActorDimension,
    DimensionBlockSource,
    LevelActorManager,
    Count
};

inline constexpr std::size_t OffCount = static_cast<std::size_t>(OffId::Count);

const char* offName(OffId id);

struct OffCandidate {
    std::size_t value = 0;
    std::string origin;
};

/// Parsed profile: the JSON file shipped in resources, optionally overridden by
/// a user-supplied file written from Zaphkiel output.
class Profile {
public:
    static Profile& get();

    /// Loads the bundled defaults, then layers an override file on top if present.
    bool load(const std::filesystem::path& bundled, const std::filesystem::path& overridePath);

    /// Loads candidates from a single JSON document.
    /// `replaceAll` wipes the current table first; otherwise only the keys the
    /// document actually defines are replaced (that is how an override file
    /// layers on top of the bundled defaults).
    bool loadFromFile(const std::filesystem::path& path, bool replaceAll);

    bool loadFromText(std::string_view json, std::string_view originLabel, bool replaceAll);

    const std::vector<SigCandidate>& candidates(SigId id) const;
    const std::vector<OffCandidate>& candidates(OffId id) const;

    const std::string& gameVersion() const noexcept { return mGameVersion; }
    const std::string& sourceLabel() const noexcept { return mSource; }
    bool loaded() const noexcept { return mLoaded; }

    /// Human-readable summary used by the diagnostic dump.
    std::string describe() const;

private:
    void clear();

    std::vector<SigCandidate> mSignatures[SigCount];
    std::vector<OffCandidate> mOffsets[OffCount];
    std::string mGameVersion;
    std::string mSource;
    bool mLoaded = false;
};

}
