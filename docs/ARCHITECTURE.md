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
| Game render thread | `renderLevel` detour, `HighlightRenderer::draw` | Reads `Scanner::targets()` under a short mutex; never calls into game code that can block. |
| Game tick thread | `ClientInstance::update` detour → `World::update` + `Scanner::update` | All game-pointer dereferencing happens here. |
| Game frame thread | `eglSwapBuffers` detour → `Runtime::onFrame` | Keeps the scan advancing while paused. |
| Launcher UI thread | Module menu callbacks (`onConfigChanged`, `onToggle`) | Only touches value types and calls `Scanner::setSettings`. |
| Config worker | `ConfigStore` | Debounced JSON writes, atomic rename. |

Rule of thumb: **pointers never cross threads as ownership**; `Selection` and `FrameTargets` carry
value types (`BlockPos`, `AABB`, strings) so the render path cannot follow a freed actor.

## Key decisions

**RTTI over offsets for entity identity.** Every polymorphic Bedrock actor has a `type_info` pointer
two slots before its vtable. Reading `type_info + 8` gives the mangled class name — no offset table,
no signature, no ambiguity, and it works identically on every build. The mod therefore classifies
entities by *name* ("Zombie", "Player", "ItemActor") and never needs `Actor::getTypeNameTag`.

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

## Files worth knowing

| File | Why |
|---|---|
| `src/core/Profile.cpp` | The JSON schema: candidate lists, override merging. Change here first if you add a signature. |
| `src/core/Resolver.cpp` | Signature resolution order, pattern → vtable fallback, offset confirmation. |
| `src/core/Rtti.cpp` | Actor identity. The single most important piece for "works without signatures". |
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
