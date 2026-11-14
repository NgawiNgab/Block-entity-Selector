# ZAPHKIEL REQUEST — `Actor::mLevel`, `Actor::mDimension`, `Dimension::mBlockSource`

**Supersedes the previous request in this file's history, which was wrong.** The error was mine:
I asked for `struct<Actor><0x600>` believing the second argument set a *scan range*. It does not.
`struct<Level><0x180>` means "find every access to offset `+0x180`", and the tool confirms this in
its own output:

```
scanning 401 method(s) of "Actor" ... for [xN, #0x600]
```

So the run scanned offset `0x600` only. Of course it produced nothing useful.

---

## A. Canonical forms, and the four-entry limit

`vfc<>` truncates the output file, so **at most one per run**. `struct<>` is cheap by comparison
but still writes into the same file, and a 24,456-line output is already hard to send.

To keep this to a single run I have moved the *discovery* work into `fullf<>`, which is where it
belongs: two `fullf<>` calls at known functions plus four `struct<>` scans is a better use of the
budget than guessing offsets.

### Required configuration

`/storage/emulated/0/Android/media/org.levimc.launcher/kurumi/zaphkiel_config.txt`

| Key | Set to | Why |
|---|---|---|
| `max_fn_insns` | `400` | Already at this value from the last request. Keep it. |
| `startup_delay_secs` | `15` | Keep. |
| `dump_rtti` | `true` | Keep. |
| `separate_dump_files` | `true` | Keep. |

No other key changes.

---

## B. Complete zInput

Copy verbatim to `/storage/emulated/0/Android/media/org.levimc.launcher/kurumi/zInput.txt`.
Five entries. No absolute addresses — the two `fullf<>` addresses are RVAs from the *previous
run's* disassembly of the same build, and I state the base they were measured from below.

