#!/usr/bin/env python3
"""Parse clang-tidy output, rank findings, compare with the accepted baseline.

Fingerprint = (check, file relative to the repo root, message). Line numbers are NOT part of it, so editing a file does not
turn old accepted findings into new ones; the baseline stores a count per fingerprint and only counts above it are NEW.

  tidy_baseline.py --root DIR --input tidy.txt --baseline .clang-tidy-baseline.txt            # exit 0 = nothing new, 1 = new findings
  tidy_baseline.py ... --update                                                              # rewrite the baseline from this run
  tidy_baseline.py ... --rank                                                                # ranked report only (never fails)
"""
import argparse
import collections
import os
import re
import sys

WARN = re.compile(r'^(?P<file>/[^:\n]+):(?P<line>\d+):(?P<col>\d+): warning: (?P<msg>.*) \[(?P<check>[A-Za-z0-9.,_-]+)\]$')

# checks that point at a real per-tick or per-bot cost in this code base; the rest of performance-* is reported as "other"
HOT_CHECKS = (
    'performance-unnecessary-copy-initialization', 'performance-for-range-copy', 'performance-unnecessary-value-param',
    'performance-inefficient-vector-operation', 'performance-inefficient-string-concatenation',
    'performance-inefficient-algorithm', 'performance-faster-string-find', 'performance-use-std-move',
)
HOT_DIRS = ('src/bot_policy', 'src/main_pump', 'src/scenario', 'src/party_', 'src/heal_plan', 'src/spell_costs',
            'src/combat_rotation', 'src/bot_roster', 'src/main_feeds', 'src/quest_planner')


def parse(text, root):
    root = os.path.realpath(root)
    lines = collections.defaultdict(set)
    first_line = {}
    for raw in text.splitlines():
        m = WARN.match(raw.strip())
        if not m:
            continue
        path = os.path.realpath(m.group('file'))
        if not path.startswith(root + os.sep):
            continue  # SDK, system, build dir
        rel = os.path.relpath(path, root)
        if rel.startswith('build'):
            continue
        for check in m.group('check').split(','):
            key = (check, rel, m.group('msg'))
            # a header finding shows up once per translation unit that includes it: count distinct lines, not reports
            lines[key].add(int(m.group('line')))
            first_line[key] = min(first_line.get(key, 1 << 30), int(m.group('line')))
    return collections.Counter({k: len(v) for k, v in lines.items()}), first_line


def load_baseline(path):
    base = collections.Counter()
    if not os.path.exists(path):
        return base
    for line in open(path, encoding='utf-8'):
        line = line.rstrip('\n')
        if not line or line.startswith('#'):
            continue
        parts = line.split('\t')
        if len(parts) != 4:
            continue
        count, check, rel, msg = parts
        base[(check, rel, msg)] = int(count)
    return base


def write_baseline(path, found):
    with open(path, 'w', encoding='utf-8') as f:
        f.write('# clang-tidy accepted findings: count<TAB>check<TAB>file<TAB>message (no line numbers). Rewrite with scripts/tidy.sh --update-baseline.\n')
        for key in sorted(found, key=lambda k: (k[1], k[0], k[2])):
            f.write('%d\t%s\t%s\t%s\n' % (found[key], key[0], key[1], key[2]))


def new_findings(found, base):
    out = []
    for key, n in found.items():
        extra = n - base.get(key, 0)
        if extra > 0:
            out.append((key, extra))
    return out


def klass(key):
    check, rel, _ = key
    if check in HOT_CHECKS and rel.startswith(HOT_DIRS):
        return 'hot-path'
    if rel.startswith('tests/'):
        return 'tests'
    if check in HOT_CHECKS:
        return 'cold-path'
    if check.startswith('performance-'):
        return 'other-perf'
    return 'hygiene'


def rank(found, first_line, out):
    order = ['hot-path', 'cold-path', 'other-perf', 'hygiene', 'tests']
    groups = collections.defaultdict(list)
    for key, n in found.items():
        groups[klass(key)].append((key, n))
    by_check = collections.Counter()
    for key, n in found.items():
        by_check[key[0]] += n
    out.write('findings: %d in %d distinct (check, file, message)\n' % (sum(found.values()), len(found)))
    out.write('by check:\n')
    for c, n in by_check.most_common():
        out.write('  %5d  %s\n' % (n, c))
    for g in order:
        items = sorted(groups.get(g, []), key=lambda kv: (-kv[1], kv[0][1]))
        out.write('\n== %s: %d findings ==\n' % (g, sum(n for _, n in items)))
        for key, n in items[:40]:
            out.write('  %s:%d  [%s] x%d  %s\n' % (key[1], first_line.get(key, 0), key[0], n, key[2][:150]))
        if len(items) > 40:
            out.write('  ... %d more distinct\n' % (len(items) - 40))


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', required=True)
    ap.add_argument('--input', required=True)
    ap.add_argument('--baseline', required=True)
    ap.add_argument('--update', action='store_true')
    ap.add_argument('--rank', action='store_true')
    ap.add_argument('--partial', action='store_true', help='diff mode: only some files were checked, --update merges instead of replacing')
    a = ap.parse_args(argv)
    found, first_line = parse(open(a.input, encoding='utf-8', errors='replace').read(), a.root)
    if a.rank:
        rank(found, first_line, sys.stdout)
        return 0
    if a.update:
        if a.partial:
            merged = load_baseline(a.baseline)
            files = {k[1] for k in found}
            merged = collections.Counter({k: v for k, v in merged.items() if k[1] not in files})
            merged.update(found)
            found = merged
        write_baseline(a.baseline, found)
        print('baseline written: %d findings, %d distinct' % (sum(found.values()), len(found)))
        return 0
    base = load_baseline(a.baseline)
    new = new_findings(found, base)
    print('clang-tidy: %d findings, %d accepted in the baseline, %d NEW' % (sum(found.values()), sum(found.values()) - sum(n for _, n in new), sum(n for _, n in new)))
    for key, n in sorted(new, key=lambda kv: (kv[0][1], kv[0][0])):
        print('NEW  %s:%d  [%s] x%d  %s' % (key[1], first_line.get(key, 0), key[0], n, key[2][:200]))
    return 1 if new else 0


if __name__ == '__main__':
    sys.exit(main())
