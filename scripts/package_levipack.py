#!/usr/bin/env python3
"""Builds BlockEntitySelector.levipack from the compiled library.

The package layout follows the LeviLauncher "preload-native" manifest format:
manifest.json + the .so entry point + icon + resources.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import zipfile
from pathlib import Path

VALUE_PATTERN = re.compile(
    r'^\s*inline\s+constexpr\s+std::string_view\s+(Name|Author|Description|Version)\s*=\s*"((?:\\.|[^"\\])*)";\s*$'
)
REQUIRED_VALUES = ("Name", "Author", "Description", "Version")


def parse_version(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        match = VALUE_PATTERN.match(line)
        if match:
            values[match.group(1)] = bytes(match.group(2), "utf-8").decode("unicode_escape")
    missing = [name for name in REQUIRED_VALUES if not values.get(name)]
    if missing:
        raise ValueError("Missing version metadata: " + ", ".join(missing))
    return values


def build_manifest(values: dict[str, str], library_name: str) -> dict[str, object]:
    return {
        "type": "preload-native",
        "name": values["Name"],
        "author": values["Author"],
        "description": values["Description"],
        "version": values["Version"],
        "entry": library_name,
        "icon": "icon.png",
        "overwrite_files": ["icon.png", "resources/signatures.json"],
        "overwrite_folders": [],
    }


def write_package(
    library: Path,
    icon: Path,
    profiles: Path,
    version_header: Path,
    output: Path,
    entry_name: str | None = None,
) -> None:
    for path, label in (
        (library, "Library"),
        (icon, "Icon"),
        (profiles, "Signature profile"),
        (version_header, "Version header"),
    ):
        if not path.is_file():
            raise FileNotFoundError(f"{label} not found: {path}")

    # The manifest entry must match the .so name produced by the build. Renaming
    # here keeps the manifest honest when the build output has a different name
    # (for example a CI artifact such as libBlockEntitySelector.so.debug).
    library_name = entry_name or library.name
    manifest = build_manifest(parse_version(version_header), library_name)

    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists():
        output.unlink()

    manifest_bytes = (json.dumps(manifest, indent=2, ensure_ascii=False) + "\n").encode("utf-8")
    entries = {
        "manifest.json": manifest_bytes,
        library_name: library.read_bytes(),
        "icon.png": icon.read_bytes(),
        "resources/signatures.json": profiles.read_bytes(),
    }

    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name, payload in entries.items():
            archive.writestr(name, payload)

    with zipfile.ZipFile(output, "r") as archive:
        names = set(archive.namelist())
        if names != set(entries):
            raise RuntimeError(f"Unexpected package entries: {sorted(names)}")
        if json.loads(archive.read("manifest.json")) != manifest:
            raise RuntimeError("Manifest verification failed")
        for name, payload in entries.items():
            if archive.getinfo(name).file_size != len(payload):
                raise RuntimeError(f"Entry verification failed: {name}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", required=True, type=Path)
    parser.add_argument("--icon", required=True, type=Path)
    parser.add_argument("--profiles", required=True, type=Path)
    parser.add_argument("--version-header", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument(
        "--entry-name",
        help="Name the library gets inside the package (defaults to the file's own name). "
        "Use this when the built .so is not already called libBlockEntitySelector.so.",
    )
    args = parser.parse_args()
    try:
        write_package(
            args.library.resolve(),
            args.icon.resolve(),
            args.profiles.resolve(),
            args.version_header.resolve(),
            args.output.resolve(),
            args.entry_name,
        )
    except Exception as error:  # noqa: BLE001 - surfaced to the build log
        print(error, file=sys.stderr)
        return 1
    print(args.output.resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
