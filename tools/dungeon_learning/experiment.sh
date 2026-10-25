#!/usr/bin/env bash
# Self-service test-sim experiment runner. It never deploys to production.
set -euo pipefail

HERE="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
BOT_ROOT="$(cd "$HERE/../.." && pwd)"
SIM_HELPER="$BOT_ROOT/tools/sim_dungeon_run.sh"
ANALYZER="$HERE/analyze.py"
RUN_ROOT="${RUN_ROOT:-/tmp/dm-learning}"
ACTIVE="$RUN_ROOT/.active"
mkdir -p "$RUN_ROOT"

usage() {
  cat <<'EOF'
Usage:
  experiment.sh start NAME [PARTIES=8] [SEED=1] [HEAL=55] [TANK_HEAL=75] [DPS_MANA=40] [LOS_YARDS=6] [SECONDS=2400] [TACTICS=none]
  experiment.sh status NAME
  experiment.sh report NAME
  experiment.sh timeline NAME PARTY [--results] [--gap SECONDS]
  experiment.sh rank
  experiment.sh propose

NAME must be a simple id (letters, digits, dot, underscore, hyphen).
TACTICS is a comma list of focus, interrupt, heal, rest, or all / none (default: none, same as the module's own default).
  focus:     the damage dealers take the pack's kill_priority unit first.
  interrupt: a paladin, rogue, shaman or mage interrupts a casting unit in the tank's pack.
  heal/rest: the dungeon-specific heal and pull-pacing thresholds (DungeonHealBelowPct etc.) kick in.
Runs execute on forever-test only. start restarts the sim; it refuses if the current scenario is unfinished.
Set RUN_ROOT to choose where local logs and metrics are stored (default: /tmp/dm-learning).
EOF
}

valid_int() { [[ "$1" =~ ^[0-9]+$ ]] && (( $1 >= $2 && $1 <= $3 )); }
valid_tactics() { [[ "$1" =~ ^(none|all|(focus|interrupt|heal|rest)(,(focus|interrupt|heal|rest))*)$ ]]; }
run_dir() { printf '%s/%s' "$RUN_ROOT" "$1"; }

case "${1:-help}" in
  start)
    NAME=${2:-}
    [[ "$NAME" =~ ^[A-Za-z0-9._-]+$ ]] || { usage >&2; exit 2; }
    PARTIES=${3:-8}; SEED=${4:-1}; HEAL=${5:-55}; TANK=${6:-75}
    DPS=${7:-40}; LOS=${8:-6}; DURATION=${9:-2400}; TACTICS=${10:-none}
    valid_int "$PARTIES" 1 8 || { echo "PARTIES must be 1..8 (five bots per party)." >&2; exit 2; }
    valid_int "$SEED" 1 2147483647 || { echo "SEED must be 1..2147483647." >&2; exit 2; }
    valid_int "$HEAL" 30 85 || { echo "HEAL must be 30..85." >&2; exit 2; }
    valid_int "$TANK" 40 95 || { echo "TANK_HEAL must be 40..95." >&2; exit 2; }
    valid_int "$DPS" 10 80 || { echo "DPS_MANA must be 10..80." >&2; exit 2; }
    valid_int "$LOS" 3 12 || { echo "LOS_YARDS must be 3..12." >&2; exit 2; }
    valid_int "$DURATION" 30 86400 || { echo "SECONDS must be 30..86400." >&2; exit 2; }
    valid_tactics "$TACTICS" || { echo "TACTICS must be 'none', 'all', or a comma list of focus,interrupt,heal,rest." >&2; exit 2; }
    exec 9>"$RUN_ROOT/.launch.lock"
    flock -n 9 || { echo "Another local experiment command is launching; retry shortly." >&2; exit 3; }
    if [[ -f "$ACTIVE" ]]; then
      echo "An experiment is still registered: $(cat "$ACTIVE"). Run status/report for it before starting another." >&2
      exit 3
    fi
    DIR=$(run_dir "$NAME")
    [[ ! -e "$DIR" ]] || { echo "Run directory already exists: $DIR (choose a new NAME)." >&2; exit 3; }
    mkdir "$DIR"
    cat > "$DIR/candidate.env" <<EOF
