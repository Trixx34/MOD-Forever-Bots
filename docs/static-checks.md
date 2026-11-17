# Static performance checks (clang-tidy and cppcheck)

Both tools look for what costs time in the bot pump (about 800 bots per tick on prod): copies in loops, by-value parameters, needless copies, repeated lookups. They are not a style check.

| | clang-tidy | cppcheck |
|---|---|---|
| config | `.clang-tidy` | `.cppcheck-suppressions` + flags in `scripts/cppcheck.sh` |
| script | `scripts/tidy.sh` | `scripts/cppcheck.sh` |
| accepted findings | `.clang-tidy-baseline.txt` | `.cppcheck-baseline.txt` |
| compares with baseline | `scripts/tidy_baseline.py` | `scripts/cppcheck_baseline.py` |

Baseline rule: a finding is (check, file, message), counted, without the line number. A run fails (exit 1) only when a count is above the baseline, so editing a file never turns accepted findings into new ones, and fixing one only lowers the count. After a clean-up run `--update-baseline`, review the diff of the baseline file in the commit.

## clang-tidy
```
scripts/tidy.sh --configure --sdk /path/to/forever-module-sdk --build-dir build-tidy --diff      # first time: Ninja + compile_commands.json + the generated protobuf headers only
scripts/tidy.sh --diff [origin/test]     # changed .cpp files, plus the .cpp files that include a changed header
scripts/tidy.sh --full                   # all 230+ files, about 6 minutes with -j2
scripts/tidy.sh --rank                   # ranked report, never fails
scripts/tidy.sh --update-baseline        # accept the current state
```
`CLANG_TIDY=/path` or `TIDY_VENV=/path/to/venv` (a venv with `pip install clang-tidy`, works without root). The script hands the compiler's system include dirs to clang-tidy (`-isystem`), because a pip clang-tidy has no libstdc++ of its own. Low priority (`nice -n 19`), `--jobs 2` by default.

Enabled: `performance-*` (minus enum-size, no-int-to-ptr, avoid-endl), a few `bugprone-` (use-after-move, move-forwarding-reference, unused-return-value, string-constructor, inc-dec-in-conditions), `readability-container-contains/size-empty/data-pointer`, `readability-use-anyofallof`, `modernize-use-emplace/pass-by-value/loop-convert`. Everything else is off on purpose (noise).

## cppcheck
```
scripts/cppcheck.sh --diff | --full | --rank | --update-baseline        # --base-include <sdk>/include, CPPCHECK=/path or TIDY_VENV=venv
```
`--enable=performance,warning,portability --inline-suppr`; use `// cppcheck-suppress <id>` for a single false positive, `.cppcheck-suppressions` for a class of them. cppcheck's `error:*Lifetime` findings in this code base are false positives on pointers into a const-ref parameter container (see the ranked report in the commit message).

CI proposal: `jenkins/perf-checks.proposed.Jenkinsfile` in forever-ops (branch `feature/perf-checks-20261117`).
