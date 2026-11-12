#pragma once

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
    std::string id;          ///< block id ("minecraft:diamond_ore") or entity class name ("Zombie")
    std::string prettyName;  ///< what the module menu / toast shows
    int dimensionId = 0;
    bool matchBySubstring = false; ///< entity selection covers subclasses/derived names

    explicit operator bool() const noexcept { return kind != Kind::None && !id.empty(); }

    void clear() {
        kind = Kind::None;
        id.clear();
        prettyName.clear();
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

    /// Toggles a block: selecting the same block again clears the selection.
    bool toggleBlock(const std::string& blockId, const std::string& prettyName);

    /// Toggles an entity class. `classes` lists every live class name that was
    /// merged into this selection so toggling is symmetric with the picker.
    bool toggleEntity(const std::string& className, const std::string& prettyName);

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
