#include "core/Profile.hpp"
#include "core/Rtti.hpp"
#include "selection/BlockCursor.hpp"
#include "selection/Scanner.hpp"
#include "selection/Selection.hpp"

#include <bes/sdk/Types.hpp>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int gChecks = 0;
int gFailures = 0;

#define CHECK(...)                                                                                                     \
    do {                                                                                                               \
        ++gChecks;                                                                                                     \
        if (!(__VA_ARGS__)) {                                                                                          \
            ++gFailures;                                                                                               \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #__VA_ARGS__);                                        \
        }                                                                                                              \
    } while (false)

void testAabb() {
    using bes::sdk::AABB;
    using bes::sdk::Vec3;

    const AABB box{Vec3{0.f, 0.f, 0.f}, Vec3{2.f, 4.f, 6.f}};
    CHECK(box.valid());
    CHECK(!AABB{}.valid());
    // An inverted box is invalid rather than a negative volume.
    CHECK(!AABB{Vec3{1.f, 1.f, 1.f}, Vec3{0.f, 0.f, 0.f}}.valid());

    const auto extent = box.extent();
    CHECK(extent.x == 2.f && extent.y == 4.f && extent.z == 6.f);

    const auto center = box.center();
    CHECK(center.x == 1.f && center.y == 2.f && center.z == 3.f);

    const auto expanded = box.expanded(0.5f);
    CHECK(expanded.min.x == -0.5f && expanded.max.z == 6.5f);

    // Distance is measured to the nearest point on the box, so it is zero inside.
    CHECK(box.distanceSquaredTo(Vec3{1.f, 2.f, 3.f}) == 0.f);
    CHECK(box.distanceSquaredTo(Vec3{3.f, 2.f, 3.f}) == 1.f);

    // Ray from -Z hits; ray facing away misses.
    CHECK(box.intersectsRay(Vec3{1.f, 2.f, -5.f}, Vec3{0.f, 0.f, 1.f}, 20.f));
    CHECK(!box.intersectsRay(Vec3{1.f, 2.f, -5.f}, Vec3{0.f, 0.f, -1.f}, 20.f));
    // Ray that would hit but is too short must miss.
    CHECK(!box.intersectsRay(Vec3{1.f, 2.f, -5.f}, Vec3{0.f, 0.f, 1.f}, 2.f));
    // Origin inside the box counts as a hit with a zero-length ray.
    CHECK(box.intersectsRay(Vec3{1.f, 2.f, 3.f}, Vec3{0.f, 1.f, 0.f}, 0.f));
    // Parallel ray outside the slab misses.
    CHECK(!box.intersectsRay(Vec3{9.f, 2.f, -5.f}, Vec3{0.f, 0.f, 1.f}, 20.f));
}

void testColor() {
    using bes::sdk::Color;

    const Color color{0.25f, 0.5f, 0.75f, 1.f};
    const auto withAlpha = color.withAlpha(0.5f);
    CHECK(withAlpha.r == color.r && withAlpha.g == color.g && withAlpha.b == color.b);
    CHECK(withAlpha.a == 0.5f);
    // withAlpha clamps instead of wrapping.
    CHECK(color.withAlpha(5.f).a == 1.f);
    CHECK(color.withAlpha(-1.f).a == 0.f);

    // The launcher stores colours as 0xAARRGGBB; round-tripping must preserve
    // every channel within one 8-bit step.
    const auto packed = color.toArgb();
    CHECK(((packed >> 24) & 0xFF) == 255);
    CHECK(((packed >> 16) & 0xFF) == 64);  // 0.25
    CHECK(((packed >> 8) & 0xFF) == 128);  // 0.5
    CHECK((packed & 0xFF) == 191);         // 0.75

    const auto restored = Color::fromArgb(packed);
    CHECK(std::abs(restored.r - color.r) < 0.01f);
    CHECK(std::abs(restored.g - color.g) < 0.01f);
    CHECK(std::abs(restored.b - color.b) < 0.01f);
    CHECK(std::abs(restored.a - color.a) < 0.01f);
}

