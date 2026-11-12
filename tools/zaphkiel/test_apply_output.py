#!/usr/bin/env python3
"""Tests for the Zaphkiel output converter.

Run directly (`python3 tools/zaphkiel/test_apply_output.py`) or through
`tools/run_tests.sh`, which runs it alongside the C++ tests.
"""

from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent

_spec = importlib.util.spec_from_file_location("apply_output", HERE / "apply_output.py")
assert _spec and _spec.loader
apply_output = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(apply_output)

# Shapes Zaphkiel actually emits: a `fn=` header with the pattern and offsets
# under it, and a one-line progress echo.
ANALYSIS = """\
=== sig_targets.txt -> 1.26.60 ===
fn=0x8A3F20  LevelRenderer::renderLevel(LevelRendererPlayer&, ScreenContext&)
  sig: 3F 23 03 D1 F8 5F 01 A9 F6 57 02 A9 F4 4F 03 A9
  offset +0x430  LevelRenderer::mLevelRendererPlayer
  offset +0x614  LevelRendererPlayer::mCamPos
fn=0x71F0C4  Tessellator::begin(DebugCallback*, int, int, int)
  sig: FF 43 01 D1 FE 0B 00 F9 FD 03 00 91
  offset +0xB0  ScreenContext::mTessellator
[+] BlockSource::getBlock -> 0x51C0A8
[!] HitResult::getEntity -> not found
"""

ZAPHKIEL_SIGNATURES = {
    "signatures": [
        {"name": "ActorManager::getRuntimeActorList", "pattern": "AA BB CC DD EE FF 00 11 ? ? 22 33"},
        {"name": "ClientInstance::update", "pattern": "11 22 33 44 55 66 77 88"},
        {"name": "Something::Unknown", "pattern": "00 11"},
    ]
}


class ConverterTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.analysis = self.tmp / "zaphkiel_out.txt"
        self.analysis.write_text(ANALYSIS, encoding="utf-8")
        self.signatures = self.tmp / "signatures.json"
        self.signatures.write_text(json.dumps(ZAPHKIEL_SIGNATURES), encoding="utf-8")
        self.base = REPO / "resources" / "signatures.json"

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def run_converter(self, base: Path | None = None, output: Path | None = None) -> dict:
        """Invokes the CLI the way a user would, then reads the result back."""
        output = output or (self.tmp / "merged.json")
        argv = [
            "apply_output.py",
            "--signatures", str(self.signatures),
            "--analysis", str(self.analysis),
            "--base", str(base or self.base),
            "--output", str(output),
            "--game-version", "1.26.60",
        ]
        previous = sys.argv
        sys.argv = argv
        try:
            code = apply_output.main()
        finally:
            sys.argv = previous
        self.assertEqual(code, 0, "converter reported failure")
        return json.loads(output.read_text(encoding="utf-8"))

    def candidates(self, profile: dict, key: str) -> list[dict]:
        entry = profile["signatures"].get(key) or profile["offsets"].get(key)
        self.assertIsNotNone(entry, f"{key} missing from the merged profile")
        return entry["candidates"]

    def test_patterns_and_offsets_are_mapped(self) -> None:
        profile = self.run_converter()

        patterns = [c["pattern"] for c in self.candidates(profile, "render_level")]
        self.assertIn("3F 23 03 D1 F8 5F 01 A9 F6 57 02 A9 F4 4F 03 A9", patterns)
        patterns = [c["pattern"] for c in self.candidates(profile, "tessellator_begin")]
        self.assertIn("FF 43 01 D1 FE 0B 00 F9 FD 03 00 91", patterns)

        offsets = {c["offset"].upper() for c in self.candidates(profile, "level_renderer_player")}
        self.assertIn("0X430", offsets)
        offsets = {c["offset"].upper() for c in self.candidates(profile, "screen_context_tessellator")}
        self.assertIn("0XB0", offsets)

        # A function that Zaphkiel located but printed no pattern for is still
        # not silently dropped from the base profile.
        self.assertTrue(self.candidates(profile, "block_source_get_block"))

    def test_generated_candidates_come_first(self) -> None:
        """The pattern from this build must be tried before the bundled fallback."""
        profile = self.run_converter()
        candidates = self.candidates(profile, "render_level")
        self.assertEqual(candidates[0]["pattern"], "3F 23 03 D1 F8 5F 01 A9 F6 57 02 A9 F4 4F 03 A9")
        self.assertGreater(len(candidates), 1, "the old pattern should survive as a fallback")

    def test_duplicate_values_are_collapsed(self) -> None:
        """Re-running the converter must not grow the candidate lists."""
        first = self.run_converter()
        second = self.run_converter(base=self.tmp / "merged.json", output=self.tmp / "merged2.json")
        self.assertEqual(first, second)

        for key in ("render_level", "tessellator_begin"):
            patterns = [c["pattern"] for c in self.candidates(first, key)]
            self.assertEqual(len(patterns), len(set(patterns)), f"{key} has duplicate patterns")
        for key in ("level_renderer_player", "screen_context_tessellator"):
            values = [c["offset"].upper() for c in self.candidates(first, key)]
            self.assertEqual(len(values), len(set(values)), f"{key} has duplicate offsets")

    def test_existing_note_is_kept(self) -> None:
        """A confirmed offset keeps the readable member name from the base profile."""
        profile = self.run_converter()
        notes = [c["note"] for c in self.candidates(profile, "level_renderer_player")]
        self.assertIn("LevelRenderer::mLevelRendererPlayer", notes)

    def test_unrecognised_entries_are_reported_not_invented(self) -> None:
        profile = self.run_converter()
        # "Something::Unknown" matches no hint, so it must not appear anywhere.
        dumped = json.dumps(profile)
        self.assertNotIn("00 11\n", dumped)
        self.assertNotIn("Something::Unknown", dumped)

    def test_game_version_is_recorded(self) -> None:
        profile = self.run_converter()
        self.assertEqual(profile["game_version"], "1.26.60")

    def test_missing_inputs_do_not_produce_a_profile(self) -> None:
        argv = [
            "apply_output.py",
            "--base", str(self.base),
            "--output", str(self.tmp / "empty.json"),
        ]
        previous = sys.argv
        sys.argv = argv
        try:
            code = apply_output.main()
        finally:
            sys.argv = previous
        self.assertNotEqual(code, 0)
        self.assertFalse((self.tmp / "empty.json").exists())

    def test_unreadable_json_is_tolerated(self) -> None:
        self.signatures.write_text("{ not json", encoding="utf-8")
        profile = self.run_converter()
        # The analysis file still supplied patterns, so a usable profile exists.
        self.assertTrue(self.candidates(profile, "render_level"))


class BaseProfileTests(unittest.TestCase):
    def test_bundled_profile_is_valid_and_complete(self) -> None:
        """The shipped profile must parse and cover every documented id."""
        profile = json.loads((REPO / "resources" / "signatures.json").read_text(encoding="utf-8"))
        for section in ("signatures", "offsets"):
            self.assertIn(section, profile)
            for key, entry in profile[section].items():
                self.assertIsInstance(entry, dict, key)
                self.assertIn("candidates", entry, key)
                self.assertTrue(entry["candidates"], f"{key} has no candidates")
                for candidate in entry["candidates"]:
                    self.assertTrue(
                        candidate.get("pattern") or candidate.get("offset") or candidate.get("rtti"),
                        f"{key} has a candidate with neither pattern, offset nor rtti",
                    )

    def test_every_offset_candidate_parses_as_hex(self) -> None:
        profile = json.loads((REPO / "resources" / "signatures.json").read_text(encoding="utf-8"))
        for key, entry in profile["offsets"].items():
            for candidate in entry["candidates"]:
                raw = str(candidate["offset"])
                self.assertTrue(raw.lower().startswith("0x"), f"{key}: {raw} is not 0x-prefixed")
                int(raw, 16)  # raises when malformed


if __name__ == "__main__":
    unittest.main(verbosity=2)
