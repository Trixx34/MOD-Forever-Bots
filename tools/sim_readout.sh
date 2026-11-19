#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# sim_readout.sh: the read-out of a sim run, per arm (ArmSplit, doc/sim-ab.md) and per hour, from the module's journal lines and the characters table.
# Read-only: it reads the journal and runs SELECTs; it writes only its own temp files (removed at the end).
#
# On the sim, as root (nothing is copied there):
#   ssh forever-test 'sudo -n bash -s -- "2026-11-19 12:28" "2026-11-19 20:30"' < tools/sim_readout.sh
# Arguments: START (journalctl --since, e.g. "2026-11-19 12:28:00"), END (optional, --until).
# Environment (all optional):
#   READOUT_UNIT   the systemd unit (default forever-sim)
#   READOUT_DB     the characters database (default forever_test_characters)
#   READOUT_LOG    read this file instead of the journal (lines "2026-11-19T12:32:31+0100 host unit[pid]: forever-bots: ..." or the classic
#                  "Nov 19 12:32:31 host unit[pid]: ..."); START/END still cut it
#   READOUT_CHARS  read "guid level class online" rows (tab or space separated) from this file instead of the database
#   READOUT_NO_HOST=1  skip the RAM/disk section (tests)
# Sections: window; per arm and hour: died, bot-hours, deaths per bot-hour, levels gained; deaths by map, by level band and top killers; per-bot spread
# (median, top-10 share); the deaths_top lines; repeat deaths at one 100-yard cell; level-safe / death-loop / below-map counts; no_route per bot-hour;
# levels now by band and class; errors; pump decide ms; RAM/disk.
# The arm of a line is its arm= field; without one, an odd guid is arm B when the run has arm fields at all, else every line is arm "all".
# Bot-hours come from the pump_arm lines (per arm, one a minute) or, without them, from the pump lines (arm "all").
# Levels gained: per bot, the last level= seen minus the first (level_up and the other events with a level= field), divided by the bot-hours.
set -u
START="${1:-}"
END="${2:-}"
if [ -z "$START" ]; then
    echo "usage: sim_readout.sh START [END]   (START/END as journalctl --since/--until takes them)" >&2
    exit 2
fi
UNIT="${READOUT_UNIT:-forever-sim}"
DB="${READOUT_DB:-forever_test_characters}"
TMP="$(mktemp -d /tmp/sim-readout.XXXXXX)"
trap 'rm -rf "$TMP"' EXIT

# --- the lines, with an ISO timestamp (YYYY-MM-DDTHH:MM:SS) in front ---------------------------------------------------------------------
norm() {
    # $1 = year for the classic format; reads raw lines, prints "ts<TAB>text after 'forever-bots: '" (other lines are dropped)
    awk -v year="$1" -v start="$2" -v end="$3" '
    BEGIN {
        split("Jan Feb Mar Apr May Jun Jul Aug Sep Oct Nov Dec", m, " ")
        for (i = 1; i <= 12; i++) mon[m[i]] = sprintf("%02d", i)
    }
    {
        p = index($0, "forever-bots: ")
        if (p == 0) next
        if ($1 ~ /^[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]T/) ts = substr($1, 1, 19)
        else if ($1 in mon) ts = sprintf("%s-%s-%02dT%s", year, mon[$1], $2 + 0, $3)
        else next
        if (start != "" && ts < start) next
        if (end != "" && ts > end) next
        print ts "\t" substr($0, p + 14)
    }'
}
iso() {
    # "2026-11-19 12:28" -> "2026-11-19T12:28:00"; empty stays empty
    [ -z "$1" ] && return 0
    local d t
    d="${1%% *}"; t="${1#* }"
    [ "$t" = "$1" ] && t="00:00:00"
    case "$t" in ??:??) t="$t:00" ;; esac
    printf '%sT%s' "$d" "$t"
}
YEAR="${START:0:4}"
case "$YEAR" in [0-9][0-9][0-9][0-9]) ;; *) YEAR="$(date +%Y)" ;; esac
if [ -n "${READOUT_LOG:-}" ]; then
    norm "$YEAR" "$(iso "$START")" "$(iso "$END")" < "$READOUT_LOG" > "$TMP/lines"
else
    if [ -n "$END" ]; then
        journalctl -u "$UNIT" --since "$START" --until "$END" -o short-iso --no-pager 2>/dev/null | norm "$YEAR" "" "" > "$TMP/lines"
    else
        journalctl -u "$UNIT" --since "$START" -o short-iso --no-pager 2>/dev/null | norm "$YEAR" "" "" > "$TMP/lines"
    fi
