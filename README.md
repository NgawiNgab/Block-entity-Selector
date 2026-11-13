# Block Entity Selector

A native [LeviLauncher](https://github.com/LiteLDev/LeviLaunchroid) mod for Minecraft Bedrock that
highlights **every instance of a selected block or entity type in the world**, drawn only on your
own screen.

Target game version: **1.26.6x** · Language: C++20 · Built on the [preloader](https://github.com/LiteLDev/preloader-android) SDK.

---

## What it does

Pick a block (say `minecraft:diamond_ore`) or an entity (`Zombie`, `Player`, `ItemActor`) and the mod
draws a coloured outline box around **every loaded instance** of it — through terrain, in real time,
only on your client. Nothing is sent to the server, nothing is modified in the world.

- Type or paste a **Block Identifier** (`minecraft:diamond_ore`) or **Entity
  Identifier** (`minecraft:zombie`) in the module menu. Bare names work too —
  `diamond_ore` becomes `minecraft:diamond_ore`.
- Tap **SEL** (the on-screen button) to cycle the selection:
  entity under the crosshair → block under your feet → cleared.
- The keybind clears an active selection; with nothing selected it toggles highlighting on or off.
- Block scans walk outward from the player in a resumable cursor, a fixed number of lookups per
  tick, so a long radius does not spike frame time.
- Entity scans reuse the game's actor list, so they are always complete and always current.
- Blocks, mobs and players each get their own colour; players can be excluded entirely, and the
  translucent fill is off by default (outline only).
- One identifier can cover several internal classes (`minecraft:villager` →
  `Villager`, `VillagerV2`), and the menu shows which ones it expanded to.

### Identifier confidence

A typed identifier is a *claim*, so the mod labels what it actually knows:

| Label | Meaning |
|---|---|
| `documented` | Listed in the bundled table. Not proof it exists in your build. |
| `observed-live` | The mod saw it in your world: a block read back this id, or a mapped class was present. |
| `unverified` | Well-formed, but nothing has confirmed it. Still selectable — the mod will look. |
| `invalid` | Not a legal identifier; rejected, and the previous selection is kept. |

`registry-confirmed` is defined but not implemented yet — see
[docs/UPDATING.md](docs/UPDATING.md) §8. The mod will not claim an identifier is
real until the game itself says so.

## How it works

The mod is intentionally built around *runtime discovery* instead of hardcoded offsets, because no
offset/signature dump exists for this game build yet:

| Concern | Approach |
|---|---|
| Entity identity | **RTTI**: `vtable[-2]` → `type_info` → name. No offsets at all; works on any build. |
| Function addresses | Signature table in `resources/signatures.json`, resolved with `pl::memory::resolveSignatures`, with `resolveVtableFunction` fallback candidates. |
| Struct offsets | Candidate lists per field, **validated against live objects** before use (`Resolver::confirmOffset`). |
| World drawing | Hook `LevelRenderer::renderLevel` → `Tessellator::begin/color/vertex` → `MeshHelpers::renderMeshImmediately`. |
| Through-wall | `GL_DEPTH_TEST` disabled only for the highlight pass, via `libGLESv2.so` dlsym. |

Because every signature and offset is a *candidate list* read from JSON, updating the mod for a new
game build never requires recompiling: run Zaphkiel, convert its output, drop the new
`signatures.json` into the mod's `config/` folder.

## Install

1. Install LeviLauncher and Minecraft Bedrock 1.26.6x.
2. Download `BlockEntitySelector.levipack` from Releases.
3. Import it in LeviLauncher's mod manager and enable it.
4. In game, open the Mod Menu → **Block / Entity Selector** → enable.
5. Tap **SEL**, or use the keybind.

If the highlight does not appear, open **Dump Diagnostics** in the module menu. The report lists
which signatures resolved, which offsets were confirmed and by what evidence, the live scan
settings, how many matches were found, and which actor classes actually matched — which is what
tells "nothing near you" apart from "the offsets are wrong".

## Building

Requirements: Android NDK r28c, xmake, Python 3.

```sh
xmake f -y -p android -a arm64-v8a -m release --ndk=/path/to/android-ndk-r28c
xmake -y
```

Outputs `libBlockEntitySelector.so` and `BlockEntitySelector.levipack` in
`build/android/arm64-v8a/release/`.

The first build resolves the preloader SDK, `fmt` and `nlohmann/json`, then pins them into
`xmake-requires.lock` — commit that file to make later builds reproducible.

`--entry-name` on the packaging script keeps the manifest's `entry` field in sync with the library
name the launcher expects; it is wired up in `xmake.lua` already.

Host-side unit tests (no Android toolchain needed):

```sh
./tools/run_tests.sh          # needs g++ + nlohmann-json3-dev, python3
```

The suite covers RTTI typeinfo-name parsing, AABB/ray maths, colour handling, profile
loading/merging, the block-scan cursor, and the Zaphkiel converter end to end.

## Retargeting to a new game build

See **[docs/UPDATING.md](docs/UPDATING.md)** for the full procedure. The short version:

1. Copy `tools/zaphkiel/zInput.txt` to the launcher's `kurumi/` folder and run Zaphkiel.
2. Convert its output into a fresh profile:
   ```sh
   python3 tools/zaphkiel/apply_output.py \
       --signatures /sdcard/.../kurumi/signatures.json \
       --analysis   /sdcard/.../kurumi/zaphkiel_out.txt \
       --output     build/signatures.json
   ```
3. Copy the result to `<mod dir>/config/signatures.json` (or replace `resources/signatures.json`).
4. Reload the mod and check **Dump Diagnostics**.

## Project layout

```
include/bes/          public headers (Version, sdk types)
src/core/             runtime, signature resolver, profile, RTTI, hooks, config, logging
src/world/            world access (actors, blocks, snapshot)
src/selection/        selection state + world scanner
src/render/           highlight renderer + GL state
src/modules/          the user-facing module (config + menu registration)
src/launcher/         on-screen picker button
resources/            signatures.json — the retargeting surface
tools/zaphkiel/       Zaphkiel target list + output converter
tools/                run_tests.sh
tests/                host unit tests
docs/                 UPDATING.md, ARCHITECTURE.md
```

## Credits

Structure and several rendering techniques are modelled on
[BedrockTools](https://github.com/QYCottage/BedrockTools) by RadiantByte, and the
signature/offset workflow follows [Zaphkiel-RE](https://github.com/Viablecobra/Zaphkiel-RE).

## License

MIT. See [LICENSE](LICENSE).

The signature table in `resources/signatures.json` is data, not code: the pattern strings were taken
from BedrockTools, which publishes them under its own terms. Regenerate them with Zaphkiel for your
own build if you plan to redistribute.

Status: the Android arm64 build and the host test suite run in CI on every push, producing a real
`libBlockEntitySelector.so` and `BlockEntitySelector.levipack`. It has **not** been run against a
real 1.26.6x install yet, so the runtime side is unverified: the bundled signature patterns are
seeded from BedrockTools and are the first thing to regenerate with Zaphkiel. See
`docs/UPDATING.md`.

**Disclaimer:** not affiliated with Mojang or Microsoft. Use at your own risk and in line with
Minecraft's terms of service; client-side visual mods can still be against the rules of individual
servers.