```
/* ------------------------------------------------------------------
   Request 2: Actor::mLevel / mDimension, Dimension::mBlockSource
   1.26.6x, build c60fa32908019eecae39ebfe7bac5755dc35cde2
   struct<> scans ONE offset, so each candidate is its own entry.
   ------------------------------------------------------------------ */

/* --- 1. Actor::mLevel -------------------------------------------------
   Hypothesis A: the datum is a WeakRef<Level> at +0x1f0.
   Hypothesis B: the WeakRef wrapper starts at +0x1e8 and the Level* is
   the second word, so the raw pointer is at +0x1f0.
   These two are indistinguishable from the caller side, which is why
   both offsets go in.                                              */
struct<Actor><0x1e8>

/* --- 2. Actor::mLevel, the pointer word itself ---------------------- */
struct<Actor><0x1f0>

/* --- 3. Actor::mDimension -------------------------------------------
   Same shape: read ~20 times as a bare pointer that is passed straight
   to a BL, which fits StrongEntitySyncPacket::getDimensionId().
   Candidates are the profile's 0x1e0 plus the 0x1e8 boundary.       */
struct<Actor><0x1e0>

/* --- 4. Dimension::mBlockSource -------------------------------------
   Profile says 0xd0, fallbacks 0xc8 / 0xd8. Only the primary is
   requested here; if it comes back empty I will ask for the other two
   rather than spend the budget guessing.                            */
struct<Dimension><0xd0>

/* --- 5. Actor slot 0x1a0 --------------------------------------------
   fullvf<Actor> showed a getter `ldr x0,[x0,#0x1a0]; ret` at RVA
   0xfda5664 -- the ONLY clean single-field getter in all 137 slots.
   Whatever that field is, it is a cheap, unambiguous anchor for
   sanity-checking the Actor layout. Confirm what it returns.        */
fullf<0xfda5664>
```

**Address note:** `0xfda5664` is an **RVA**, measured against base `0x79726af000`, which is why it
can be passed as-is to `fullf<>` (Zaphkiel resolves RVAs into the loaded image). If it reports
"no function at address", the build drifted and I will re-derive it — do not adjust the number by
hand.

---

## C. Expected output

### C1. The four `struct<>` scans

Each prints `scanning N method(s) ... for [xN, #OFFSET]`, a list of `LDR`/`STR` sites, and a
decompile of each unique function. What decides the question:

| Entry | Proves mLevel if the dump shows |
|---|---|
| `struct<Actor><0x1e8>` | an `ldr`/`str` here whose value is a **reference-counted wrapper** (a `bl` to an increment/decrement helper on the same pointer), i.e. `WeakRef`/`AutomaticRef` semantics |
| `struct<Actor><0x1f0>` | an `ldr x?, [x?, #0x1f0]` used directly as a **callee's x0** with no further offsetting — a bare `Level*` |
| `struct<Actor><0x1e0>` | the same shape, whose callee takes the level/registry as x1 (matches the `x0=entity, x1=level` pattern already seen) |
| `struct<Dimension><0xd0>` | a slot the class's **own** block-getting code reads to reach `BlockSource` |

The decisive detail I want from each: **what happens to the loaded value immediately after the
load.** Passed as an argument → a raw pointer member. Fed into a refcount helper → a wrapper.
Offset further → a sub-object, and I need the nested offset instead.

### C2. `fullf<0xfda5664>` — the anchor

Expected: `ldr x0, [x0, #0x1a0]` then `ret`, three instructions total (the run already showed
this). I want the callers it reports, because *who* asks for that field tells me what the field
is, and that in turn corroborates that the offsets around it (`0x1e0`, `0x1e8`, `0x1f0`, `0x1f8`)
belong to the same object.

### C3. Good vs bad result

- **Good:** `struct<Actor><0x1f0>` and `<0x1e0>` both return sites, and the context shows a bare
  pointer being passed to a callee. Then I set the profile from evidence and the runtime probe
  promotes them on the first live frame.
- **Bad:** any entry returns "0 access site(s)". That means nothing in Actor's 401 scanned methods
  touches that offset, so the field lives in a base class (`Actor` derives from something), and I
  will request `hier<Actor>` before asking for anything else. I would rather spend one run on the
  hierarchy than another on guesses.

---

## D. Analysis objective

**Establish, for 1.26.6x:** whether `Actor::mLevel` and `Actor::mDimension` hold a raw pointer or
a reference-counted wrapper, and at which byte offsets; and where `Dimension` keeps its
`BlockSource`.

**Why these, still:** `localPlayer → mLevel` and `localPlayer → mDimension → mBlockSource` are the
two chains that goals 1 and 2 depend on. Everything else in the profile either has a working
runtime probe (`actor_state_vector`, `actor_aabb_component`) or is not on the critical path.

**What I will do with the answer:** update `candidates` in `resources/signatures.json`. They stay
candidates — `Resolver::offset()` will not serve them until `World::probeActorLevelAndDimension()`
confirms them against `rtti::className()` on a live object. Zaphkiel narrows the hypothesis; the
live probe is still the only thing that can promote.

**What I will not do:** call, patch, or hook anything based on a layout this dump does not show.
If an entry is empty, the answer is "not found", not "try the neighbour and see".

### A correction to hold on to

The previous run's `0/11 proven` was **not** evidence that these offsets are wrong. It was
evidence that the memory map was truncated at 4096 regions while the device maps 6022, so nothing
was testable at all. That is fixed in `0f34409`. This request is therefore the *first* real test
of the offset table, not a second opinion on a failed one — which is worth remembering before
concluding the profile is bad.

---

## Notes for the operator

- Run in the session where Minecraft is loaded through LeviLauncher; `struct<>` needs RTTI
  registered and a vtable to walk.
- One `vfc<>` maximum per run — this request uses none.
- If output looks truncated, check line count **and** file size **and** whether the last line is
  partial before concluding it was capped.
- Send back `zaphkiel_out.txt`. If it is large, the four `STRUCT:` sections plus `fullf` are all I
  need.
