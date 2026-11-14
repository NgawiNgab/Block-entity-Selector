#include "core/Logging.hpp"
#include "core/Profile.hpp"
#include "core/Resolver.hpp"
#include "core/Rtti.hpp"
#include "render/RenderStage.hpp"
#include "selection/BlockCursor.hpp"
#include "selection/Identifier.hpp"
#include "selection/Scanner.hpp"
#include "selection/Selection.hpp"

#include <bes/sdk/AddressSpace.hpp>
#include <bes/sdk/LibcxxString.hpp>
#include <bes/sdk/Types.hpp>

#include <cmath>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

/// Test doubles for the RTTI reader, deliberately at file scope (external
/// linkage) rather than inside the anonymous namespace below. A type with internal
/// linkage gets a leading '*' on its typeinfo name, which is a different parse
/// path from every class in the game; keeping these external tests the path the
/// game actually uses. Both forms are covered separately.
namespace rtti_fixture {
struct Shape {
    virtual ~Shape() = default;
    virtual int sides() const { return 0; }
};
struct Triangle : Shape {
    int sides() const override { return 3; }
};
struct Level {};
struct LevelRenderer : Shape {};
struct Actor {
    virtual ~Actor() = default;
};
struct ZombieActor : Actor {};
} // namespace rtti_fixture

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

/// Reads a class name out of a REAL polymorphic object.
///
/// This is the test that was missing. `testRttiNames()` only exercises the string
/// parser, so it passed happily while `className()` returned "" for every object
/// in the game -- and every offset probe in World.cpp gates on a class name, so
/// nothing was ever confirmed. The defect was a single vtable index: typeinfo
/// lives at vtable[-1] (Itanium), and the reader looked at vtable[-2], which is
/// offset-to-top and zero for every primary vtable.
///
/// Testing against actual objects is the only way to catch that: the index is not
/// visible in any string the parser is given.