fi

# --- the characters (guid level class online) ----------------------------------------------------------------------------------------
if [ -n "${READOUT_CHARS:-}" ]; then
    awk 'NF >= 4 { print $1, $2, $3, $4 }' "$READOUT_CHARS" > "$TMP/chars"
else
    mysql -N "$DB" -e "select guid, level, class, online from characters" 2>/dev/null | awk 'NF >= 4 { print $1, $2, $3, $4 }' > "$TMP/chars" || true
fi

echo "== sim read-out: $UNIT from '$START' to '${END:-now}'"
if [ ! -s "$TMP/lines" ]; then
    echo "no forever-bots lines in the window"
    exit 0
fi

awk -F'\t' -v charsfile="$TMP/chars" '
function field(text, key,    t, p, rest, q) {
    t = " " text
    p = index(t, " " key "=")
    if (p == 0) return ""
    rest = substr(t, p + length(key) + 2)
    q = index(rest, " ")
    return q == 0 ? rest : substr(rest, 1, q - 1)
}
function band(l) {
    if (l == "" || l + 0 <= 0) return "?"
    l = l + 0
    if (l <= 5) return "01-05"; if (l <= 10) return "06-10"; if (l <= 15) return "11-15"; if (l <= 20) return "16-20"
    if (l <= 30) return "21-30"; if (l <= 40) return "31-40"; if (l <= 50) return "41-50"; return "51-60"
}
function armof(text, bot,    a) {
    a = field(text, "arm")
    if (a != "") return a
    if (split_seen && bot != "") return (bot % 2 == 1) ? "B" : "A"
    return "all"
}
function evname(text,    p) {
    if (substr(text, 1, 6) != "event ") return ""
    p = index(text, ":")
    return p == 0 ? "" : substr(text, 7, p - 7)
}
# prints the top n keys of arr (key = "arm SEP x") for one arm, "x:count" separated by spaces
function top(arr, arm, n,    k, best, bestk, out, i, used, parts) {
    out = ""
    split("", used)
    for (i = 1; i <= n; i++) {
        best = 0; bestk = ""
        for (k in arr) {
            if (k in used) continue
            split(k, parts, SUBSEP)
            if (parts[1] != arm) continue
            if (arr[k] > best) { best = arr[k]; bestk = k }
        }
        if (bestk == "") break
        used[bestk] = 1
        split(bestk, parts, SUBSEP)
        out = out (out == "" ? "" : " ") parts[2] ":" best
    }
    return out == "" ? "none" : out
}
function fmt(x) { return sprintf("%.2f", x) }
function floor100(x,    q) { q = int(x / 100); if (x < 0 && q * 100 != x) q--; return q * 100 }
BEGIN {
    while ((getline row < charsfile) > 0) {
        split(row, c, " ")
        clevel[c[1]] = c[2]; cclass[c[1]] = c[3]; conline[c[1]] = c[4]
    }
}
# first pass (the file is read twice): is there any arm field at all?
FNR == NR { if (index($2, " arm=") > 0) split_seen = 1; next }
{
    ts = $1; text = $2; hour = substr(ts, 1, 13)
    if (first == "") first = ts
    last = ts
    nlines++
    hours[hour] = 1
    if (text ~ /(^| )(error|fatal|exception|assert)/ || text ~ /[Ee]rror:|FATAL|[Ee]xception|[Aa]ssert/) {
        nerr++
        if (nerr <= 3) errs[nerr] = ts " " substr(text, 1, 200)
    }
    if (substr(text, 1, 6) == "pump: ") {
        n = split(text, w, " ")
        bots = w[2] + 0
        for (i = 1; i < n; i++) if (w[i] == "avg") davg = w[i + 1] + 0; else if (w[i] == "max") dmax = w[i + 1] + 0
        pumpn[hour]++; pumpavg[hour] += davg; if (dmax > pumpmax[hour]) pumpmax[hour] = dmax
        pumpbots[hour] += bots
        allbh[hour] += bots / 60.0
        next
    }
    if (substr(text, 1, 10) == "pump_arm: ") {
        a = field(text, "arm"); arms[a] = 1
        bh[a, hour] += field(text, "bots") / 60.0
        armpb[a] += field(text, "per_bot_us"); armpn[a]++
        have_pump_arm = 1
        next
    }
    if (substr(text, 1, 12) == "deaths_top: ") {
        a = field(text, "arm"); if (a == "") a = "all"
        ndt++; dt[ndt] = ts " arm=" a " deaths=" field(text, "deaths") " online=" field(text, "bots_online") " median=" field(text, "median") " median_died=" field(text, "median_died") " top10_pct=" field(text, "top10_pct")
        next
    }
    ev = evname(text)
    if (ev == "") next
    body = substr(text, 7)
    bot = field(body, "bot")
    a = armof(body, bot)
    arms[a] = 1
    if (bot != "") { active[a, bot] = 1; botarm[bot] = a }
    lv = field(body, "level")
    if (bot != "" && lv != "" && lv + 0 > 0) {
        if (!((a, bot) in firstlv)) firstlv[a, bot] = lv + 0
        lastlv[a, bot] = lv + 0
    }
    if (ev == "died") {
        died[a, hour]++; diedall[a]++
        perbot[a, bot]++
        map = field(body, "map"); bymap[a, map]++
        if (lv == "" && (bot in clevel)) { lv = clevel[bot]; lvsrc = "characters" }
        byband[a, band(lv)]++
        k = field(body, "killer_entry"); if (k == "") k = "none"
        killers[a, k]++
        pos = field(body, "pos"); split(pos, xy, ",")
        cell = map "@" floor100(xy[1]) "," floor100(xy[2])
        cells[a, bot " " cell]++
        next
    }
    if (ev == "no_route") { noroute[a, hour]++; noroutea[a]++; next }
    if (ev == "quest_walk_blocked" || ev ~ /^level_safe/ || ev ~ /^level_unsafe/ || ev ~ /^death_loop/ || ev ~ /^below_map/ || ev == "death_area_escape" || ev == "ghost_fallback_teleport" || ev == "level_up" || ev == "quest_reward") {
        counts[a, ev]++
        if (ev == "quest_walk_blocked") { r = field(body, "reason"); if (r != "") counts[a, ev " reason=" r]++ }
        if (ev == "below_map_recover") { r = field(body, "reason"); if (r != "") counts[a, ev " reason=" r]++ }
    }
}
END {
    print "window: " first " .. " last ", " nlines " lines, arm fields " (split_seen ? "yes" : "no")
    # bot-hours: per arm from pump_arm, else "all" from pump
    if (!have_pump_arm) for (h in allbh) bh["all", h] = allbh[h]
    nh = 0; for (h in hours) hl[++nh] = h
    for (i = 2; i <= nh; i++) { x = hl[i]; j = i - 1; while (j > 0 && hl[j] > x) { hl[j + 1] = hl[j]; j-- } hl[j + 1] = x }
    na = 0; for (a in arms) al[++na] = a
    for (i = 2; i <= na; i++) { x = al[i]; j = i - 1; while (j > 0 && al[j] > x) { al[j + 1] = al[j]; j-- } al[j + 1] = x }
    # an arm with deaths but no bot-hours of its own ("all" lines of a split run): use the pump total split evenly
    for (i = 1; i <= na; i++) { a = al[i]; tb = 0; for (h in hours) tb += bh[a, h]; tbh[a] = tb }

    print ""
    print "== per arm and hour: died, bot-hours, deaths per bot-hour, no_route per bot-hour"
    for (i = 1; i <= na; i++) {
        a = al[i]
        for (j = 1; j <= nh; j++) {
            h = hl[j]; b = bh[a, h]
            if (died[a, h] + 0 == 0 && b == 0 && noroute[a, h] + 0 == 0) continue
            printf "arm=%s hour=%s died=%d bot_hours=%s deaths_per_bot_hour=%s no_route_per_bot_hour=%s\n", a, h, died[a, h], fmt(b), (b > 0 ? fmt(died[a, h] / b) : "n/a"), (b > 0 ? fmt(noroute[a, h] / b) : "n/a")
        }
    }

    print ""
    print "== per arm, whole window"
    for (i = 1; i <= na; i++) {
        a = al[i]; b = tbh[a]
        gained = 0; nb = 0
        for (k in firstlv) { split(k, p, SUBSEP); if (p[1] != a) continue; gained += lastlv[k] - firstlv[k]; nb++ }
        nact = 0; for (k in active) { split(k, p, SUBSEP); if (p[1] == a) nact++ }
        printf "arm=%s died=%d bot_hours=%s deaths_per_bot_hour=%s levels_gained=%d levels_per_bot_hour=%s no_route=%d no_route_per_bot_hour=%s active_bots=%d\n", a, diedall[a], fmt(b), (b > 0 ? fmt(diedall[a] / b) : "n/a"), gained, (b > 0 ? sprintf("%.3f", gained / b) : "n/a"), noroutea[a], (b > 0 ? fmt(noroutea[a] / b) : "n/a"), nact
    }

    print ""
    print "== deaths by map (top 8), by level band, top killers (top 10)"
    for (i = 1; i <= na; i++) {
        a = al[i]
        print "arm=" a " maps: " top(bymap, a, 8)
        print "arm=" a " bands: " top(byband, a, 9)
        print "arm=" a " killers: " top(killers, a, 10)
    }
    if (lvsrc != "") print "(a died line without level=: the band is the level in the characters table now)"

    print ""
    print "== deaths per bot (bots seen in any event count, the ones that did not die as 0): median, top-10 share"
    for (i = 1; i <= na; i++) {
        a = al[i]; n = 0; split("", v)
        for (k in active) { split(k, p, SUBSEP); if (p[1] != a) continue; v[++n] = perbot[a, p[2]] + 0 }
        for (x = 2; x <= n; x++) { y = v[x]; j = x - 1; while (j > 0 && v[j] < y) { v[j + 1] = v[j]; j-- } v[j + 1] = y }
        t10 = 0; for (x = 1; x <= n && x <= 10; x++) t10 += v[x]
        if (n == 0) med = 0; else if (n % 2 == 1) med = v[(n + 1) / 2]; else med = (v[n / 2] + v[n / 2 + 1]) / 2
        printf "arm=%s bots=%d median=%.1f top10=%d top10_pct=%.1f\n", a, n, med, t10, (diedall[a] > 0 ? 100.0 * t10 / diedall[a] : 0)
    }
    print "-- deaths_top lines"
    if (ndt == 0) print "none"
    for (x = 1; x <= ndt; x++) print dt[x]

    print ""
    print "== repeat deaths of one bot in one 100-yard cell (top 10; cells with 5 or more)"
    for (i = 1; i <= na; i++) {
        a = al[i]; many = 0
        for (k in cells) { split(k, p, SUBSEP); if (p[1] == a && cells[k] >= 5) many++ }
        print "arm=" a " cells_5plus=" many " top: " top(cells, a, 10)
    }

    print ""
    print "== level-safe, death-loop, below-map and other counts"
    for (i = 1; i <= na; i++) {
        a = al[i]; out = ""
        for (k in counts) { split(k, p, SUBSEP); if (p[1] == a) out = out "\n  " p[2] "=" counts[k] }
        print "arm=" a (out == "" ? " none" : out)
    }

    print ""
    print "== errors: " nerr + 0
    for (x = 1; x <= nerr && x <= 3; x++) print errs[x]

    print ""
    print "== pump decide per hour (avg of the minute averages, max), ms"
    for (j = 1; j <= nh; j++) {
        h = hl[j]
        if (pumpn[h] + 0 == 0) continue
        printf "hour=%s bots=%d decide_avg_ms=%.2f decide_max_ms=%.2f\n", h, pumpbots[h] / pumpn[h], pumpavg[h] / pumpn[h] / 1000.0, pumpmax[h] / 1000.0
    }
    for (i = 1; i <= na; i++) if (armpn[al[i]] > 0) printf "arm=%s per_bot_turn_us=%.1f\n", al[i], armpb[al[i]] / armpn[al[i]]

    # levels now (characters, online), by band and by class, per arm
    print ""
    print "== levels now (characters online): by band and by class (class:avg_level/bots)"
    for (g in conline) {
        if (conline[g] != 1) continue
        a = (g in botarm) ? botarm[g] : (split_seen ? ((g % 2 == 1) ? "B" : "A") : "all")
        nowband[a, band(clevel[g])]++
        clsum[a, cclass[g]] += clevel[g]; clnum[a, cclass[g]]++
        nowarms[a] = 1; anyonline = 1
    }
    if (!anyonline) print "no characters data"
    for (a in nowarms) {
        bands = ""; split("01-05 06-10 11-15 16-20 21-30 31-40 41-50 51-60", bl, " ")
        for (x = 1; x <= 8; x++) if (nowband[a, bl[x]] > 0) bands = bands " " bl[x] ":" nowband[a, bl[x]]
        cls = ""
        for (x = 1; x <= 12; x++) if (clnum[a, x] > 0) cls = cls " " x ":" sprintf("%.1f", clsum[a, x] / clnum[a, x]) "/" clnum[a, x]
        print "arm=" a " bands:" bands
        print "arm=" a " classes:" cls
    }
}' "$TMP/lines" "$TMP/lines"

if [ "${READOUT_NO_HOST:-0}" != "1" ]; then
    echo ""
    echo "== RAM / disk"
    free -m | sed -n 2p
    df -h / | tail -1
fi
