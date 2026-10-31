#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Builds mmap_check against the Detour library of the core's source tree (DETOUR_DIR, default the core checkout on the build server).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
DETOUR_DIR="${DETOUR_DIR:-/srv/forever/src/Trinity-Forever-Modules/dep/recastnavigation/Detour}"
OUT="${1:-$HERE/mmap_check}"
g++ -O2 -std=c++17 -I"$DETOUR_DIR/Include" "$HERE/mmap_check.cpp" "$DETOUR_DIR"/Source/Detour{Alloc,Assert,Common,NavMesh,NavMeshQuery,Node}.cpp -o "$OUT"
echo "built $OUT"
