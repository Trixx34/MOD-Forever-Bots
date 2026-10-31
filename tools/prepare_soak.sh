#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Prepare the data files and config for a bots soak run on the sim.
#
#   tools/prepare_soak.sh data        grind spots and world quests, from the world database
#   tools/prepare_soak.sh rotations   data/rotations.txt, from a Server.log with BotAbilities: lines
#   tools/prepare_soak.sh plan        which data files exist, and the start-sim-soak.sh command to use
#   tools/prepare_soak.sh all         data, rotations, plan (rotations stops with a message when there is no dump yet)
#
# Settings come from the environment; nothing secret is kept here.
#   MYSQL_DEFAULTS_FILE  option file with the database login ([client] user/password/host); required for `data`
#   WORLD_DB             world database name (default: world)
#   SERVER_LOG           worldserver log holding the BotAbilities: dump (default: the sim's Server.log)
#   DATA_DIR             where the data files go (default: <module>/data)
set -euo pipefail

MODULE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DATA_DIR="${DATA_DIR:-$MODULE/data}"
WORLD_DB="${WORLD_DB:-world}"
SERVER_LOG="${SERVER_LOG:-/srv/forever/build/sim-logs/Server.log}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

die() { echo "prepare_soak: $*" >&2; exit 1; }

sql() { # sql <query> > file ; tab separated, no header
    [ -n "${MYSQL_DEFAULTS_FILE:-}" ] || die "set MYSQL_DEFAULTS_FILE to an option file with the database login"
    mysql --defaults-extra-file="$MYSQL_DEFAULTS_FILE" -N -B "$WORLD_DB" -e "$1"
}

