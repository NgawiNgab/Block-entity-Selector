#!/usr/bin/env bash
# Builds and runs the host-side unit tests without the Android toolchain.
#
# Needs only a C++20 compiler plus nlohmann/json headers; the preloader SDK is
# replaced by tests/stubs. On Debian/Ubuntu:
#   sudo apt install g++ nlohmann-json3-dev
set -euo pipefail
cd "$(dirname "$0")/.."

CXX=${CXX:-g++}
OUT=${OUT:-build/tests/bes-tests}
mkdir -p "$(dirname "$OUT")"

"$CXX" -std=c++20 -O1 -g -frtti \
    -Iinclude -Isrc -Itests/stubs \
    -o "$OUT" \
    tests/test_main.cpp src/core/Profile.cpp src/core/Rtti.cpp src/core/Logging.cpp \
    -ldl

"$OUT"
