# ZAPHKIEL REQUEST — prove `Actor::mLevel`, `Actor::mDimension`, `Dimension::mBlockSource` for 1.26.6x

**Why now:** the 2026-11-13 tombstone shows a `SIGSEGV` at `fault addr 0x9` from
`World::update`, because those three offsets were used on a live player without ever having
been proven on 1.26.6x. The code now refuses to use an unproven offset, so the crash is gone —
and so is all functionality until these three are proven. **This request is the unblocker.**

---

## A. Required configuration

`/storage/emulated/0/Android/media/org.levimc.launcher/kurumi/zaphkiel_config.txt`

Change **only** these four keys. Everything else stays at its current value; the rest of the
list is preserved because prior runs already produced usable `sig`/`fullvf` output with it.

| Key | Set to | Why |
|---|---|---|
| `max_fn_insns` | `400` | Default is `120` and cuts a preview mid-function. Prologues here are long enough that a `bl`/`str` past the cut silently hides the field store we are looking for. |
| `startup_delay_secs` | `15` | The library must be *mapped and past its own init* before the scan. Prior runs used `3` and worked, but 15 costs nothing and removes a race on a cold start. |
| `dump_rtti` | `true` | Already the default; restated because the whole method depends on `findclass` resolving. |
| `separate_dump_files` | `true` | Already the default. Keeps the analysis file small enough to send back. |

**Do not change** `full_decompile` (leave `false`) and `get_strings` (leave `false`). Neither is
needed here and both make the output large enough to be unwieldy.

If your config uses different key names than the above, **stop and send me the file** rather
than guessing — I will re-issue the table against the real keys.

---

## B. Complete zInput

Copy this verbatim into
`/storage/emulated/0/Android/media/org.levimc.launcher/kurumi/zInput.txt`.
It is self-contained: nothing from a previous request is required, and it contains **no
absolute addresses** (all four entries are class-based, so they survive the `-0x6e04000`
base drift between runs).

```
/* ------------------------------------------------------------------
   Request: Actor / Dimension layout for 1.26.6x
   Purpose: prove the three field offsets the world layer needs.
   All entries are class-based; no VAs, so base drift is irrelevant.
   ------------------------------------------------------------------ */

/* 1. Does the class we expect actually exist in this build, and what
      exactly is it called? "Actor" also prefixes ActorRenderDispatcher,
      ActorFactory, ActorManager... so the full name matters.           */
findclass<Actor>

/* 2. Actor's own vtable. This is the independent path to the same
      object the struct scan uses, and it tells us the base class
      is polymorphic (the struct scan requires a vtable).               */
fullvf<Actor>

/* 3. Actor layout. Actor is large; 0x600 covers well past every
      candidate offset in our profile (max is 0x238), so a field that
      is absent from the dump is genuinely absent, not truncated.       */
struct<Actor><0x600>

/* 4. Dimension layout. Our candidate is 0xD0 and the fallbacks reach
      0xD8; 0x200 is comfortable.                                       */
struct<Dimension><0x200>
```

**Syntax note:** these are exactly the forms already verified in this project's runs —
`findclass<>`, `fullvf<Class>`, `struct<Class><0xSIZE>` (no `<1>` register hint; that hint is
deliberately omitted because it can disturb the scan). I have not used `strfn<>` because its
`fn_start` was found unreliable, and I have not used any `add<>` (proven to be a silent no-op).

---

## C. Expected output

Three things, in this order of importance.

### C1. From `struct<Actor><0x600>` — the decisive evidence

I need the dump to show, for the candidate offsets in our profile:

| Field | Our unproven candidate | What proves it |
|---|---|---|
| `Actor::mLevel` | `0x1F0` | a slot whose type is a pointer/`WeakRef` to `Level`, sitting where a `Level*` load appears |
| `Actor::mDimension` | `0x1E0` | a slot whose type is `Dimension*` / an automatic-ref to `Dimension` |
| `Actor::mStateVectorComponent` | `0x228` | a sub-object whose `+0x0` reads as three floats |

Our profile's values are **guesses with no `origin` recorded**. If the dump disagrees, the dump
wins and the profile gets corrected — that is the entire point of this request.

### C2. From `struct<Dimension><0x200>` — the block source

| Field | Our unproven candidate | What proves it |
|---|---|---|
| `Dimension::mBlockSource` | `0xD0` (fallbacks `0xC8`, `0xD8`) | a `BlockSource*` / `std::unique_ptr<BlockSource>` member |

Corroborating detail that would be very useful: any member in this dump whose type names
`BlockSource`, and whether it is a raw pointer, a reference wrapper, or held by value.

