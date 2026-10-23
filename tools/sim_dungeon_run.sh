#!/usr/bin/env bash
# The sim helper of the dungeon tactics loop (needs ssh to forever-test, sudo there): sim_dungeon_run.sh start N [tactics] [seconds] | report | leveling | status
set -u
CONF=/srv/forever/sim/conf/worldserver.conf.d/modules.d/zz-dungeon-run.conf
STAMP=${RUN_DIR:-/tmp}/dm-run.start
case "${1:-}" in
  start)
    N=${2:-4}
    TAC=${3:-none}
    DUR=${4:-3600}
    MAXB=$((5*N))
    ssh forever-test "sudo -n sh -c 'cat > $CONF <<EOT
Module.forever-bots.DungeonFile = /srv/forever/sim/modules_runtime/forever-bots/data/dungeons.txt
Module.forever-bots.DungeonEventsFile = /srv/forever/sim/modules_runtime/forever-bots/data/dungeon_events.txt
Module.forever-bots.Dungeon = dm
Module.forever-bots.DungeonTeleport = 1
Module.forever-bots.DungeonReviveOutOfCombat = 1
Module.forever-bots.MaxBots = $MAXB
Module.forever-bots.ScenarioParties = $N
Module.forever-bots.Gathering = 0
Module.forever-bots.DungeonTactics = $TAC
Module.forever-bots.ScenarioDurationSec = $DUR
Modules.Tier2.DungeonTelegraph = 1
EOT
systemctl restart forever-sim'" 
    date '+%F %T' > $STAMP; cat $STAMP;;
  report)
    ssh forever-test "sudo -n journalctl -u forever-sim --since '$(cat $STAMP)' --no-pager -q 2>/dev/null | grep -E 'forever-bots: (scenario|event (died|ghost|revived|focus|interrupt_target|heal)|bot [0-9]+ .* not done)'" > ${RUN_DIR:-/tmp}/dm-run.log
    # Full chronological trace for a dungeon encounter: core cast starts, effective damage/heals,
    # and the module's cast-result/decision events. Keep separate from the compact party summary.
    ssh forever-test "sudo -n journalctl -u forever-sim --since '$(cat $STAMP)' --no-pager -q 2>/dev/null | grep -E 'dungeon_telegraph|forever-bots: event cast_result:|forever-bots: event healer_dps_los'" > ${RUN_DIR:-/tmp}/dm-run.telegraph.log
    echo "telegraph: ${RUN_DIR:-/tmp}/dm-run.telegraph.log ($(wc -l < "${RUN_DIR:-/tmp}/dm-run.telegraph.log") lines)"
    wc -l < ${RUN_DIR:-/tmp}/dm-run.log
    python3 -I $(dirname "$0")/dungeon_report.py ${RUN_DIR:-/tmp}/dm-run.log --names $(dirname "$0")/names-dm.tsv;;
  leveling)
    ssh forever-test "sudo -n sh -c 'rm -f $CONF; systemctl restart forever-sim'"; echo restored;;
  status)
    ssh forever-test "sudo -n journalctl -u forever-sim --since '-3min' --no-pager -q | grep -E 'forever-bots: scenario' | tail -12" | cut -c1-200;;
esac