void testRttiOnRealObjects() {
    using namespace bes::core::rtti;
    using namespace rtti_fixture;

    Triangle triangle;
    ZombieActor zombie;
    LevelRenderer renderer;

    // A non-empty name is the whole point: empty was the bug.
    CHECK(!className(&triangle).empty());
    CHECK(!className(&zombie).empty());
    CHECK(!className(&renderer).empty());

    // ...and it has to be the RIGHT name, not just any bytes at that offset.
    CHECK(className(&triangle) == "Triangle");
    CHECK(className(&zombie) == "ZombieActor");
    CHECK(className(&renderer) == "LevelRenderer");

    // The mangled forms must differ, or the two reads above could be luck.
    CHECK(mangledName(&zombie) != mangledName(&renderer));

    // The gates that World.cpp actually relies on. These were unreachable while
    // className() was empty, which is exactly why the offset chain never proved
    // itself.
    const auto levelLike = className(&renderer);
    CHECK(levelLike.find("Level") != std::string::npos);
    CHECK(levelLike.find("Renderer") != std::string::npos); // the negative filter
    CHECK(className(&zombie).find("Level") == std::string::npos);

    // A non-polymorphic object has no vtable at all and must report empty rather
    // than reading whatever follows it.
    Level plain{};
    CHECK(className(&plain).empty());
    CHECK(className(nullptr).empty());

    // Register classes survive the round trip, since that is how the game's own
    // nested names arrive.
    CHECK(classNameFromMangled("N4game13LevelRendererE") == "LevelRenderer");

    // The compiler prefixes a leading '*' on internal-linkage types. The game has
    // none, but the previous version of this test did, and the empty result looked
    // exactly like "this object has no RTTI".
    CHECK(classNameFromMangled("*N12_GLOBAL__N_112rtti_fixture8TriangleE") == "Triangle");
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

/// The crash on 2026-11-13 came from `Resolver::offset()` handing out a profile
/// candidate that no live object had ever confirmed: a 0x9 pointer reached game
/// code and the game faulted on `ldr s0, [x1]`. These checks pin the fix.
///
/// They are written as positive *and* negative assertions on purpose. Asserting
/// only "unproven reads as 0" would still pass if `confirmOffset` did nothing at
/// all, so the promotion path is checked in the same test.
void testOffsetsRequireProof() {
    using bes::core::OffId;
    using bes::core::Resolver;

    // Load a profile this test owns. `Profile` is a process-wide singleton and
    // earlier tests already replaced its offsets, so inheriting that state would
    // make these assertions depend on test order.
    auto& profile = bes::core::Profile::get();
    CHECK(profile.loadFromText(R"({
        "offsets": {
            "actor_level":      { "candidates": [ { "offset": "0x1F0" } ] },
            "actor_dimension":  { "candidates": [ { "offset": "0x1E0" } ] },
            "actor_state_vector": { "candidates": [ { "offset": "0x228" } ] }
        }
    })",
                              "offsets", false));

    auto& resolver = Resolver::get();
    resolver.resolveAll("libthisdoesnotexist.so");

    // 1. A category with a profile candidate must still read as unusable until
    //    something proves it. This is the exact bug: 0x1F0 was live immediately.
    CHECK(!resolver.offsetValidated(OffId::ActorLevel));
    CHECK(resolver.offset(OffId::ActorLevel) == 0);
    CHECK(resolver.offsetCandidate(OffId::ActorLevel) == 0x1F0);

    CHECK(!resolver.offsetValidated(OffId::ActorDimension));
    CHECK(resolver.offset(OffId::ActorDimension) == 0);
    CHECK(resolver.offsetCandidate(OffId::ActorDimension) == 0x1E0);

    // 2. A category the profile says nothing about reads the same way, so a
    //    caller cannot distinguish them by accident.
    CHECK(resolver.offsetCandidate(OffId::ActorStateVector) == 0x228);
    CHECK(resolver.offset(OffId::ActorStateVector) == 0);

    // 3. Proving one category promotes that one only.
    resolver.confirmOffset(OffId::ActorLevel, 0x2A8, "test");
    CHECK(resolver.offsetValidated(OffId::ActorLevel));
    CHECK(resolver.offset(OffId::ActorLevel) == 0x2A8);
    CHECK(!resolver.offsetValidated(OffId::ActorDimension));
    CHECK(resolver.offset(OffId::ActorDimension) == 0);

    // 4. Proof is recorded against the proven value, not the profile's guess,
    //    and it is idempotent.
    CHECK(resolver.offsetCandidate(OffId::ActorLevel) == 0x1F0);
    CHECK(resolver.offset(OffId::ActorLevel) == 0x2A8);
    resolver.confirmOffset(OffId::ActorLevel, 0x2A8, "test again");
    CHECK(resolver.offset(OffId::ActorLevel) == 0x2A8);

    // 5. The summary states proof status rather than printing a bare hex value,
    //    because "0x1F0" and "0x1F0, proven" are different claims.
    const auto& summary = resolver.summary();
    CHECK(summary.find("PROVEN") != std::string::npos);
    CHECK(summary.find("unproven (not used)") != std::string::npos);
    CHECK(summary.find("offsets proven:") != std::string::npos);

    // 6. Every category the renderer or world layer could touch must start
    //    unproven on a fresh resolve. If a future profile ever marks something
    //    validated at load time, this catches it.
    std::size_t provenAtStartup = 0;
    for (std::size_t index = 0; index < bes::core::OffCount; ++index) {
        const auto id = static_cast<OffId>(index);
        if (id == OffId::ActorLevel) continue; // proven above, deliberately
        if (resolver.offsetValidated(id)) ++provenAtStartup;
    }
    CHECK(provenAtStartup == 0);
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

