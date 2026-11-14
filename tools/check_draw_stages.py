#!/usr/bin/env python3
"""Static check: every exit from HighlightRenderer::draw() records a RenderStage.

Why this exists: the diagnostics dump used to report a single "renderer frames"
counter, so a count of 0 conflated every way the draw path can bail out -- no
tessellator, no camera, no material, empty geometry, no targets. RenderStage
separates those cases. That only holds if each early return actually notes its
stage, so a newly added `return;` that forgets to call noteStage() would quietly
restore the ambiguity this was built to remove.

The host test build cannot link HighlightRenderer.cpp (it pulls in the Android
loader), so the invariant is checked against the source text instead.

Exit code 0 when every exit is covered, 1 with the offending lines otherwise.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

SOURCE = Path(__file__).resolve().parent.parent / "src/render/HighlightRenderer.cpp"
ENTRY = "void HighlightRenderer::draw("


def draw_body(text: str) -> str:
    start = text.index(ENTRY)
    end = text.index("} // namespace bes::render", start)
    return text[start:end]


def uncovered_exits(body: str) -> list[tuple[int, str]]:
    """Return (line number, source line) for every `return;` lacking a stage note.

    A note may sit on the same line or on the line directly above it, which is
    how the multi-line guard clauses are written.
    """
    bad: list[tuple[int, str]] = []
    for match in re.finditer(r"\breturn;", body):
        line_no = body.count("\n", 0, match.start())
        lines = body.splitlines()
        current = lines[line_no]
        previous = lines[line_no - 1] if line_no else ""
        stripped = current.strip()
        if stripped.startswith("//"):
            continue
        if "noteStage" in current or "noteStage" in previous:
            continue
        # Report as 1-based line numbers within the function body.
        bad.append((line_no + 1, stripped))
    return bad


def main() -> int:
    if not SOURCE.exists():
        print(f"missing source file: {SOURCE}", file=sys.stderr)
        return 1

    body = draw_body(SOURCE.read_text(encoding="utf-8"))
    bad = uncovered_exits(body)
    if bad:
        print("draw() has exits that do not record a RenderStage:")
        for line_no, line in bad:
            print(f"    +{line_no}: {line}")
        return 1

    print("renderer: every draw() exit records a stage")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
