#!/usr/bin/env python3
"""Parse cppcheck output (template '{file}:{line}:{column}: {severity}: {message} [{id}]'), rank, compare with the baseline.
Same fingerprint idea as tidy_baseline.py: (id, file, message), no line numbers, counts per fingerprint; only counts above the baseline are NEW."""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tidy_baseline as tb  # noqa: E402

LINE = re.compile(r'^(?P<file>[^:\n]+):(?P<line>\d+):(?P<col>\d+): (?P<sev>\w+): (?P<msg>.*) \[(?P<check>[A-Za-z0-9_.-]+)\]$')
HOT_DIRS = tb.HOT_DIRS


def parse(text, root):
    root = os.path.realpath(root)
    found, first = tb.collections.Counter(), {}
    for raw in text.splitlines():
        m = LINE.match(raw.strip())
        if not m:
            continue
        path = os.path.realpath(m.group('file') if os.path.isabs(m.group('file')) else os.path.join(root, m.group('file')))
        if not path.startswith(root + os.sep):
            continue
        rel = os.path.relpath(path, root)
        key = ('%s:%s' % (m.group('sev'), m.group('check')), rel, m.group('msg'))
        found[key] += 1
        first.setdefault(key, int(m.group('line')))
    return found, first


def klass(key):
    sev, rel = key[0].split(':')[0], key[1]
    if rel.startswith('tests/'):
        return 'tests'
    if key[0].endswith('uninitMemberVarNoCtor'):
        return 'noise'  # aggregates with default member init elsewhere: cppcheck false positive
    if sev in ('performance', 'warning', 'error') and rel.startswith(HOT_DIRS):
        return 'hot-path'
    if sev in ('performance', 'warning', 'error'):
        return 'cold-path'
    return 'hygiene'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', required=True)
    ap.add_argument('--input', required=True)
    ap.add_argument('--baseline', required=True)
    ap.add_argument('--update', action='store_true')
    ap.add_argument('--rank', action='store_true')
    ap.add_argument('--partial', action='store_true')
    a = ap.parse_args()
    found, first = parse(open(a.input, encoding='utf-8', errors='replace').read(), a.root)
    if a.rank:
        tb.klass = klass
        tb.rank(found, first, sys.stdout)
        return 0
    if a.update:
        if a.partial:
            old = tb.load_baseline(a.baseline)
            files = {k[1] for k in found}
            merged = tb.collections.Counter({k: v for k, v in old.items() if k[1] not in files})
            merged.update(found)
            found = merged
        tb.write_baseline(a.baseline, found)
        print('baseline written: %d findings, %d distinct' % (sum(found.values()), len(found)))
        return 0
    new = tb.new_findings(found, tb.load_baseline(a.baseline))
    n_new = sum(n for _, n in new)
    print('cppcheck: %d findings, %d accepted in the baseline, %d NEW' % (sum(found.values()), sum(found.values()) - n_new, n_new))
    for key, n in sorted(new, key=lambda kv: (kv[0][1], kv[0][0])):
        print('NEW  %s:%d  [%s] x%d  %s' % (key[1], first.get(key, 0), key[0], n, key[2][:200]))
    return 1 if new else 0


if __name__ == '__main__':
    sys.exit(main())