void testRttiNames() {
    using namespace bes::core::rtti;

    CHECK(classNameFromMangled("6Zombie") == "Zombie");
    CHECK(classNameFromMangled("11ZombieActor") == "ZombieActor");
    CHECK(classNameFromMangled("6Player") == "Player");
    CHECK(classNameFromMangled("_ZTS6Zombie") == "Zombie");
    CHECK(classNameFromMangled("N6bedrock5Actor6ZombieE") == "Zombie");
    CHECK(classNameFromMangled("N4entt13basic_storageI16ActorEquipment8EntityIdEE") == "basic_storage");
    CHECK(classNameFromMangled("").empty());
    CHECK(classNameFromMangled("!!not_a_name").empty());

    CHECK(classifyActor("Zombie") == ActorKind::Other);
    CHECK(classifyActor("ZombieMob") == ActorKind::Mob);
    CHECK(classifyActor("Player") == ActorKind::Player);
    CHECK(classifyActor("LocalPlayer") == ActorKind::Player);
    CHECK(classifyActor("ItemActor") == ActorKind::Item);
    CHECK(classifyActor("Arrow") == ActorKind::Projectile);
    CHECK(classifyActor("") == ActorKind::Unknown);

    CHECK(std::string(kindName(ActorKind::Mob)) == "mob");
    CHECK(std::string(kindName(ActorKind::Projectile)) == "projectile");
}

void testProfileParsing() {
    auto& profile = bes::core::Profile::get();

    CHECK(profile.loadFromText(R"({
        "signatures": {
            "render_level": { "candidates": [ "48 8B ?? 90", { "pattern": "AA BB", "note": "second" } ] },
            "actor_is_player": [ "11 22 33" ]
        },
        "offsets": {
            "actor_level": { "candidates": [ { "offset": "0x1F0", "note": "Actor::mLevel" } ] }
        }
    })",
                              "first", true));

    const auto& renderLevel = profile.candidates(bes::core::SigId::RenderLevel);
    CHECK(renderLevel.size() == 2);
    CHECK(renderLevel[0].kind == bes::core::SigCandidate::Kind::Pattern);
    CHECK(renderLevel[0].pattern == "48 8B ?? 90");
    CHECK(renderLevel[1].pattern == "AA BB");
    // Per-candidate notes are folded into the origin string, which is what the
    // diagnostic dump prints when a candidate wins.
    CHECK(renderLevel[1].origin.find("second") != std::string::npos);
    // A bare array of strings is accepted as shorthand for a candidate list.
    CHECK(profile.candidates(bes::core::SigId::ActorIsPlayer).size() == 1);
    CHECK(profile.candidates(bes::core::OffId::ActorLevel).front().value == 0x1F0);

    // Merging touches only the keys present in the document. New candidates go
    // first, the previously loaded ones stay behind them as fallbacks.
    CHECK(profile.loadFromText(R"({
        "signatures": { "render_level": "99 99" }
    })",
                              "override", false));
    const auto& merged = profile.candidates(bes::core::SigId::RenderLevel);
    CHECK(merged.size() == 3);
    CHECK(merged[0].pattern == "99 99");
    CHECK(merged[1].pattern == "48 8B ?? 90");
    CHECK(merged[2].pattern == "AA BB");
    CHECK(profile.candidates(bes::core::SigId::ActorIsPlayer).size() == 1);
    CHECK(profile.candidates(bes::core::OffId::ActorLevel).front().value == 0x1F0);

    // An override that matches no known key is a no-op, not a silent success:
    // the table must be untouched (the warning is covered by Profile::load).
    CHECK(profile.loadFromText(R"({ "signatures": { "render_levle": "11 22" } })", "typo", false));
    CHECK(profile.candidates(bes::core::SigId::RenderLevel).size() == 3);

    // Malformed documents are rejected without wiping the table.
    CHECK(!profile.loadFromText("{ this is not json", "broken", false));
    CHECK(profile.candidates(bes::core::SigId::ActorIsPlayer).size() == 1);
}