NAME=$NAME
PARTIES=$PARTIES
SEED=$SEED
HEAL=$HEAL
TANK=$TANK
DPS=$DPS
LOS=$LOS
DURATION=$DURATION
TACTICS=$TACTICS
EOF
    printf '%s\n' "$NAME" > "$ACTIVE"
    echo "Starting $NAME: $PARTIES parties, seed=$SEED, heal=$HEAL, tank=$TANK, dps-mana=$DPS, los-yards=$LOS, seconds=$DURATION, tactics=$TACTICS"
    if RUN_DIR="$DIR" "$SIM_HELPER" start "$PARTIES" "$TACTICS" "$DURATION" "$SEED" "$HEAL" "$TANK" "$DPS" "$LOS"; then
      :
    else
      result=$?
      if (( result == 3 )); then
        rm -f "$ACTIVE" "$DIR/candidate.env"
        rmdir "$DIR" 2>/dev/null || true
      else
        echo "Start failed. Inspect $DIR; keep the active marker until confirming no sim run is active." >&2
      fi
      exit "$result"
    fi
    echo "Run directory: $DIR"
    echo "Check progress: $0 status $NAME"
    echo "When finished: $0 report $NAME"
    ;;

  status)
    NAME=${2:-}
    [[ "$NAME" =~ ^[A-Za-z0-9._-]+$ ]] || { usage >&2; exit 2; }
    DIR=$(run_dir "$NAME")
    [[ -f "$DIR/dm-run.start" ]] || { echo "No run stamp found for $NAME in $DIR" >&2; exit 2; }
    RUN_DIR="$DIR" "$SIM_HELPER" status
    ;;

  report)
    NAME=${2:-}
    [[ "$NAME" =~ ^[A-Za-z0-9._-]+$ ]] || { usage >&2; exit 2; }
    DIR=$(run_dir "$NAME")
    [[ -f "$DIR/candidate.env" && -f "$DIR/dm-run.start" ]] || { echo "No complete run setup found for $NAME in $DIR" >&2; exit 2; }
    # This file is written by this script and contains only numeric parameters and the validated run name.
    # shellcheck disable=SC1090
    source "$DIR/candidate.env"
    TACTICS=${TACTICS:-none}   # older run directories were written before TACTICS existed
    # Freeze the capture to the configured run window. The sim starts the next scenario
    # automatically, so an unbounded report would keep appending later parties to these metrics.
    RUN_UNTIL=$(date -d "$(cat "$DIR/dm-run.start") + $((DURATION + 300)) seconds" '+%F %T')
    RUN_UNTIL="$RUN_UNTIL" RUN_DIR="$DIR" "$SIM_HELPER" report
    python3 -I "$ANALYZER" analyze --summary "$DIR/dm-run.log" --trace "$DIR/dm-run.telegraph.log" --out "$DIR" \
      --candidate "$NAME" --seed "$SEED" --parties "$PARTIES" --heal "$HEAL" --tank-heal "$TANK" --dps-mana "$DPS" --los-yards "$LOS" --tactics "$TACTICS"
    if python3 - "$DIR/metrics.json" <<'PY'
import json, sys
metrics = json.load(open(sys.argv[1], encoding="utf-8"))
raise SystemExit(0 if not metrics.get("warnings") else 1)
PY
    then
      if [[ -f "$ACTIVE" && "$(cat "$ACTIVE")" == "$NAME" ]]; then
        rm -f "$ACTIVE"
      fi
      echo "Complete candidate stored: $DIR/metrics.json (capture ended $RUN_UNTIL)"
    else
      echo "Candidate is incomplete; keep the active marker and use status/report again after all party outcomes arrive."
    fi
    ;;

  rank|propose)
    shopt -s nullglob
    METRICS=("$RUN_ROOT"/*/metrics.json)
    ((${#METRICS[@]})) || { echo "No analyzed candidates under $RUN_ROOT" >&2; exit 2; }
    python3 -I "$ANALYZER" "$1" "${METRICS[@]}"
    ;;

  timeline)
    # Ordered, block-per-pull fight trace for one party of a completed report.
    NAME=${2:-}; PARTY=${3:-}
    [[ "$NAME" =~ ^[A-Za-z0-9._-]+$ ]] || { usage >&2; exit 2; }
    valid_int "$PARTY" 1 8 || { echo "PARTY must be 1..8 (five bots per party)." >&2; exit 2; }
    DIR=$(run_dir "$NAME")
    [[ -f "$DIR/dm-run.telegraph.log" ]] || { echo "No telegraph capture for $NAME in $DIR; run report first." >&2; exit 2; }
    shift 3
    python3 -I "$ANALYZER" timeline --trace "$DIR/dm-run.telegraph.log" --summary "$DIR/dm-run.log" \
      --party "$PARTY" --out "$DIR/party$PARTY.timeline.txt" "$@"
    ;;

  help|-h|--help)
    usage
    ;;
  *)
    usage >&2
    exit 2
    ;;
esac
