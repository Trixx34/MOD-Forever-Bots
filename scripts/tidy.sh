#!/usr/bin/env bash
# Performance clang-tidy for MOD-Forever-Bots (config: .clang-tidy, accepted findings: .clang-tidy-baseline.txt, docs: docs/static-checks.md).
#
#   scripts/tidy.sh --diff [BASE]     only the .cpp files changed against BASE (default origin/test) + the .cpp files that include a changed header
#   scripts/tidy.sh --full            every .cpp of src/ and tests/
#   scripts/tidy.sh --rank            full run, print the ranked report, never fail
#   scripts/tidy.sh --update-baseline accept the current findings (full run; with --diff: merge for the checked files)
# Exit 0 = no finding beyond the baseline, 1 = new findings, 2 = setup problem (no clang-tidy, no compile_commands.json).
# Options: --build-dir DIR (default $TIDY_BUILD_DIR or ./build-tidy)  --jobs N (default 2)  --sdk DIR (SDK checkout, default $FOREVER_MODULE_SDK_SOURCE)
#          --save-output FILE (raw clang-tidy output)  --configure (create the build dir: Ninja, Debug, compile_commands.json, builds only the generated protobuf headers)
# clang-tidy: $CLANG_TIDY, else clang-tidy on PATH, else $TIDY_VENV/bin/clang-tidy (a venv with `pip install clang-tidy`).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODE=diff BASE=origin/test BUILD="${TIDY_BUILD_DIR:-$ROOT/build-tidy}" JOBS=2 UPDATE=0 RANK=0 CONFIGURE=0 SDK="${FOREVER_MODULE_SDK_SOURCE:-}" SAVE=""
while [ $# -gt 0 ]; do
    case "$1" in
        --diff) MODE=diff; if [ $# -gt 1 ] && [[ "$2" != --* ]]; then BASE="$2"; shift; fi ;;
        --full) MODE=full ;;
        --rank) MODE=full RANK=1 ;;
        --update-baseline) UPDATE=1 ;;
        --build-dir) BUILD="$2"; shift ;;
        --jobs) JOBS="$2"; shift ;;
        --sdk) SDK="$2"; shift ;;
        --configure) CONFIGURE=1 ;;
        --save-output) SAVE="$2"; shift ;;
        -h|--help) sed -n '2,13p' "$0"; exit 0 ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
    shift
done

TIDY="${CLANG_TIDY:-}"
[ -n "$TIDY" ] || TIDY="$(command -v clang-tidy || true)"
[ -n "$TIDY" ] || { [ -n "${TIDY_VENV:-}" ] && [ -x "$TIDY_VENV/bin/clang-tidy" ] && TIDY="$TIDY_VENV/bin/clang-tidy"; } || true
[ -n "$TIDY" ] || { echo "tidy: clang-tidy not found (set CLANG_TIDY, or TIDY_VENV to a venv with 'pip install clang-tidy', or install the clang-tidy package)" >&2; exit 2; }

if [ "$CONFIGURE" = 1 ] || [ ! -f "$BUILD/compile_commands.json" ]; then
    [ "$CONFIGURE" = 1 ] || [ -n "$SDK" ] || { echo "tidy: $BUILD/compile_commands.json is missing; run with --configure (and --sdk DIR)" >&2; exit 2; }
    args=(-S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON)
    [ -z "$SDK" ] || args+=("-DFOREVER_MODULE_SDK_SOURCE=$SDK")
    cmake "${args[@]}" >/dev/null
    # the sources include the generated *.pb.h of the SDK: build only those targets (no compile of the bots)
    nice -n 19 ninja -C "$BUILD" -j"$JOBS" $(ninja -C "$BUILD" -t targets all | sed -n 's/^\(.*\.pb\.h\):.*/\1/p') >/dev/null
fi

cd "$ROOT"
# all translation units of this repo from compile_commands.json (SDK units and generated files excluded)
mapfile -t ALL < <(python3 - "$BUILD/compile_commands.json" "$ROOT" <<'PY'
import json, os, sys
root = os.path.realpath(sys.argv[2])
seen = set()
for e in json.load(open(sys.argv[1])):
    f = os.path.realpath(os.path.join(e['directory'], e['file']))
    rel = os.path.relpath(f, root)
    if rel.startswith(('src/', 'tests/')) and f.endswith('.cpp') and rel not in seen:
        seen.add(rel); print(rel)
PY
)
FILES=()
if [ "$MODE" = full ]; then
    FILES=("${ALL[@]}")
else
    MB="$(git merge-base "$BASE" HEAD)"
    mapfile -t CHANGED < <(git diff --name-only --diff-filter=ACMR "$MB" -- src tests; git ls-files --others --exclude-standard -- src tests)
    declare -A want=()
    for f in "${CHANGED[@]}"; do
        case "$f" in
            *.cpp) want["$f"]=1 ;;
            *.h|*.hpp) # a changed header: tidy the units that include it
                while IFS= read -r u; do want["$u"]=1; done < <(grep -l -E "#include \"([^\"]*/)?$(basename "$f")\"" "${ALL[@]}" 2>/dev/null || true) ;;
        esac
    done
    for f in "${ALL[@]}"; do [ -z "${want[$f]:-}" ] || FILES+=("$f"); done
fi
if [ "${#FILES[@]}" = 0 ]; then echo "tidy: no C++ files to check ($MODE mode)"; exit 0; fi
echo "tidy: $MODE mode, ${#FILES[@]} files, $("$TIDY" --version | sed -n 's/.*LLVM version \(.*\)/clang-tidy \1/p'), jobs $JOBS" >&2

# a clang-tidy without a matching gcc/libstdc++ (pip wheel, or compile_commands from g++) cannot find <cstdint>: hand it the system C++ include dirs of the compiler
SYS=()
while IFS= read -r d; do SYS+=("--extra-arg=-isystem$d"); done < <(echo | "${CXX:-c++}" -E -x c++ - -v 2>&1 | sed -n '/^#include <...> search starts here:/,/^End of search list/p' | grep '^ /' | sed 's/^ //')
OUT="$(mktemp "${TMPDIR:-/tmp}/tidy-out.XXXXXX")"; trap 'rm -f "$OUT"' EXIT
# one clang-tidy per file, -j in parallel, low priority; a crash of one file does not stop the run (its stderr goes to the output file)
printf '%s\n' "${FILES[@]}" | xargs -P "$JOBS" -n 1 nice -n 19 "$TIDY" -p "$BUILD" --quiet --config-file="$ROOT/.clang-tidy" "${SYS[@]}" >"$OUT" 2>&1 || true

[ -z "$SAVE" ] || cp "$OUT" "$SAVE"
BASELINE="$ROOT/.clang-tidy-baseline.txt"
if [ "$RANK" = 1 ]; then python3 "$ROOT/scripts/tidy_baseline.py" --root "$ROOT" --input "$OUT" --baseline "$BASELINE" --rank; exit 0; fi
if [ "$UPDATE" = 1 ]; then
    extra=(); [ "$MODE" = full ] || extra=(--partial)
    python3 "$ROOT/scripts/tidy_baseline.py" --root "$ROOT" --input "$OUT" --baseline "$BASELINE" --update "${extra[@]}"; exit 0
fi
# in diff mode only the checked files can be compared: restrict the baseline to them via --partial semantics (unchecked files give no findings, never "new")
python3 "$ROOT/scripts/tidy_baseline.py" --root "$ROOT" --input "$OUT" --baseline "$BASELINE"