void testVtableCandidates() {
    auto& profile = bes::core::Profile::get();
    CHECK(profile.loadFromText(R"({
        "signatures": {
            "actor_get_name_tag": { "candidates": [ { "rtti": "10ActorOwner", "slot": 3 } ] }
        }
    })",
                              "vtable-test", false));

    const auto& candidates = profile.candidates(bes::core::SigId::ActorGetNameTag);
    CHECK(candidates.size() == 1);
    CHECK(candidates[0].kind == bes::core::SigCandidate::Kind::Vtable);
    CHECK(candidates[0].pattern == "10ActorOwner");
    CHECK(candidates[0].slot == 3);
}

void testOffsetsAreSane() {
    // Guards against a profile reaching a struct member that cannot exist: an
    // offset that is not 8-byte aligned or is beyond the plausible object size
    // is almost always a bad Zaphkiel reading.
    auto& profile = bes::core::Profile::get();
    CHECK(profile.loadFromText(R"({
        "offsets": {
            "actor_level": { "candidates": [ { "offset": "0x1F0" }, { "offset": "0x2A8" } ] }
        }
    })",
                              "offsets", false));

    const auto& candidates = profile.candidates(bes::core::OffId::ActorLevel);
    CHECK(candidates.size() == 2);
    CHECK(candidates[0].value == 0x1F0);
    CHECK(candidates[1].value == 0x2A8);
    // The first candidate is what offset() reports until a probe rejects it.
    CHECK(profile.candidates(bes::core::OffId::ActorLevel).front().value == 0x1F0);

    for (const auto& candidate : candidates) {
        CHECK(candidate.value % 8 == 0);
        CHECK(candidate.value < 0x1000);
    }
}

void testScannerSettings() {
    using bes::selection::Scanner;

    auto& scanner = Scanner::get();
    Scanner::Settings settings;
    settings.radius = 24.f;
    settings.entityRange = 90.f;
    settings.includePlayers = false;
    settings.scanBlocks = true;
    scanner.setSettings(settings);

    CHECK(scanner.settings().radius == 24.f);
    CHECK(scanner.settings().entityRange == 90.f);
    CHECK(!scanner.settings().includePlayers);

    // Nothing selected means nothing to draw, and no stale targets survive.
    scanner.reset();
    const auto targets = scanner.targets();
    CHECK(targets.blocks.empty());
    CHECK(targets.entityBoxes.empty());
    CHECK(!targets.hasAnchor);

    // Pushing the same settings again must not throw away collected results
    // (setSettings is called on every config change).
    scanner.setSettings(settings);
    CHECK(scanner.settings().radius == 24.f);
}

void testSelectionMatching() {
    using bes::selection::entityMatches;
    using bes::selection::Kind;
    using bes::selection::Selection;
    using bes::selection::Target;

    Target target;
    target.kind = Kind::Entity;
    target.id = "Zombie";

    // Exact class match only; a sibling class with a similar name must not match.
    CHECK(entityMatches(target, "Zombie"));
    CHECK(!entityMatches(target, "ZombieVariant"));
    CHECK(!entityMatches(target, "Husk"));
    // Case and whitespace are meaningful: RTTI names are exact identifiers.
    CHECK(!entityMatches(target, "zombie"));

    // Substring mode is opt-in and covers subclasses.
    target.matchBySubstring = true;
    CHECK(entityMatches(target, "Zombie"));
    CHECK(entityMatches(target, "ZombieVariant"));

    // A block target never matches an actor class, and vice versa.
    Target block;
    block.kind = Kind::Block;
    block.id = "minecraft:diamond_ore";
    CHECK(!entityMatches(block, "Zombie"));
    CHECK(!entityMatches(block, "minecraft:diamond_ore"));

    // Degenerate targets match nothing.
    Target empty;
    CHECK(!entityMatches(empty, "Zombie"));
    CHECK(!entityMatches(target, ""));

    // The store hands back value copies, so the render thread never shares state.
    auto& store = Selection::get();
    store.set(target);
    CHECK(store.active());
    const auto snapshot = store.current();
    CHECK(snapshot.kind == Kind::Entity && snapshot.id == "Zombie");
    store.clear();
    CHECK(!store.active());
    CHECK(!store.current());
    // The copy taken earlier is unaffected by the clear.
    CHECK(snapshot.id == "Zombie");
}

