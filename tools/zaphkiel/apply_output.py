#!/usr/bin/env python3
"""Converts Zaphkiel output into a signatures.json for Block Entity Selector.

Typical use on a phone:

    python3 tools/zaphkiel/apply_output.py \\
        --signatures /sdcard/.../kurumi/signatures.json \\
        --analysis   /sdcard/.../kurumi/zaphkiel_out.txt \\
        --base       resources/signatures.json \\
        --output     build/signatures.json

The tool is intentionally forgiving: it merges whatever it can recognise and
reports what it skipped, so a partial Zaphkiel run still produces a usable
profile instead of failing outright.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

# Keys are the JSON names used by resources/signatures.json. The value lists the
# substrings that identify the matching function in Zaphkiel output.
SIGNATURE_HINTS: dict[str, tuple[str, ...]] = {
    "render_level": ("LevelRenderer::renderLevel", "renderLevel"),
    "tessellator_begin": ("Tessellator::begin",),
    "tessellator_color": ("Tessellator::color",),
    "tessellator_vertex": ("Tessellator::vertex",),
    "render_mesh_immediately": ("renderMeshImmediately", "MeshHelpers::renderMeshImmediately"),
    "actor_manager_list": ("getRuntimeActorList", "ActorManager::getRuntimeActorList"),
    "actor_is_player": ("Actor::isPlayer",),
    "level_get_hit_result": ("Level::getHitResult", "getHitResult"),
    "hit_result_get_entity": ("HitResult::getEntity",),
    "block_source_get_block": ("BlockSource::getBlock",),
    "render_material_group": ("RenderMaterialGroup",),
    "client_instance_update": ("ClientInstance::update",),
    "client_instance_get_local_player": ("ClientInstance::getLocalPlayer",),
}

OFFSET_HINTS: dict[str, tuple[str, ...]] = {
    "client_instance_level_renderer": ("ClientInstance::mLevelRenderer",),
    "level_renderer_player": ("LevelRenderer::mLevelRendererPlayer",),
    "level_renderer_player_cam_pos": ("LevelRendererPlayer::mCamPos",),
    "screen_context_tessellator": ("ScreenContext::mTessellator",),
    "screen_context_color_holder": ("ScreenContext::mColorHolder",),
    "actor_state_vector": ("Actor::mStateVectorComponent",),
    "actor_aabb_component": ("Actor::mAABBShape", "Actor::mStateVectorComponent + 8"),
    "actor_level": ("Actor::mLevel",),
    "actor_dimension": ("Actor::mDimension",),
    "dimension_block_source": ("Dimension::mBlockSource", "BlockSource"),
    "level_actor_manager": ("Level::mActorManager",),
}

# Zaphkiel prints one target per block:
#
#   fn=0xc61ad30  LevelRenderer::renderLevel(LevelRendererPlayer&, ScreenContext&)
#     sig: 3F 23 03 D1 F8 5F 01 A9 ...
#     offset +0x430  LevelRenderer::mLevelRendererPlayer
#
# so the function header, the pattern and the offsets are three separate shapes.
FUNCTION_LINE = re.compile(r"^(?:fn|add|vft|struct|findclass|strfn|entt)\s*=\s*0x(?P<addr>[0-9a-fA-F]+)\s*(?P<label>.*)$")
SIG_LINE = re.compile(r"^sig\s*:\s*(?P<pattern>[0-9a-fA-F?][0-9a-fA-F? ]{7,})$")
OFFSET_LINE = re.compile(r"^offset\s*\+?0x(?P<value>[0-9a-fA-F]+)\s*(?:[—\-]+|:)?\s*(?P<label>.*)$")
# Address-only echoes: "0x8a3f20  LevelRenderer::renderLevel" and the progress
# line form "[+] LevelRenderer::renderLevel -> 0x8a3f20".
ADDRESS_LINE = re.compile(r"^0x(?P<addr>[0-9a-fA-F]{6,16})\s+(?P<label>[A-Za-z0-9_:<>~ ]+)$")
PROGRESS_LINE = re.compile(r"^(?:\[[^\]]*\]\s*)?(?P<label>[A-Za-z0-9_:<>~ ]+?)\s*(?:->|@|=)\s*0x(?P<addr>[0-9a-fA-F]{4,16})$")


def load_json(path: Path | None) -> dict:
    if path is None or not path.exists():
        return {}
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as error:
        print(f"warning: {path} is not valid JSON: {error}", file=sys.stderr)
        return {}


def match_hint(text: str, hints: dict[str, tuple[str, ...]]) -> str | None:
    for key, needles in hints.items():
        for needle in needles:
            if needle and needle in text:
                return key
    return None


def collect_from_analysis(path: Path) -> tuple[dict[str, list[str]], dict[str, list[str]], list[str]]:
    """Returns (signature patterns, offsets, notes) found in zaphkiel_out.txt."""
    patterns: dict[str, list[str]] = {}
    offsets: dict[str, list[str]] = {}
    notes: list[str] = []

    if not path or not path.exists():
        return patterns, offsets, notes

    current_key: str | None = None
    for raw_line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw_line.strip()
        if not line:
            continue

        # A new target starts here: remember which JSON key it belongs to so a
        # following "sig:" line can be attributed even when the pattern itself
        # carries no label.
        function_match = FUNCTION_LINE.match(line)
        if function_match:
            current_key = match_hint(function_match.group("label"), SIGNATURE_HINTS)
            if current_key:
                notes.append(f"target {current_key} -> {function_match.group('label')[:80]}")
            continue

        address_match = ADDRESS_LINE.match(line) or PROGRESS_LINE.match(line)
        if address_match:
            current_key = match_hint(address_match.group("label"), SIGNATURE_HINTS) or current_key
            continue

        if line.startswith("[") or set(line) <= {"=", "-"}:
            continue

        sig_match = SIG_LINE.match(line)
        if sig_match:
            key = current_key or match_hint(line, SIGNATURE_HINTS)
            if key:
                patterns.setdefault(key, []).append(sig_match.group("pattern").strip())
            continue

        offset_match = OFFSET_LINE.match(line)
        if offset_match:
            key = match_hint(offset_match.group("label"), OFFSET_HINTS) or match_hint(line, OFFSET_HINTS)
            if key:
                offsets.setdefault(key, []).append("0x" + offset_match.group("value").upper())

    return patterns, offsets, notes


def collect_from_signature_json(path: Path) -> tuple[dict[str, list[str]], list[str]]:
    """Reads Zaphkiel's signatures.json and maps entries by their labels."""
    patterns: dict[str, list[str]] = {}
    notes: list[str] = []

    document = load_json(path)
    if not document:
        return patterns, notes

    entries = document.get("signatures", document) if isinstance(document, dict) else document
    if isinstance(entries, dict):
        entries = [dict(value, name=key) if isinstance(value, dict) else {"name": key, "pattern": value}
                   for key, value in entries.items()]

    for entry in entries if isinstance(entries, list) else []:
        if not isinstance(entry, dict):
            continue
        label = str(entry.get("name") or entry.get("label") or entry.get("comment") or "")
        pattern = entry.get("pattern") or entry.get("signature")
        key = match_hint(label, SIGNATURE_HINTS)
        if key and isinstance(pattern, str) and pattern:
            patterns.setdefault(key, []).append(pattern)
            notes.append(f"signature {key} <- {label}")
    return patterns, notes


