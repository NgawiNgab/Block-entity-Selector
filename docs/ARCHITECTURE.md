# Architecture

A short tour of how a selection turns into pixels, and which piece owns which concern.

```
        ┌──────────────┐   tap SEL    ┌──────────────────┐
        │ Picker (UI)  │─────────────▶│ Selection (state)│
        └──────────────┘              └──────────────────┘
                 │ reads                        │ read by
                 ▼                              ▼
        ┌──────────────┐              ┌──────────────────┐
        │ World        │──actors─────▶│ Scanner          │
        │ (offsets)    │◀──blocks─────│ (per-tick work)  │
        └──────────────┘              └──────────────────┘
                 ▲                              │ FrameTargets
                 │ confirmOffset                ▼
        ┌──────────────┐              ┌──────────────────┐
        │ Resolver     │              │ HighlightRenderer│──▶ Tessellator
        │ (signatures) │              └──────────────────┘
        └──────────────┘
                 ▲
        ┌──────────────┐
        │ Profile      │  resources/signatures.json
        └──────────────┘
```

## Threads

| Thread | Runs | Notes |
|---|---|---|
| Game render thread | `renderLevel` detour, `HighlightRenderer::draw` | Reads `Scanner::targets()` and `World::snapshot()` (both copies, both under a mutex); never calls into game code that can block. |
| Game frame thread (`Rendering Pool`) | `eglSwapBuffers` detour → `Runtime::onFrame` → `World::update` + `Scanner::update` | **The single tick driver.** All game-pointer dereferencing happens here, on one thread. |
| Game tick thread | `ClientInstance::update` detour → records the local player only | Deliberately does **not** tick the module: two drivers meant one thread could rewrite the snapshot while the other read it. See the 2026-11-13 tombstone. |
| Launcher UI thread | Module menu callbacks (`onConfigChanged`, `onToggle`) | Only touches value types and calls `Scanner::setSettings`. |
| Config worker | `ConfigStore` | Debounced JSON writes, atomic rename. |

Rule of thumb: **pointers never cross threads as ownership**; `Selection` and `FrameTargets` carry
value types (`BlockPos`, `AABB`, strings) so the render path cannot follow a freed actor.

`World` follows the same rule: `snapshot()` returns a **copy** taken under `World::mMutex`, and
`Snapshot` is `static_assert`ed trivially copyable so a future member cannot quietly reintroduce
the aliasing. It used to hand out a reference to the live member, which two threads then raced on.

## Offset proof

`Resolver::offset()` **fails closed**. Every value in `resources/signatures.json` is a
*candidate* — a hypothesis with no `origin` recorded — and it is not usable until a probe has
confirmed it against a live object:

```
profile candidate  --confirmOffset()-->  proven value  --offset()-->  readable by callers
```

`offset()` returns `0` for anything unproven, and `offsetCandidate()` exists only for the
diagnostics dump. This is not defensive style; it is the fix for the 2026-11-13 SIGSEGV, where an
unproven `actor_level` was dereferenced on a live player and the resulting garbage pointer reached
game code.

**Callers must treat `0` as "unproven", never as "field at offset zero".** A pointer read at
offset 0 returns the object's own vptr, which is a real mapped address and will pass any
plausibility heuristic — the trap that made this bug hard to see. There is a test
(`testOffsetsRequireProof`) pinning the fail-closed behaviour in both directions.

## Identifier layer

Selection is by **identifier** (`minecraft:diamond_ore`, `minecraft:zombie`),
typed or pasted by the player. `src/selection/Identifier.{hpp,cpp}` is the only
place that interprets those strings, and it touches no game pointer, offset or
pattern, so it is fully host-testable.

```
player input ──▶ normalise() ──▶ resolve()
                                   │
                     ┌─────────────┴─────────────┐
                     ▼                           ▼
              Confidence level            ClassIndex (1:Many)
         Invalid / Unverified /           minecraft:villager
      ObservedLive / Documented /          ├─ Villager
    RegistryConfirmed / RegistryResolved   └─ VillagerV2
                     │                           │
                     ▼                           ▼
              shown in the menu        consumed by Scanner/entityMatches
```

Two rules make this honest rather than decorative:

**A typed identifier is a claim, not a fact.** Every `Target` carries a
`Confidence`. The menu label prints it, so `minecraft:custom_mob` shows as
`[unverified]` until the mod actually sees one in the world, and
`minecraft:zombie` shows `[documented]` — never `[confirmed]`, which is reserved
for an answer from the game's own registry lookup. The registry-backed levels are
defined and unimplemented; see `docs/UPDATING.md`.

**Entity identifiers are 1:Many.** One id can be backed by several RTTI classes
(`minecraft:villager` → `Villager` and `VillagerV2`). `ClassIndex` holds those
bindings, additive and deduplicated, keeping the strongest evidence per pair. The
scanner only asks the index; adding a new evidence source is a `bind()` call, not
a scanner edit. That separation is what lets a runtime registry lookup be bolted
on later without touching matching logic.

Evidence sources, weakest to strongest: hand-typed (nothing), observed live (the
mod saw the block id or actor class), bundled documented table, game registry.

## Key decisions