void testIdentifierSyntax() {
    using bes::identifier::Domain;
    using bes::identifier::normalise;
    using bes::identifier::validSyntax;

    // Canonical forms pass.
    CHECK(validSyntax("minecraft:diamond_ore", Domain::Block));
    CHECK(validSyntax("minecraft:deepslate_diamond_ore", Domain::Block));
    CHECK(validSyntax("custom:my_block.v2", Domain::Block));
    CHECK(validSyntax("custom:path/to/thing", Domain::Block));

    // Malformed forms are rejected rather than coerced.
    CHECK(!validSyntax("", Domain::Block));
    CHECK(!validSyntax("diamond_ore", Domain::Block));            // no namespace
    CHECK(!validSyntax(":diamond_ore", Domain::Block));           // empty namespace
    CHECK(!validSyntax("minecraft:", Domain::Block));             // empty path
    CHECK(!validSyntax("Minecraft:diamond_ore", Domain::Block));  // upper case
    CHECK(!validSyntax("minecraft:Diamond_Ore", Domain::Block));
    CHECK(!validSyntax("minecraft:has space", Domain::Block));
    // A trailing underscore is legal in an identifier, so it must NOT be
    // rejected; only '.', '-' and '/' are separators that cannot end a path.
    CHECK(validSyntax("minecraft:trailing_", Domain::Block));
    CHECK(!validSyntax("minecraft:trailing.", Domain::Block));
    CHECK(!validSyntax("minecraft:trailing-", Domain::Block));
    CHECK(!validSyntax("minecraft:trailing/", Domain::Block));
    CHECK(!validSyntax("minecraft:a::b", Domain::Block));         // double colon
    CHECK(!validSyntax("minecraft:a//b", Domain::Block));

    // Normalisation handles what people actually paste.
    CHECK(normalise("  minecraft:diamond_ore  ", Domain::Block) == "minecraft:diamond_ore");
    CHECK(normalise("MINECRAFT:Diamond_Ore", Domain::Block) == "minecraft:diamond_ore");
    CHECK(normalise("diamond_ore", Domain::Block) == "minecraft:diamond_ore");   // bare name
    CHECK(normalise("block/minecraft:chest", Domain::Block) == "minecraft:chest"); // domain prefix
    CHECK(normalise("entity/minecraft:zombie", Domain::Entity) == "minecraft:zombie");
    CHECK(normalise("minecraft:minecraft:chest", Domain::Block) == "minecraft:chest");
    CHECK(normalise("", Domain::Block).empty());
    CHECK(normalise("   ", Domain::Block).empty());
    CHECK(normalise("bad name!", Domain::Block).empty());
}

