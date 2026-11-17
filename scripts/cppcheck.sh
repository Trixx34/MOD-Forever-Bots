#!/usr/bin/env bash
# cppcheck for MOD-Forever-Bots (performance, warning, portability). Accepted findings: .cppcheck-baseline.txt, suppressions: .cppcheck-suppressions.
#
#   scripts/cppcheck.sh --diff [BASE]    changed .cpp/.h files against BASE (default origin/test) only
#   scripts/cppcheck.sh --full           all of src/ and tests/
#   scripts/cppcheck.sh --rank           full run, ranked report, never fails
#   scripts/cppcheck.sh --update-baseline
# Exit 0 = nothing beyond the baseline, 1 = new findings, 2 = setup problem. --error-exitcode is used by cppcheck itself for real errors (severity error).
# cppcheck: $CPPCHECK, else PATH, else $TIDY_VENV/bin/cppcheck (venv with `pip install cppcheck`). Options: --jobs N (default 2), --base-include DIR (SDK include dir, optional).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODE=diff BASE=origin/test JOBS=2 UPDATE=0 RANK=0 SDKINC="${FOREVER_MODULE_SDK_SOURCE:+$FOREVER_MODULE_SDK_SOURCE/include}"
while [ $# -gt 0 ]; do
    case "$1" in
        --diff) MODE=diff; if [ $# -gt 1 ] && [[ "$2" != --* ]]; then BASE="$2"; shift; fi ;;
        --full) MODE=full ;;
        --rank) MODE=full RANK=1 ;;
        --update-baseline) UPDATE=1 ;;
        --jobs) JOBS="$2"; shift ;;
        --base-include) SDKINC="$2"; shift ;;
        -h|--help) sed -n '2,9p' "$0"; exit 0 ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
    shift
done
CC="${CPPCHECK:-$(command -v cppcheck || true)}"
[ -n "$CC" ] || { [ -n "${TIDY_VENV:-}" ] && [ -x "$TIDY_VENV/bin/cppcheck" ] && CC="$TIDY_VENV/bin/cppcheck"; } || true
[ -n "$CC" ] || { echo "cppcheck: not found (set CPPCHECK, TIDY_VENV, or install cppcheck)" >&2; exit 2; }
cd "$ROOT"
if [ "$MODE" = full ]; then
    FILES=(src tests)
else
    MB="$(git merge-base "$BASE" HEAD)"
    mapfile -t FILES < <({ git diff --name-only --diff-filter=ACMR "$MB" -- src tests; git ls-files --others --exclude-standard -- src tests; } | grep -E '\.(cpp|h|hpp)$' | sort -u)
    [ "${#FILES[@]}" -gt 0 ] || { echo "cppcheck: no changed C++ files"; exit 0; }
fi
OUT="$(mktemp "${TMPDIR:-/tmp}/cppcheck-out.XXXXXX")"; trap 'rm -f "$OUT"' EXIT
inc=(-I src); [ -z "$SDKINC" ] || inc+=(-I "$SDKINC")
nice -n 19 "$CC" --enable=performance,warning,portability --inline-suppr --suppressions-list=.cppcheck-suppressions \
    --std=c++20 --language=c++ --quiet --error-exitcode=0 -j "$JOBS" "${inc[@]}" \
    --template='{file}:{line}:{column}: {severity}: {message} [{id}]' "${FILES[@]}" 2>"$OUT" || true
B="$ROOT/.cppcheck-baseline.txt"
if [ "$RANK" = 1 ]; then python3 scripts/cppcheck_baseline.py --root "$ROOT" --input "$OUT" --baseline "$B" --rank; exit 0; fi
if [ "$UPDATE" = 1 ]; then
    extra=(); [ "$MODE" = full ] || extra=(--partial)
    python3 scripts/cppcheck_baseline.py --root "$ROOT" --input "$OUT" --baseline "$B" --update "${extra[@]}"; exit 0
fi
python3 scripts/cppcheck_baseline.py --root "$ROOT" --input "$OUT" --baseline "$B"
