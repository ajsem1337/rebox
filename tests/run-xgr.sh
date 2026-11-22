#!/usr/bin/env bash
# Runs the Gaming Runtime stand-in's tests (shims/xgameruntime, from americas-recomp) under Wine:
#
#   tests/run-xgr.sh [TEST...]
#
# Builds build/xgameruntime.dll and build/xgr/test_*.exe (needs XODUS_IDL: the flake's dev shell
# sets it), then runs each test in a scratch directory next to the DLL and table.json, as upstream's
# run_tests.py does on Windows. test_save also runs a second time with "reopen", which checks that
# the saves survived the first process. Prints PASS/FAIL lines and a summary; exits non-zero if any
# test failed.
set -uo pipefail

here=$(cd "$(dirname "$0")/.." && pwd)
: "${WINE:?run from the dev shell}" "${XODUS_IDL:?run from the dev shell (XODUS_IDL)}"
make -C "$here" -s xgr-tests >/dev/null || { echo "FAIL building xgameruntime.dll or its tests"; exit 1; }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
export WINEPREFIX="$work/prefix" WINEARCH=win64 WINEDEBUG=${WINEDEBUG:--all}
"$WINE" wineboot -i >/dev/null 2>&1
run="$work/run"
mkdir -p "$run"
cp "$here/build/xgameruntime.dll" "$here/build/xgr/table.json" "$here"/build/xgr/test_*.exe "$run/"
export WINEDLLOVERRIDES="xgameruntime=n"

tests=("$@")
[ ${#tests[@]} -gt 0 ] || tests=($(cd "$run" && ls test_*.exe | sed 's/\.exe$//'))
failed=()
for t in "${tests[@]}"; do
    for args in "" reopen; do
        [ -n "$args" ] && [ "$t" != test_save ] && continue
        out=$(cd "$run" && timeout 300 "$WINE" "$t.exe" $args 2>&1)
        code=$?
        fails=$(grep -c '^FAIL' <<<"$out")
        passes=$(grep -c '^PASS' <<<"$out")
        label="$t${args:+ $args}"
        if [ $code -eq 0 ] && [ "$fails" -eq 0 ]; then
            echo "ok   $label: $passes checks"
        else
            echo "FAIL $label: exit $code, $fails failed of $((passes + fails))"
            grep '^FAIL' <<<"$out" | sed 's/^/     /' | head -20
            failed+=("$label")
        fi
    done
done
if [ ${#failed[@]} -eq 0 ]; then
    echo "PASSED xgameruntime stand-in: ${#tests[@]} tests"
else
    echo "FAILED xgameruntime stand-in: ${failed[*]}"
    exit 1
fi
