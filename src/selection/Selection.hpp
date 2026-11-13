#pragma once

#include "selection/Identifier.hpp"

#include <bes/sdk/Types.hpp>

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace bes::selection {

enum class Kind : std::uint8_t {
    None,
    Block,
    Entity,
};

/// What the user is currently highlighting. Stored as a value type so the
/// render thread never touches pointers owned by the tick thread.
struct Target {
    Kind kind = Kind::None;
    /// Canonical identifier the user chose: "minecraft:diamond_ore" or
    /// "minecraft:zombie". Always normalised before it lands here.
    std::string id;
    std::string prettyName;  ///< what the module menu / toast shows
    int dimensionId = 0;
    /// How much is actually known about this identifier. The UI reports it, and
    /// the scanner never claims more than it does.
    identifier::Confidence confidence = identifier::Confidence::Unverified;
    /// Entity only: every RTTI class this identifier expands to (1:Many). Filled
    /// from the identifier index, never from a guess. Empty means "no class
    /// mapping yet", which is a legitimate, reportable state.
    std::vector<std::string> classes;
    /// Entity only: also match subclasses whose class name contains one of
    /// `classes`. Off by default so a mapping is exact unless asked otherwise.
    bool matchBySubstring = false;

    explicit operator bool() const noexcept { return kind != Kind::None && !id.empty(); }

    void clear() {
        kind = Kind::None;
        id.clear();
        prettyName.clear();
        confidence = identifier::Confidence::Unverified;
        classes.clear();
        matchBySubstring = false;
    }
};

/// Resolved positions/boxes for one frame, handed to the renderer.
struct FrameTargets {
    std::vector<bes::sdk::BlockPos> blocks;
    std::vector<bes::sdk::AABB> entityBoxes;
    /// Parallel to entityBoxes: true when that box belongs to a player, so the
    /// renderer can colour players differently from mobs.
    std::vector<char> entityIsPlayer;
    bes::sdk::BlockPos anchor{};   ///< used to sort blocks by distance
    bool hasAnchor = false;
};

/// Thread-safe selection state shared between the tick thread and the render hook.
class Selection {
public:
    static Selection& get();

    void set(Target target);
    Target current() const;
    void clear();

    bool active() const;

    /// Selects a block by identifier. The identifier is normalised first; an
    /// invalid one is rejected and the previous selection is left alone.
    bool selectBlock(const std::string& rawIdentifier);

    /// Selects an entity by identifier, expanding it through the class index.
    /// Returns false (and changes nothing) when the identifier is invalid.
    bool selectEntity(const std::string& rawIdentifier);

    /// Convenience for the on-screen picker: selects an entity by an RTTI class
    /// name observed on a live actor. This is *evidence*, so the identifier is
    /// recorded as ObservedLive rather than as a documented mapping.
    bool selectEntityFromClass(const std::string& className);

    /// Clears the selection when `id` is already selected, otherwise selects it.
    /// Used by both the picker and the menu's own select buttons.
    bool toggleBlock(const std::string& blockId, const std::string& prettyName = {});
    bool toggleEntity(const std::string& entityId, const std::string& prettyName = {});

    /// Cached entity selection helpers, refreshed by the tick loop.
    void setMatchedClasses(std::vector<std::string> classes);
    std::vector<std::string> matchedClasses() const;

    /// Incremented whenever the selection changes; the renderer uses it to know
    /// when to rebuild its cached geometry.
    std::uint64_t revision() const;

private:
    Selection() = default;

    mutable std::mutex mMutex;
    Target mTarget{};
    std::vector<std::string> mMatchedClasses;
    std::uint64_t mRevision = 1;
};

/// True when a live actor class name matches the selected entity target.
bool entityMatches(const Target& target, const std::string& className);

}