### C3. From `findclass<Actor>` / `fullvf<Actor>` — existence and shape

- The exact RTTI name string (is it bare `Actor`, or nested, and is there a `WeakEntityRef`
  involved).
- Whether the vtable resolved at all. `struct<>` needs a vtable, so if `fullvf<Actor>` is empty
  and `struct<Actor>` is empty too, that tells me the name is wrong rather than the layout.

### What a good result looks like

A `struct<Actor><0x600>` block listing offset/type/size pairs including something recognisable
at or near `0x1E0` and `0x1F0`, plus a `struct<Dimension>` block with a `BlockSource` pointer
near `0xD0`. With those, I can set the offsets from evidence instead of a guess, the probe will
promote them on the first live frame, and the mod starts working.

### What a bad result looks like

- All four sections empty → the class names do not exist in 1.26.6x under those spellings, and
  I will need a different entry point (see D3).
- `struct<Actor>` present but containing no `Level`/`Dimension`-typed member anywhere in
  `0x600` → the fields are held behind a nested sub-object, and I will request that sub-object's
  layout next rather than guess again.

---

## D. Analysis objective

**What this must establish:** the byte offsets of `Actor::mLevel`, `Actor::mDimension`, and
`Dimension::mBlockSource` in **1.26.6x**, sufficiently proven that the runtime probe in
`World::probeActorLevelAndDimension()` can confirm them on a live player and promote them from
"candidate" to "proven".

**Why these three and only these three:** they are the exact chain the crash came from —
`localPlayer -> mLevel`, `localPlayer -> mDimension -> mBlockSource`. Goal 1 (live actor access)
and goal 2 (block identification) both run through this chain. Nothing else in the profile is
blocking anything right now: `actor_state_vector` (`0x228`) and `actor_aabb_component` (`0x230`)
already have working runtime probes with geometric validation, so they can self-correct without
Zaphkiel. These three cannot — a wrong `mLevel` is not detectably wrong until you dereference it,
which is what killed the process.

**D1 — how the result will be used.** The values become the `candidates` arrays in
`resources/signatures.json`. They stay *candidates*: the runtime probe still has to confirm them
against `rtti::className()` on a live object before `Resolver::offset()` will hand them out. The
Zaphkiel result narrows the hypothesis; the live probe remains the only thing that can promote.

**D2 — what I will not do with the result.** I will not write a hook, patch, or function call
against a layout that this dump does not show. If the dump shows the members, I update the
profile. If it does not, the correct answer is "not found yet", not "try 0x1F8 and see".

**D3 — if `Actor` does not resolve.** Fallback, in order, and I will pick based on what comes
back rather than asking you to run all three blind:
`hier<LocalPlayer>` (we already have `LocalPlayer`'s vtable at RVA `0x139f74e0`, 247 slots, so the
hierarchy walk has a confirmed starting point) → `findclass<Player>` → `findclass<ActorUniqueID>`.

---

## Notes for the operator

- Run this **fresh**, in the same session where Minecraft has been loaded through LeviLauncher.
  `findclass`/`fullvf` need the RTTI to be registered.
- `vfc<>` truncates the output file and must appear at most once — this request uses **none**,
  so that constraint does not apply.
- If the output looks truncated, check the file's line count and size and whether the last line
  is partial **before** concluding it was capped. That has bitten us twice.
- Send back `zaphkiel_out.txt` (and `signatures.txt`/`signatures.json` if produced). I will
  reconcile the declared counts against the output rather than trusting a summary.

---

## What ships in this same test cycle

You said to do all three at once. Here is what is in the build you are about to test, so the
result is interpretable:

| # | Change | Expected effect on device |
|---|---|---|
| 1 | `Resolver::offset()` fails closed — an unproven offset reads as `0` and is never dereferenced | **No more crash on world join.** The mod will instead report `offsets proven: 0/11` (or a small number) and do nothing. |
| 2 | `World::mSnapshot` is mutex-guarded; `snapshot()` returns a copy; the frame hook is the **only** tick driver | No more freeze on toggle; no torn reads between the render and update threads. |
| 3 | This Zaphkiel request | Gives me the three offsets to make 1 and 2 stop mattering. |

**So the expected device result of this build is "no crash, little visible function, and a
diagnostics dump that says exactly what is still unproven."** That is the intended outcome —
it is the difference between a crash and a to-do list.

The two numbers I need from the dump afterwards:

```
   offsets proven:   N/11
2. signatures:       N/13 resolved
```

If `offsets proven` is `0` and `signatures` is `13/13`, then presence and hooks are fine and only
the offsets are missing — which is exactly what phase 3 fixes.