make_data() {
    mkdir -p "$DATA_DIR" "$WORK/exports"
    echo "== grind spots"
    sql "SELECT c.map, ROUND(c.position_x,1), ROUND(c.position_y,1), ROUND(c.position_z,1), (ct.minlevel + ct.maxlevel) / 2
         FROM creature c JOIN creature_template ct ON ct.entry = c.id
         WHERE ct.npcflag = 0 AND ct.\`rank\` = 0 AND ct.type NOT IN (8, 11, 12, 13) AND c.map IN (0, 1)
           AND ct.minlevel BETWEEN 1 AND 60" > "$WORK/spawns.tsv"
    [ -s "$WORK/spawns.tsv" ] || die "the spawn export is empty (check the column names with SHOW COLUMNS, doc/grind-spots.md)"
    python3 -I "$MODULE/tools/make_grind_spots.py" "$WORK/spawns.tsv" > "$WORK/grind_spots.txt"
    install -m 664 "$WORK/grind_spots.txt" "$DATA_DIR/grind_spots.txt"

    echo "== world quests (the exports can take a few minutes)"
    local e="$WORK/exports"
    sql "SELECT q.ID, c.QuestLevel, q.MinLevel, q.AllowableRaces, a.AllowableClasses, a.PrevQuestID, q.SuggestedGroupNum
         FROM quest_template q LEFT JOIN quest_template_addon a ON a.ID = q.ID
         JOIN quest_template_classic_level c ON c.ID = q.ID
         WHERE c.QuestLevel BETWEEN 1 AND 60 AND q.QuestSortID >= 0" > "$e/quests.tsv"
    sql "SELECT o.QuestID, o.\`Order\`, o.Type, o.ObjectID, o.Amount FROM quest_objectives o
         JOIN quest_template_classic_level c ON c.ID = o.QuestID AND c.QuestLevel BETWEEN 1 AND 60" > "$e/objectives.tsv"
    sql "SELECT s.quest, 'start', s.id, cr.map, cr.position_x, cr.position_y, cr.position_z
         FROM creature_queststarter s JOIN creature cr ON cr.id = s.id
         UNION ALL
         SELECT en.quest, 'end', en.id, cr.map, cr.position_x, cr.position_y, cr.position_z
         FROM creature_questender en JOIN creature cr ON cr.id = en.id" > "$e/npcs.tsv"
    sql "SELECT o.QuestID, cr.id, cr.map, cr.position_x, cr.position_y, cr.position_z
         FROM quest_objectives o JOIN creature cr ON cr.id = o.ObjectID WHERE o.Type = 0
         UNION ALL
         SELECT o.QuestID, cr.id, cr.map, cr.position_x, cr.position_y, cr.position_z
         FROM quest_objectives o JOIN creature_loot_template lt ON lt.Item = o.ObjectID
         JOIN creature cr ON cr.id = lt.Entry WHERE o.Type = 1" > "$e/targets.tsv"
    sql "SELECT o.ObjectID, lt.Entry FROM quest_objectives o JOIN creature_loot_template lt ON lt.Item = o.ObjectID WHERE o.Type = 1" > "$e/drops.tsv"
    for f in quests objectives npcs targets; do [ -s "$e/$f.tsv" ] || die "$f.tsv is empty (check the column names, doc/world-quests.md)"; done
    python3 -I "$MODULE/tools/make_world_quests.py" "$e" --explain "$WORK/skipped.tsv" > "$WORK/world_quests.txt"
    [ -s "$WORK/world_quests.txt" ] || die "no quests were kept"
    install -m 664 "$WORK/world_quests.txt" "$DATA_DIR/world_quests.txt"
    install -m 664 "$WORK/skipped.tsv" "$DATA_DIR/world_quests_skipped.tsv"
    echo "wrote $DATA_DIR/grind_spots.txt and $DATA_DIR/world_quests.txt"
}

make_rotations() {
    [ -r "$SERVER_LOG" ] || die "cannot read $SERVER_LOG (set SERVER_LOG)"
    if ! grep -q 'BotAbilities:' "$SERVER_LOG"; then
        cat >&2 <<EOF
prepare_soak: no 'BotAbilities:' lines in $SERVER_LOG. Make the dump first:
  1. set Bot.Dump.Abilities = 1 in the sim core's worldserver.conf and restart the sim
  2. let the module spawn at least one bot of each class (any race)
  3. run this again with SERVER_LOG pointing at the log, then set Bot.Dump.Abilities back to 0
EOF
        exit 2
    fi
    python3 -I "$MODULE/tools/make_rotations.py" --dump "$SERVER_LOG" --spec "$MODULE/data/rotation_spec.txt" > "$WORK/rotations.txt" 2> "$WORK/warnings.txt" || { cat "$WORK/warnings.txt" >&2; die "make_rotations failed"; }
    [ -s "$WORK/rotations.txt" ] || die "rotations.txt is empty"
    install -m 664 "$WORK/rotations.txt" "$DATA_DIR/rotations.txt"
    echo "wrote $DATA_DIR/rotations.txt; spells the dump did not have (dropped):"
    cat "$WORK/warnings.txt"
    local forever
    forever=$(grep -c -E 'Holy Strike|Divine Storm|Chaos Bolt|Saber Slash|Lava Burst|Starsurge' "$DATA_DIR/rotations.txt" || true)
    echo "rows mentioning a sample of Forever abilities: $forever"
}

make_config() {
    # start-sim-soak.sh writes forever-bots.conf itself; it takes world_quests.txt and rotations.txt from --data-dir when they exist.
    local f
    for f in world_quests.txt grind_spots.txt rotations.txt; do
        [ -s "$DATA_DIR/$f" ] && echo "have    $DATA_DIR/$f" || echo "missing $DATA_DIR/$f"
    done
    echo
    echo "Start the run with (as root; the patched script is Trinity-Forever-Modules/serverScripts/start-sim-soak.sh):"
    echo "  sudo bash start-sim-soak.sh --bots 300 --minutes 480 --data-dir $DATA_DIR"
    echo "The sim runs the binary in /srv/forever/sim/modules_runtime/forever-bots: it must be rebuilt from this tree first."
}

case "${1:-}" in
    data) make_data ;;
    rotations) make_rotations ;;
    plan) make_config ;;
    all) make_data; make_rotations; make_config ;;
    *) sed -n 2,16p "$0"; exit 1 ;;
esac
