// Host-side tests for the parts of the mod that do not touch the game.
//
// Build:  xmake f -m debug && xmake build tests && xmake run tests
// Linux:  tools/run_tests.sh

#include "core/Profile.hpp"
#include "core/Rtti.hpp"

#include <bes/sdk/Types.hpp>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int gFailures = 0;
int gChecks = 0;

void check(bool condition, const char* what, int line) {
    ++gChecks;
    if (condition) return;
    ++gFailures;
    std::printf("FAIL %s:%d  %s\n", __FILE__, line, what);
}

// Variadic so comma-containing expressions (braced initialisers) still work.
#define CHECK(...) check((__VA_ARGS__), #__VA_ARGS__, __LINE__)

bool near(float left, float right, float epsilon = 1e-4f) {
    return std::fabs(left - right) <= epsilon;
}

// ---- sdk::AABB -------------------------------------------------------------

void testAabb() {
    const bes::sdk::AABB box{{0.f, 0.f, 0.f}, {1.f, 2.f, 3.f}};
    CHECK(box.valid());
    CHECK(near(box.extent().x, 1.f));
    CHECK(near(box.extent().y, 2.f));
    CHECK(near(box.extent().z, 3.f));
    CHECK(near(box.center().y, 1.f));

    const bes::sdk::AABB flat{{0.f, 0.f, 0.f}, {0.f, 1.f, 1.f}};
    CHECK(!flat.valid());

    // Distance from a point: outside on one axis, inside on the others.
    CHECK(near(box.distanceSquaredTo({3.f, 1.f, 1.f}), 4.f));
    CHECK(near(box.distanceSquaredTo({0.5f, 1.f, 1.f}), 0.f));

    // Ray straight through the middle hits, a parallel miss does not.
    using bes::sdk::Vec3;
    CHECK(box.intersectsRay({0.5f, 1.f, -5.f}, {0.f, 0.f, 1.f}, 10.f));
    CHECK(!box.intersectsRay({5.5f, 1.f, -5.f}, {0.f, 0.f, 1.f}, 10.f));
    // Ray that would hit but is too short.
    CHECK(!box.intersectsRay({0.5f, 1.f, -5.f}, {0.f, 0.f, 1.f}, 2.f));
    // Start inside the box.
    CHECK(box.intersectsRay({0.5f, 1.f, 0.5f}, {0.f, 0.f, 1.f}, 5.f));

    const auto expanded = box.expanded(0.5f);
    CHECK(near(expanded.min.x, -0.5f));
    CHECK(near(expanded.max.y, 2.5f));
}

// ---- colours ---------------------------------------------------------------

void testColor() {
    const auto color = bes::sdk::Color::fromArgb(0xFF2ED573);
    CHECK(near(color.r, 0x2E / 255.f));
    CHECK(near(color.g, 0xD5 / 255.f));
    CHECK(near(color.b, 0x73 / 255.f));
    CHECK(near(color.a, 1.f));

    const auto half = color.withAlpha(0.5f);
    CHECK(near(half.a, 0.5f));
    CHECK(near(half.r, color.r));

    const auto transparent = bes::sdk::Color::fromArgb(0x00112233);
    CHECK(near(transparent.a, 0.f));
}

// ---- rtti helpers ----------------------------------------------------------

void testRttiNames() {
    using namespace bes::core::rtti;

    // Plain class typeinfo (the common case for every Bedrock actor class).
    CHECK(classNameFromMangled("6Zombie") == "Zombie");
    CHECK(classNameFromMangled("11ZombieActor") == "ZombieActor");
    CHECK(classNameFromMangled("6Player") == "Player");
    // The _ZTS spelling seen in symbol tables is accepted too.
    CHECK(classNameFromMangled("_ZTS6Zombie") == "Zombie");
    // Namespaced typeinfo: innermost class wins.
    CHECK(classNameFromMangled("N6bedrock5Actor6ZombieE") == "Zombie");
    // Templated typeinfo: the template argument list is dropped.
    CHECK(classNameFromMangled("N4entt13basic_storageI16ActorEquipment8EntityIdEE") == "basic_storage");
    // Garbage in, empty out.
    CHECK(classNameFromMangled("").empty());
    CHECK(classNameFromMangled("!!not_a_name").empty());

    // Below the top-level Actor families, classification falls back to "other",
    // which is why the picker uses the real class name rather than a tag.
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

// ---- profile ---------------------------------------------------------------

void testProfileParsing() {
    auto& profile = bes::core::Profile::get();

    const char* document = R"({
        "game_version": "1.26.6x-test",
        "signatures": {
            "render_level": { "candidates": [
                { "pattern": "AA BB ?? DD", "note": "first" },
                "EE FF 00"
            ]},
            "actor_is_player": "11 22 33",
            "unknown_key_is_ignored": "AA"
        },
        "offsets": {
            "level_renderer_player": { "candidates": [ { "offset": "0x430" }, { "offset": 1072 } ] },
            "actor_level": "0x1F0"
        }
    })";

    CHECK(profile.loadFromText(document, "unit-test", true));
    CHECK(profile.loaded());
    CHECK(profile.gameVersion() == "1.26.6x-test");

    const auto& renderLevel = profile.candidates(bes::core::SigId::RenderLevel);
    CHECK(renderLevel.size() == 2);
    CHECK(renderLevel[0].pattern == "AA BB ?? DD");
    CHECK(renderLevel[0].kind == bes::core::SigCandidate::Kind::Pattern);

    const auto& isPlayer = profile.candidates(bes::core::SigId::ActorIsPlayer);
    CHECK(isPlayer.size() == 1);
    CHECK(isPlayer[0].pattern == "11 22 33");

    // Keys not mentioned in the document stay empty.
    CHECK(profile.candidates(bes::core::SigId::RenderMeshImmediately).empty());

    const auto& playerRenderer = profile.candidates(bes::core::OffId::LevelRendererPlayer);
    CHECK(playerRenderer.size() == 2);
    CHECK(playerRenderer[0].value == 0x430);
    CHECK(playerRenderer[1].value == 0x430);

    const auto& actorLevel = profile.candidates(bes::core::OffId::ActorLevel);
    CHECK(actorLevel.size() == 1);
    CHECK(actorLevel[0].value == 0x1F0);

    // An override document only replaces the keys it defines.
    const char* overrideDocument = R"({
        "signatures": { "render_level": "99 99" }
    })";
    CHECK(profile.loadFromText(overrideDocument, "override", false));
    CHECK(profile.candidates(bes::core::SigId::RenderLevel).size() == 1);
    CHECK(profile.candidates(bes::core::SigId::RenderLevel)[0].pattern == "99 99");
    // Untouched keys survive the merge.
    CHECK(profile.candidates(bes::core::SigId::ActorIsPlayer).size() == 1);
    CHECK(profile.candidates(bes::core::OffId::ActorLevel).size() == 1);

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
    })", "vtable-test", false));

    const auto& candidates = profile.candidates(bes::core::SigId::ActorGetNameTag);
    CHECK(candidates.size() == 1);
    CHECK(candidates[0].kind == bes::core::SigCandidate::Kind::Vtable);
    CHECK(candidates[0].pattern == "10ActorOwner");
    CHECK(candidates[0].slot == 3);
}

} // namespace

int main() {
    testAabb();
    testColor();
    testRttiNames();
    testProfileParsing();
    testVtableCandidates();

    std::printf("%d checks, %d failure(s)\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