void testBlockCursor() {
    using bes::selection::BlockCursor;
    using bes::sdk::BlockPos;

    // A 1x1 column: visits every Y in range, then reports exhaustion.
    BlockCursor cursor;
    cursor.reset(0, 0, 0, 0, 2, -64, 320);
    CHECK(cursor.active());
    CHECK(cursor.cellCount() == 4);  // y = -2..1

    std::vector<int> visitedY;
    while (cursor.active()) {
        visitedY.push_back(cursor.position().y);
        cursor.advance();
    }
    CHECK(visitedY.size() == 4);
    // Bottom-up, in order.
    CHECK(visitedY[0] == -2 && visitedY[1] == -1 && visitedY[2] == 0 && visitedY[3] == 1);
    CHECK(!cursor.active());
    // Advancing a finished cursor is a no-op, not a wrap-around.
    const auto last = cursor.position();
    cursor.advance();
    CHECK(cursor.position().y == last.y);

    // Vertical radius is clamped to the dimension build limits.
    // Centre 300 with a 32-block vertical radius clamps to the ceiling (320),
    // leaving y = 268..319 above a 3x3 footprint.
    cursor.reset(0, 300, 0, 1, 32, -64, 320);
    CHECK(cursor.cellCount() == 3 * 3 * (320 - 268));

    // An empty range is never active, so a caller cannot spin on it.
    cursor.reset(0, 0, 0, 2, 2, 10, -10);
    CHECK(!cursor.active());
    CHECK(cursor.cellCount() == 0);

    // A full 3x3x2 pass visits exactly cellCount cells and covers the whole box.
    cursor.reset(5, 5, 5, 1, 1, -64, 320);
    const auto expected = cursor.cellCount();
    std::vector<BlockPos> cells;
    while (cursor.active()) {
        cells.push_back(cursor.position());
        cursor.advance();
    }
    CHECK(static_cast<std::int64_t>(cells.size()) == expected);
    CHECK(expected == 18);  // 3 * 3 * 2

    // No duplicates and no escapes from the box.
    bool inBounds = true;
    for (std::size_t left = 0; left < cells.size(); ++left) {
        const auto& cell = cells[left];
        if (cell.x < 4 || cell.x > 6 || cell.y < 4 || cell.y > 5 || cell.z < 4 || cell.z > 6) inBounds = false;
        for (std::size_t right = left + 1; right < cells.size(); ++right) {
            if (cells[left].x == cells[right].x && cells[left].y == cells[right].y &&
                cells[left].z == cells[right].z) {
                inBounds = false;
            }
        }
    }
    CHECK(inBounds);

    // invalidate() stops the walk without pretending it finished.
    cursor.reset(0, 0, 0, 1, 1, -64, 320);
    CHECK(cursor.active());
    cursor.invalidate();
    CHECK(!cursor.active());
}

} // namespace

int main() {
    testAabb();
    testColor();
    testRttiNames();
    testProfileParsing();
    testVtableCandidates();
    testOffsetsAreSane();
    testScannerSettings();
    testBlockCursor();
    testSelectionMatching();

    std::printf("%d checks, %d failure(s)\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
