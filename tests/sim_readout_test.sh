#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# tools/sim_readout.sh on a made-up journal (tests/data/sim-readout-journal.log, ISO and classic timestamps, two arms) and characters table. No host, no ssh.
# Usage: sim_readout_test.sh <source dir>
set -u
SRC="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
fails=0
OUT="$(READOUT_LOG="$SRC/tests/data/sim-readout-journal.log" READOUT_CHARS="$SRC/tests/data/sim-readout-chars.tsv" READOUT_NO_HOST=1 \
       bash "$SRC/tools/sim_readout.sh" "2026-11-19 12:00" "2026-11-19 13:59:59")"
rc=$?
expect() {
    if ! grep -qF -- "$1" <<< "$OUT"; then
        echo "FAIL: missing: $1"
        fails=$((fails + 1))
    fi
}
absent() {
    if grep -qF -- "$1" <<< "$OUT"; then
        echo "FAIL: unexpected: $1"
        fails=$((fails + 1))
    fi
}
[ "$rc" -eq 0 ] || { echo "FAIL: exit $rc"; fails=$((fails + 1)); }
expect "window: 2026-11-19T12:00:30 .. 2026-11-19T13:50:00, 26 lines, arm fields yes"
expect "arm=A hour=2026-11-19T12 died=2 bot_hours=1.00 deaths_per_bot_hour=2.00 no_route_per_bot_hour=1.00"
expect "arm=B hour=2026-11-19T12 died=1 bot_hours=1.00 deaths_per_bot_hour=1.00 no_route_per_bot_hour=0.00"
expect "arm=A hour=2026-11-19T13 died=1 bot_hours=2.00 deaths_per_bot_hour=0.50 no_route_per_bot_hour=0.00"
expect "arm=B hour=2026-11-19T13 died=1 bot_hours=2.00 deaths_per_bot_hour=0.50 no_route_per_bot_hour=1.00"
expect "arm=A died=3 bot_hours=3.00 deaths_per_bot_hour=1.00 levels_gained=2 levels_per_bot_hour=0.667 no_route=1 no_route_per_bot_hour=0.33 active_bots=1"
expect "arm=B died=2 bot_hours=3.00 deaths_per_bot_hour=0.67 levels_gained=1 levels_per_bot_hour=0.333 no_route=2 no_route_per_bot_hour=0.67 active_bots=2"
expect "arm=A maps: 0:3"
expect "arm=A bands: 06-10:3"
expect "arm=A killers: 202:3"
expect "11-15:1"            # bot 43's died line has no level: its level now (12) from the characters table
expect "2991:1"
expect "251707:1"
expect "(a died line without level=: the band is the level in the characters table now)"
expect "arm=A bots=1 median=3.0 top10=3 top10_pct=100.0"
expect "arm=B bots=2 median=1.0 top10=2 top10_pct=100.0"
expect "2026-11-19T13:00:00 arm=A deaths=2 online=60 median=0.0 median_died=2.0 top10_pct=100.0"
expect "2026-11-19T13:00:00 arm=all deaths=3 online=120"
expect "arm=A cells_5plus=0 top: 66 0@-10700,200:3"
expect "quest_walk_blocked=1"
expect "quest_walk_blocked reason=level_unsafe=1"
expect "below_map_recover reason=z_below=1"
expect "level_safe_yard=1"
expect "death_loop_escape=1"
expect "level_up=1"
expect "== errors: 1"
expect "could not publish actions: error writing the queue"
expect "hour=2026-11-19T12 bots=120 decide_avg_ms=7.85 decide_max_ms=33.45"
expect "hour=2026-11-19T13 bots=120 decide_avg_ms=11.21 decide_max_ms=46.95"
expect "arm=A per_bot_turn_us=15.4"
expect "arm=A bands: 06-10:1"
expect "arm=B bands: 06-10:1 11-15:1"
expect "arm=B classes: 4:11.0/2"
absent "14:30"
absent "11:59"
absent "== RAM / disk"

# No arguments: usage, exit 2.
bash "$SRC/tools/sim_readout.sh" > /dev/null 2>&1
[ $? -eq 2 ] || { echo "FAIL: no-argument exit"; fails=$((fails + 1)); }
# A window with no lines.
EMPTY="$(READOUT_LOG="$SRC/tests/data/sim-readout-journal.log" READOUT_CHARS=/dev/null READOUT_NO_HOST=1 bash "$SRC/tools/sim_readout.sh" "2026-11-20 00:00")"
grep -qF "no forever-bots lines in the window" <<< "$EMPTY" || { echo "FAIL: empty window"; fails=$((fails + 1)); }

if [ "$fails" -ne 0 ]; then
    echo "--- output"
    echo "$OUT"
    exit 1
fi
echo "sim_readout_test: all passed"