def merge(base: dict, patterns: dict[str, list[str]], offsets: dict[str, list[str]],
          game_version: str | None) -> dict:
    result = json.loads(json.dumps(base))  # deep copy
    result.setdefault("signatures", {})
    result.setdefault("offsets", {})

    for key, found in patterns.items():
        candidates = [{"pattern": pattern, "note": "zaphkiel"} for pattern in dict.fromkeys(found)]
        if not candidates:
            continue
        existing = result["signatures"].get(key, {})
        if isinstance(existing, str):
            existing = {"candidates": [existing]}
        old = existing.get("candidates", []) if isinstance(existing, dict) else []
        # Freshly generated patterns win: they match the build being profiled.
        # Older entries with the same pattern text are dropped rather than kept
        # with their old note, so a re-run does not double the list.
        seen = {candidate["pattern"] for candidate in candidates}
        kept = [c for c in old if isinstance(c, dict) and c.get("pattern") not in seen]
        result["signatures"][key] = {"candidates": candidates + kept}

    for key, found in offsets.items():
        candidates = [{"offset": value, "note": "zaphkiel"} for value in dict.fromkeys(found)]
        if not candidates:
            continue
        existing = result["offsets"].get(key, {})
        if isinstance(existing, str):
            existing = {"candidates": [existing]}
        old = existing.get("candidates", []) if isinstance(existing, dict) else []
        # Same dedupe rule, keyed on the numeric value (case-insensitive).
        # When Zaphkiel confirms a value the profile already had, keep the old
        # note: it carries the struct/member name, which is what the diagnostics
        # dump shows and what a human reads.
        notes = {str(c.get("offset", "")).upper(): c.get("note", "") for c in old if isinstance(c, dict)}
        merged = [{"offset": candidate["offset"], "note": notes.get(str(candidate["offset"]).upper(), candidate["note"])}
                  for candidate in candidates]
        seen = {str(candidate["offset"]).upper() for candidate in candidates}
        kept = [c for c in old if isinstance(c, dict) and str(c.get("offset", "")).upper() not in seen]
        result["offsets"][key] = {"candidates": merged + kept}

    if game_version:
        result["game_version"] = game_version
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--signatures", type=Path, help="Zaphkiel signatures.json output")
    parser.add_argument("--analysis", type=Path, help="Zaphkiel zaphkiel_out.txt output")
    parser.add_argument("--base", type=Path, help="starting profile (default: resources/signatures.json)")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--game-version", help="value for the profile's game_version field")
    parser.add_argument("--dry-run", action="store_true", help="print the result instead of writing it")
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parents[2]
    base_path = args.base or (repo_root / "resources" / "signatures.json")
    base = load_json(base_path)

    pattern_notes: list[str] = []
    patterns: dict[str, list[str]] = {}
    offsets: dict[str, list[str]] = {}

    if args.signatures:
        found, notes = collect_from_signature_json(args.signatures)
        patterns.update(found)
        pattern_notes.extend(notes)

    if args.analysis:
        found_patterns, found_offsets, notes = collect_from_analysis(args.analysis)
        patterns.update(found_patterns)
        offsets.update(found_offsets)
        pattern_notes.extend(notes)

    if not patterns and not offsets:
        print("No Zaphkiel data matched. Check that the files exist and the mod "
              "targets in zInput.txt were run.", file=sys.stderr)
        return 1

    merged = merge(base, patterns, offsets, args.game_version)
    payload = json.dumps(merged, indent=2) + "\n"

    if args.dry_run:
        print(payload)
    else:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(payload, encoding="utf-8")
        print(f"wrote {args.output}")

    for key in sorted(set(patterns) | set(offsets)):
        source = []
        if key in patterns:
            source.append(f"{len(patterns[key])} pattern(s)")
        if key in offsets:
            source.append(f"{len(offsets[key])} offset(s)")
        print(f"  {key}: {', '.join(source)}")

    for note in pattern_notes[:20]:
        print(f"  note: {note}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