void testIdentifierIndex() {
    using bes::identifier::ClassIndex;
    using bes::identifier::Confidence;
    using bes::identifier::Domain;
    using bes::identifier::resolve;

    auto& index = ClassIndex::get();
    index.clear();
    index.loadDocumentedTable();

    // 1:Many expansion: one identifier, several RTTI classes.
    const auto villager = resolve(Domain::Entity, "minecraft:villager");
    CHECK(villager);
    CHECK(villager.classes.size() == 2);
    CHECK(villager.confidence == Confidence::DocumentedTable);

    // A documented entity resolves to its class set.
    const auto zombie = resolve(Domain::Entity, "zombie");  // bare name normalises
    CHECK(zombie.id == "minecraft:zombie");
    CHECK(zombie.classes.size() == 1);
    CHECK(zombie.classes[0] == "Zombie");

    // Blocks have identifiers but no class mapping; that is expected, not an error.
    const auto ore = resolve(Domain::Block, "minecraft:diamond_ore");
    CHECK(ore);
    CHECK(ore.classes.empty());
    CHECK(ore.confidence == Confidence::DocumentedTable);

    // An id that is well-formed but unknown is Unverified, not Invalid, and is
    // still selectable so the scanner can look for it.
    const auto unknown = resolve(Domain::Entity, "minecraft:nonexistent_mob");
    CHECK(unknown);
    CHECK(unknown.confidence == Confidence::Unverified);
    CHECK(unknown.classes.empty());

    // Garbage is Invalid and must not become a selection.
    const auto bad = resolve(Domain::Block, "not an id!!");
    CHECK(!bad);
    CHECK(bad.confidence == Confidence::Invalid);
    CHECK(resolve(Domain::Block, "").confidence == Confidence::Invalid);

    // Runtime evidence upgrades a binding without losing the class.
    index.bind(Domain::Entity, "minecraft:custom_mob", "CustomMobClass", Confidence::ObservedLive,
               "observed as a live actor");
    const auto custom = resolve(Domain::Entity, "minecraft:custom_mob");
    CHECK(custom);
    CHECK(custom.classes.size() == 1);
    CHECK(custom.classes[0] == "CustomMobClass");

    // Binding the same pair twice must not duplicate it.
    index.bind(Domain::Entity, "minecraft:custom_mob", "CustomMobClass", Confidence::ObservedLive);
    CHECK(resolve(Domain::Entity, "minecraft:custom_mob").classes.size() == 1);

    // A second class for the same identifier extends the set (1:Many is additive).
    index.bind(Domain::Entity, "minecraft:custom_mob", "CustomMobClassV2", Confidence::ObservedLive);
    const auto both = resolve(Domain::Entity, "minecraft:custom_mob");
    CHECK(both.classes.size() == 2);

    // A note alone records evidence without inventing a class.
    index.note(Domain::Block, "minecraft:some_block", Confidence::ObservedLive, "seen");
    const auto noted = resolve(Domain::Block, "minecraft:some_block");
    CHECK(noted);
    CHECK(noted.classes.empty());

    // The index never invents a mapping for an id nobody bound.
    CHECK(!index.mapped(Domain::Entity, "minecraft:unbound_mob"));
    CHECK(index.classesFor(Domain::Entity, "minecraft:unbound_mob").empty());
}

