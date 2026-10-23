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
  experiment.sh start NAME [PARTIES=8] [SEED=1] [HEAL=55] [TANK_HEAL=75] [DPS_MANA=40] [LOS_YARDS=6] [SECONDS=2400]
  experiment.sh status NAME
  experiment.sh report NAME
  experiment.sh rank
  experiment.sh propose

NAME must be a simple id (letters, digits, dot, underscore, hyphen).
Runs execute on forever-test only. start restarts the sim; it refuses if the current scenario is unfinished.
Set RUN_ROOT to choose where local logs and metrics are stored (default: /tmp/dm-learning).
EOF
}

valid_int() { [[ "$1" =~ ^[0-9]+$ ]] && (( $1 >= $2 && $1 <= $3 )); }
run_dir() { printf '%s/%s' "$RUN_ROOT" "$1"; }

case "${1:-help}" in
  start)
    NAME=${2:-}
    [[ "$NAME" =~ ^[A-Za-z0-9._-]+$ ]] || { usage >&2; exit 2; }
    PARTIES=${3:-8}; SEED=${4:-1}; HEAL=${5:-55}; TANK=${6:-75}
    DPS=${7:-40}; LOS=${8:-6}; DURATION=${9:-2400}
    valid_int "$PARTIES" 1 8 || { echo "PARTIES must be 1..8 (five bots per party)." >&2; exit 2; }
    valid_int "$SEED" 1 2147483647 || { echo "SEED must be 1..2147483647." >&2; exit 2; }
    valid_int "$HEAL" 30 85 || { echo "HEAL must be 30..85." >&2; exit 2; }
    valid_int "$TANK" 40 95 || { echo "TANK_HEAL must be 40..95." >&2; exit 2; }
    valid_int "$DPS" 10 80 || { echo "DPS_MANA must be 10..80." >&2; exit 2; }
    valid_int "$LOS" 3 12 || { echo "LOS_YARDS must be 3..12." >&2; exit 2; }
    valid_int "$DURATION" 30 86400 || { echo "SECONDS must be 30..86400." >&2; exit 2; }
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
EOF
    printf '%s\n' "$NAME" > "$ACTIVE"
    echo "Starting $NAME: $PARTIES parties, seed=$SEED, heal=$HEAL, tank=$TANK, dps-mana=$DPS, los-yards=$LOS, seconds=$DURATION"
    if RUN_DIR="$DIR" "$SIM_HELPER" start "$PARTIES" none "$DURATION" "$SEED" "$HEAL" "$TANK" "$DPS" "$LOS"; then
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
    RUN_DIR="$DIR" "$SIM_HELPER" report
    python3 -I "$ANALYZER" analyze --summary "$DIR/dm-run.log" --trace "$DIR/dm-run.telegraph.log" --out "$DIR" \
      --candidate "$NAME" --seed "$SEED" --parties "$PARTIES" --heal "$HEAL" --tank-heal "$TANK" --dps-mana "$DPS" --los-yards "$LOS"
    if [[ -f "$ACTIVE" && "$(cat "$ACTIVE")" == "$NAME" ]] && python3 - "$DIR/metrics.json" <<'PY'
import json, sys
metrics = json.load(open(sys.argv[1], encoding="utf-8"))
raise SystemExit(0 if not metrics.get("warnings") else 1)
PY
    then
      rm -f "$ACTIVE"
      echo "Complete candidate stored: $DIR/metrics.json"
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

  help|-h|--help)
    usage
    ;;
  *)
    usage >&2
    exit 2
    ;;
esac
