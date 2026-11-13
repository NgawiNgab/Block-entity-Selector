#!/usr/bin/env bash
# Builds and runs the host-side unit tests without the Android toolchain.
#
# Needs only a C++20 compiler plus nlohmann/json headers; the preloader SDK is
# replaced by tests/stubs. On Debian/Ubuntu:
#   sudo apt install g++ nlohmann-json3-dev
set -euo pipefail
cd "$(dirname "$0")/.."

# The Zaphkiel converter is pure Python and gets tested alongside the C++ code.
if command -v python3 >/dev/null 2>&1; then
    python3 tools/zaphkiel/test_apply_output.py
else
    echo "python3 not found; skipping the converter tests" >&2
fi

echo

CXX=${CXX:-g++}
OUT=${OUT:-build/tests/bes-tests}
mkdir -p "$(dirname "$OUT")"

"$CXX" -std=c++20 -O1 -g -frtti \
    -Iinclude -Isrc -Itests/stubs \
    -o "$OUT" \
    tests/test_main.cpp \
    src/core/Profile.cpp src/core/Rtti.cpp src/core/Logging.cpp src/core/AddressSpace.cpp \
    src/core/Resolver.cpp \
    src/selection/Selection.cpp src/selection/Identifier.cpp src/selection/ScannerState.cpp \
    -ldl

"$OUT"