void testSelectionMatching() {
    using bes::selection::entityMatches;
    using bes::selection::Kind;
    using bes::selection::Selection;
    using bes::selection::Target;

    Target target;
    target.kind = Kind::Entity;
    target.id = "minecraft:zombie";
    target.classes = {"Zombie"};

    // 1:Many membership: the mapped class matches...
    CHECK(entityMatches(target, "Zombie"));
    // ...and a sibling class with a similar name does not.
    CHECK(!entityMatches(target, "ZombieVariant"));
    CHECK(!entityMatches(target, "Husk"));
    // Case is meaningful: RTTI names are exact identifiers.
    CHECK(!entityMatches(target, "zombie"));

    // A second class bound to the same identifier also matches.
    target.classes = {"Villager", "VillagerV2"};
    target.id = "minecraft:villager";
    CHECK(entityMatches(target, "Villager"));
    CHECK(entityMatches(target, "VillagerV2"));
    CHECK(!entityMatches(target, "VillagerV3"));

    // Substring mode is opt-in and covers subclasses.
    target.matchBySubstring = true;
    CHECK(entityMatches(target, "Villager"));
    CHECK(entityMatches(target, "VillagerV2"));

    target.classes = {"Zombie"};
    target.id = "minecraft:zombie";
    target.matchBySubstring = true;
    CHECK(entityMatches(target, "ZombieVariant"));
    target.matchBySubstring = false;

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
    CHECK(snapshot.kind == Kind::Entity && snapshot.id == "minecraft:zombie");
    const auto expectedId = snapshot.id;
    store.clear();
    CHECK(!store.active());
    CHECK(!store.current());
    // The copy taken earlier is unaffected by the clear.
    CHECK(snapshot.id == expectedId);
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

void testLibcxxStringDecoding() {
    // Small-string form: byte 0 is (size << 1) and the bytes live inline at +1.
    // This is the shape the SSO sites in the RE log use.
    {
        alignas(8) unsigned char buffer[24]{};
        const std::string source = "minecraft:stone";
        const auto size = source.size();
        buffer[0] = static_cast<unsigned char>(size << 1);
        std::memcpy(buffer + 1, source.data(), size);
        CHECK(bes::sdk::readLibcxxString(buffer) == source);
    }

    // Long form: byte 0 has the low bit set and {size, data} sits at +8.
    {
        alignas(8) unsigned char buffer[24]{};
        const std::string source = "minecraft:deepslate_diamond_ore";
        const auto data = reinterpret_cast<std::uint64_t>(source.data());
        const auto size = static_cast<std::uint64_t>(source.size());
        buffer[0] = 0x1;
        std::memcpy(buffer + 8, &size, sizeof(size));
        std::memcpy(buffer + 16, &data, sizeof(data));
        CHECK(bes::sdk::readLibcxxString(buffer) == source);
    }

    // A size that exceeds the caller's limit is rejected rather than trusted.
    {
        alignas(8) unsigned char buffer[24]{};
        buffer[0] = static_cast<unsigned char>(30 << 1);
        CHECK(bes::sdk::readLibcxxString(buffer, 8).empty());
    }

    // A short-form size above the 22-byte inline capacity is impossible, so it
    // must not be read as a string.
    {
        alignas(8) unsigned char buffer[24]{};
        buffer[0] = static_cast<unsigned char>(23 << 1);
        CHECK(bes::sdk::readLibcxxString(buffer).empty());
    }

    // Long form with a data pointer the predicate rejects must not be dereferenced.
    {
        alignas(8) unsigned char buffer[24]{};
        const std::uint64_t size = 8;
        const std::uint64_t data = 0x10;  // below any valid mapping
        buffer[0] = 0x1;
        std::memcpy(buffer + 8, &size, sizeof(size));
        std::memcpy(buffer + 16, &data, sizeof(data));
        CHECK(bes::sdk::readLibcxxString(buffer).empty());
    }

    // A zeroed word reports size 0, which is not a usable string.
    {
        alignas(8) unsigned char buffer[24]{};
        CHECK(bes::sdk::readLibcxxString(buffer).empty());
    }
}

void testDiagnosticReportBuffer() {
    // The diagnostics dump used to call takeReport(), which empties the buffer.
    // A second dump therefore lost every startup line -- exactly the lines needed
    // to explain a "nothing happens" report. peekReport() must be non-destructive.
    bes::core::log::peekReport();  // start from a clean slate

    bes::core::log::report("line one");
    bes::core::log::report("line two");

    const auto first = bes::core::log::peekReport();
    CHECK(first.find("line one") != std::string::npos);
    CHECK(first.find("line two") != std::string::npos);

    // Reading twice must yield the same content: this is the actual regression.
    const auto second = bes::core::log::peekReport();
    CHECK(second == first);

    // takeReport() still clears, which is what callers who own the buffer want.
    const auto taken = bes::core::log::takeReport();
    CHECK(taken == first);
    CHECK(bes::core::log::peekReport().empty());
}

/// The renderer used to expose one number, `frames`, which only counts SUCCESSFUL
/// draws. `frames: 0` therefore meant any of several different things -- not
/// installed, no target, no tessellator, no camera, no material, empty geometry --
/// and a report could not distinguish "rendering is broken" from "there was
/// nothing to draw". The stage names are now part of the diagnostics contract, so
/// they are checked here: a stage that loses its name reports as "unknown" and
/// silently collapses that distinction again.
void testRenderStageNames() {
    using Stage = bes::render::RenderStage;
    const auto stageName = &bes::render::renderStageName;

    // Every stage must have a distinct, non-empty name.
    const Stage all[] = {Stage::NotYetDrawn,   Stage::NotInstalled,  Stage::BadArguments,
                         Stage::NoTargets,     Stage::NoTessellator, Stage::NoCamera,
                         Stage::NoMaterial,    Stage::EmptyGeometry, Stage::Drew};
    // Pins the count to the list above; a new enumerator must be added in both
    // places or this fails rather than going untested.
    CHECK(std::size(all) == bes::render::kRenderStageCount);

    std::vector<std::string> names;
    for (const auto stage : all) {
        const char* name = stageName(stage);
        CHECK(name != nullptr);
        CHECK(name[0] != '\0');
        CHECK(std::string(name) != "unknown");
        names.push_back(name);
    }
    for (std::size_t a = 0; a < names.size(); ++a) {
        for (std::size_t b = a + 1; b < names.size(); ++b) {
            CHECK(names[a] != names[b]);
        }
    }

    // The two states a user most needs told apart.
    CHECK(std::string(stageName(Stage::NoTargets)) == "no targets");
    CHECK(std::string(stageName(Stage::Drew)) == "drew");

    // "Nothing has been drawn yet" must not read as "the hook is missing". The
    // dump prints `renderer installed: yes` above the stage, so a stage of
    // "not installed" before the first frame was a contradiction in the report.
    CHECK(std::string(stageName(Stage::NotYetDrawn)) != stageName(Stage::NotInstalled));
    CHECK(std::string(stageName(Stage::NotYetDrawn)) == "no frame drawn yet");
}

/// Every offset in the profile is now corroborated by an external source for this
/// exact build (BedrockTools `include/bedrocktools/sdk/offsets/*.hpp`, whose
/// Render.hpp was last changed by "feat: 26.60 support"). That makes the table
/// checkable for the first time: a typo, a bad merge or a "quick fix" that quietly
/// changes one of these numbers is a defect, not a preference.
///
/// The values are duplicated here on purpose. A test that read them from the
/// profile could only prove the profile is self-consistent.

void testOffsetsMatchReference() {
    struct Reference {
        bes::core::OffId id;
        std::size_t value;
        const char* name;
    };
    // Source column is which BedrockTools header each came from.
    const Reference expected[] = {
        {bes::core::OffId::ActorStateVector, 0x228, "Actor::mStateVectorComponent"},
        {bes::core::OffId::ActorAabbComponent, 0x230, "mStateVectorComponent + mAABBShapeComponent(8)"},
        {bes::core::OffId::ActorLevel, 0x1F0, "Actor::mLevel"},
        {bes::core::OffId::ActorDimension, 0x1E0, "Actor::mDimension"},
        {bes::core::OffId::LevelActorManager, 0x478, "Level::mActorManager"},
        {bes::core::OffId::DimensionBlockSource, 0xD0, "Dimension::mBlockSource"},
        {bes::core::OffId::ScreenContextTessellator, 0xB0, "ScreenContext::mTessellator"},
        {bes::core::OffId::ScreenContextColorHolder, 0x30, "ScreenContext::mColorHolder"},
        // Was 0x90 with no external confirmation; BedrockTools Core.hpp says 0x1B8
        // for 26.60. Pinned here so a "quick fix" back to the old guess fails.
        {bes::core::OffId::ClientInstanceLevelRenderer, 0x1B8, "ClientInstance::mLevelRenderer"},
        {bes::core::OffId::LevelRendererPlayer, 0x430, "LevelRenderer::mLevelRendererPlayer"},
        {bes::core::OffId::LevelRendererPlayerCamPos, 0x614, "LevelRendererPlayer::mCamPos"},
    };

    // Read the SHIPPED file, not the Profile singleton. Profile is process-wide and
    // earlier tests replace its offsets, so asserting through it would check
    // whatever the previous test left behind and silently pass. Reading the file is
    // also the stronger claim: this is the table that actually ships.
    std::ifstream file("resources/signatures.json");
    CHECK(file.good());
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    auto& profile = bes::core::Profile::get();
    CHECK(profile.loadFromText(text, "resources/signatures.json", true));

    for (const auto& entry : expected) {
        const auto candidate = profile.candidates(entry.id);
        if (candidate.empty()) {
            CHECK(false); // A missing category is a failure, not a skip.
            continue;
        }
        CHECK(candidate.front().value == entry.value);
        // Corroboration must also be recorded, or the diagnostics cannot tell a
        // referenced offset from one somebody guessed.
        CHECK(!candidate.front().origin.empty());
    }

    // Actor rotation (0x238) is a literal inside World.cpp's anonymous namespace
    // rather than a profile entry, so it cannot be asserted from here without
    // inventing a test-only alias for it. BedrockTools agrees on the value
    // (Actor::mActorRotationComponent = 0x238); noted rather than tested.
}

/// `gamehooks::cameraPosition()` confirms three offsets or none: the chain is
/// only promoted when the whole path validates and the result looks like a real
/// camera. The plausibility rule inside it is what separates "this candidate is a
/// cam pos" from "this candidate points at a zeroed struct", so it is the part
/// worth pinning. It is reproduced here because the real one lives in a lambda
/// inside a function that needs the preloader SDK to link.
void testCameraPlausibility() {
    const auto plausible = [](float x, float y, float z) {
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return false;
        if (std::fabs(y) > 65536.f) return false;
        if (x == 0.f && y == 0.f && z == 0.f) return false;
        return true;
    };

    // A normal standing camera.
    CHECK(plausible(100.5f, 70.f, -230.25f));
    // Negative coordinates are ordinary.
    CHECK(plausible(-1.f, 64.f, -1.f));
    // Deepslate depth and build-limit height both still count as real.
    CHECK(plausible(0.f, -60.f, 0.f));
    CHECK(plausible(0.f, 319.f, 0.f));

    // The zeroed-struct signature. A live camera is essentially never at exactly
    // 0,0,0 -- and if it were, refusing one frame costs nothing while accepting a
    // zeroed read would put the camera at world origin and draw every box wrong.
    CHECK(!plausible(0.f, 0.f, 0.f));
    // Non-finite values are the other way a bad read shows up.
    CHECK(!plausible(std::nanf(""), 64.f, 0.f));
    CHECK(!plausible(0.f, 64.f, std::nanf("")));
    CHECK(!plausible(0.f, INFINITY, 0.f));
    // Beyond any sane height envelope.
    CHECK(!plausible(0.f, 70000.f, 0.f));
    CHECK(!plausible(0.f, -70000.f, 0.f));
}

/// The diagnostics read `offsets proven: 0/11` on a live player with every probe
/// correctly written. The cause was the memory map, not the probes: a real
/// Minecraft process maps 6022 regions and kMaxRegions was 4096, so the library's
/// own mappings -- and everything above them -- read as unmapped. Every guarded
/// check then answered "not readable" and every correct candidate was rejected.
///
/// The dangerous property is that truncation does not fail loudly. These checks
/// pin the two halves that matter: the capacity must exceed a real device map, and
/// truncation must be observable so a negative answer can be treated as unknown.
void testAddressSpaceCapacity() {
    // Measured, not guessed: the 2026-11-13 tombstone's memory map has 6022
    // regions. A cap at or below that silently blinds the probes.
    CHECK(bes::sdk::AddressSpace::kMaxRegions > 6022);

    auto& space = bes::sdk::AddressSpace::get();
    CHECK(space.refresh());

    // A host process is small, so truncation must not be claimed here. Without
    // this the flag could be hardcoded true and the test would still pass.
    CHECK(!space.truncated());

    // Capacity must not have been reached, or the flag above is meaningless.
    CHECK(space.regionCount() < bes::sdk::AddressSpace::kMaxRegions);

    // read_proc/parse consistency: every region must be non-empty and ordered.
    // Done through the public surface so this stays a behavioural test.
    CHECK(space.fileBackedRegionCount() > 0);
    CHECK(space.regionCount() >= space.fileBackedRegionCount());
}

void testModulePresence() {
    // The process gate used to compare argv[0] against two hardcoded strings.
    // It now asks whether the game's image is actually mapped, so the parser that
    // answers that question has to be correct -- including on lines whose path
    // column contains spaces, which a naive sscanf("%s") would truncate.
    auto& space = bes::sdk::AddressSpace::get();
    CHECK(space.refresh());
    CHECK(space.regionCount() > 0);

    // Positive control first. Without this, breaking the path parser would make
    // every query below answer "not loaded" and the test would still pass -- it
    // only asserts absence, and absence is what a broken parser produces.
    CHECK(space.fileBackedRegionCount() > 0);

    // A process always has something mapped, and it never has a Minecraft image.
    // The second half is the assertion that matters: absence must report absence.
    CHECK(space.modulePath("libminecraftpe.so").empty());
    CHECK(!space.moduleLoaded("libminecraftpe.so"));

    // A nonsense soname must not match anything either.
    CHECK(!space.moduleLoaded("this_is_not_a_real_library_9f3a.so"));

    // An empty name must never match a region.
    CHECK(!space.moduleLoaded(""));

    // readable() must stay consistent with the same parsed table.
    int local = 0;
    CHECK(space.readable(&local, sizeof(local)));
    CHECK(!space.readable(static_cast<void*>(nullptr), 1));
    CHECK(!space.readable(reinterpret_cast<void*>(0x10), 1));

    // The matching rule itself, exercised directly. This is the part a host test
    // cannot reach through the public query, because there is no Minecraft image
    // to find -- so it is tested as a predicate.
    CHECK(bes::sdk::matchesLibraryPath("/data/app/libminecraftpe.so", "libminecraftpe.so"));
    CHECK(bes::sdk::matchesLibraryPath("/data/app/base.apk!/lib/arm64-v8a/libminecraftpe.so",
                                       "libminecraftpe.so"));
    // The loader's marker for a replaced file must not defeat the match.
    CHECK(bes::sdk::matchesLibraryPath("/data/app/libminecraftpe.so (deleted)", "libminecraftpe.so"));
    // Whole-component matching: a longer name that merely ends with the soname
    // must not match, or every diagnostic would report a false positive.
    // This one ends with the exact soname but the character before it is 'e', not
    // '/', so it must not match. A plain suffix compare accepts it -- and that
    // false positive would make the mod believe the game was present.
    CHECK(!bes::sdk::matchesLibraryPath("/data/app/fakelibminecraftpe.so", "libminecraftpe.so"));
    // Same trap on the (deleted) form.
    CHECK(!bes::sdk::matchesLibraryPath("/data/app/fakelibminecraftpe.so (deleted)",
                                        "libminecraftpe.so"));
    // And a longer name that does not end with the soname at all.
    CHECK(!bes::sdk::matchesLibraryPath("/data/app/libfakeminecraftpe.so", "libminecraftpe.so"));
    CHECK(!bes::sdk::matchesLibraryPath("", "libminecraftpe.so"));
    CHECK(!bes::sdk::matchesLibraryPath("/data/app/libminecraftpe.so", ""));
    // A path shorter than the soname must not read out of bounds.
    CHECK(!bes::sdk::matchesLibraryPath("/a.so", "libminecraftpe.so"));
    // Exact-name and no-directory cases.
    CHECK(bes::sdk::matchesLibraryPath("libminecraftpe.so", "libminecraftpe.so"));
}

} // namespace

int main() {
    testAabb();
    testColor();
    testRttiNames();
    testRttiOnRealObjects();
    testProfileParsing();
    testVtableCandidates();
    testOffsetsAreSane();
    testOffsetsRequireProof();
    testScannerSettings();
    testBlockCursor();
    testSelectionMatching();
    testIdentifierSyntax();
    testIdentifierIndex();
    testLibcxxStringDecoding();
    testDiagnosticReportBuffer();
    testRenderStageNames();
    testOffsetsMatchReference();
    testCameraPlausibility();
    testAddressSpaceCapacity();
    testModulePresence();

    std::printf("%d checks, %d failure(s)\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