**RTTI over offsets for entity identity.** Every polymorphic Bedrock actor starts with a vptr, and
the slot *immediately before* that vtable holds the `type_info` pointer (`vtable[-1]`; `vtable[-2]`
is offset-to-top and is zero for a primary vtable). Reading `type_info + 8` gives the mangled class
name — no offset table, no signature, no ambiguity, and it works identically on every build. The mod
therefore classifies entities by *name* ("Zombie", "Player", "ItemActor") and never needs
`Actor::getTypeNameTag`.

Three properties this reader must hold, each of which has failed at least once:

- **The slot is `-1`.** Reading `-2` returns null for every object, which surfaces as `className()`
  returning "" everywhere. Nothing errors; the whole offset chain merely never confirms. Because
  every probe in `World.cpp` gates on a class name, one wrong index blocked eleven offsets.
- **Objects must be 8-byte aligned.** A vptr sits at offset 0 of a polymorphic object, so such an
  object is always aligned. Without that check, a smaller or int-only object is read as eight bytes
  of "vptr" that can look aligned, and `vtable[-1]` is then read out of bounds.
- **Typeinfo names carry no `_Z` prefix.** libc++ stores the bare mangling, sometimes with a leading
  `*` for internal-linkage types, so the parser strips those itself rather than going through
  `__cxa_demangle`.

**Typeinfo names are parsed, not demangled.** `__cxa_demangle` rejects bare typeinfo names (they are
not unambiguous mangled symbols, so nested and template forms return `-2`). `src/core/Rtti.cpp`
therefore walks the Itanium mangling directly: length-prefixed identifiers, nesting, template
argument skipping. The demangler is still used as a fallback and for the diagnostics dump.

**Candidate lists, not single values.** Every signature and offset is a list. Resolution picks the
first candidate that resolves; offsets are additionally validated against a live object before they
are promoted. This is what makes a Zaphkiel-generated profile droppable at runtime.

**Probes run before reads.** `World::update` confirms the state-vector and hitbox offsets against
the live local player *before* anything reads through them, so a rejected candidate can never leak a
garbage position or box into the snapshot. Confirmation is one-shot per session because the layout
does not change while the game is running.

**Degrade, do not crash.** If `actor_manager_list` or `block_source_get_block` never resolves, the
corresponding half of the scanner logs once and reports "disabled" in the diagnostics dump instead of
walking memory. Choosing silence over a guess is the difference between a mod that does nothing and
one that crashes a player's world.

**Geometry is rebuilt per frame.** The box list is small (bounded by the scan radius) and rebuilding
avoids cache invalidation bugs, so `draw()` allocates vectors, fills them, and submits two batched
draws (lines, then triangles when the translucent fill is enabled).

**Two draw passes, one style.** `Style::seeThrough` toggles `GL_DEPTH_TEST` around the highlight
pass only, restoring the previous state afterwards (`GlState.cpp`). Depth-tested mode still works
for anyone who prefers the vanilla look.

**Menu registration is not gated on the game process.** `Runtime::enable` registers the Mod Menu
module and the on-screen button *before* it checks whether the current process is the Minecraft
child. Both are launcher services -- `pl::modmenu::ModuleBuilder` and `ButtonBuilder` -- and touch no
game state, so they cannot fail for want of `libminecraftpe.so`. Registration used to sit after the
`runningInsideMinecraft()` check, which meant a process whose `argv[0]` matched neither
`com.mojang.minecraftpe` nor `org.levimc.launcher:minecraft` registered *nothing* and said nothing.
Everything that genuinely needs the game -- signature resolution, the update hook, the frame tick,
the renderer -- stays behind the check, and the check now logs a warning instead of returning
silently.

## Files worth knowing

| File | Why |
|---|---|
| `src/core/Profile.cpp` | The JSON schema: candidate lists, override merging. Change here first if you add a signature. |
| `src/core/Resolver.cpp` | Signature resolution order, pattern → vtable fallback, offset confirmation. |
| `src/core/Rtti.cpp` | Actor identity. The single most important piece for "works without signatures". |
| `src/selection/Identifier.cpp` | The identifier layer: normalisation, confidence, 1:Many class index. Game-free and unit tested. |
| `src/selection/Scanner.cpp` | Per-tick work: entity scan throttling, block budget, dimension-change invalidation. |
| `src/selection/BlockCursor.hpp` | The resumable block walk, header-only and game-free so it can be unit tested. |
| `src/selection/ScannerState.cpp` | The parts of `Scanner` that touch no game state (`get`, settings, reset, targets); split out so the host tests can link them without `World`. |
| `src/render/HighlightRenderer.cpp` | Hook, material acquisition, batched submission, colour state. |
| `src/world/World.cpp` | All game-pointer dereferencing, offset probing, block name resolution. |

## Extending

Adding a new signature:

1. Add the id to `SigId` in `Profile.hpp` **and** the matching name to `kSigNames` in `Profile.cpp`
   (a `static_assert` enforces the count).
2. Add candidates to `resources/signatures.json`.
3. Resolve it with `Resolver::get().address(SigId::YourId)`.

Adding a new offset: same pattern with `OffId` / `kOffNames`, plus a probe in `World::probeOffsets`
so a stale value is rejected rather than trusted.
