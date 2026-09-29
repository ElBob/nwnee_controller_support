#!/usr/bin/env bash
# M5 acceptance checks on the test box (plan §9):
#   1. the release build runs in the game with every signature resolved;
#   2. with every signature deliberately broken, the game still runs normally:
#      all features off, each miss logged, and stick input doesn't crash it.
# Usage (from the repo on the box): tools/m5_checks.sh
set -euo pipefail
. "$(dirname "$0")/common.sh"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TOOLS="$ROOT/tools"
session_env
MODULE=(+TestNewModule "Contest Of Champions 0492")
fail() { echo "FAIL: $*"; exit 1; }

echo "== 1. release build"
cmake -S "$ROOT" -B /tmp/nwpad-release -DCMAKE_BUILD_TYPE=Release -DNWPAD_DEBUG_SURFACES=OFF >/dev/null
cmake --build /tmp/nwpad-release -j >/dev/null
RUN=$("$TOOLS/run_game.sh" --timeout 90 --ready log --lib /tmp/nwpad-release/libnwpad.so -- "${MODULE[@]}") || fail "release launch: $RUN"
sleep 15   # into the module
PID=$(cat "$RUN/pid")
kill -0 "$PID" || fail "release build: game exited"
LOG="$RUN/game.log"
grep -qa '\[nwpad\] signatures: 10/10 resolved' "$LOG" || fail "release build: signatures didn't all resolve"
grep -qa 'camera backend: available, movement backend: available' "$LOG" || fail "release build: features not available"
[ -S "$XDG_RUNTIME_DIR/nwpad.sock" ] && fail "release build has a control socket"
grep -a '\[nwpad\]' "$LOG" | sed 's/^/  /'
stop_pid "$PID"
echo "ok: release build"

echo "== 2. every signature broken"
cmake -S "$ROOT" -B /tmp/nwpad-broken -DNWPAD_SIGNATURES="$ROOT/tests/live/broken_signatures.yaml" >/dev/null
cmake --build /tmp/nwpad-broken -j >/dev/null
RUN=$("$TOOLS/run_game.sh" --timeout 90 --lib /tmp/nwpad-broken/libnwpad.so -- "${MODULE[@]}") || fail "broken launch: $RUN"
sleep 15
PID=$(cat "$RUN/pid")
LOG="$RUN/game.log"
total=$(grep -c '^  [a-z_]*:' "$ROOT/tests/live/broken_signatures.yaml")
grep -qa "\[nwpad\] signatures: 0/$total resolved" "$LOG" || fail "expected 0/$total resolved"
[ "$(grep -ca '\[nwpad\] signature .*not found' "$LOG")" = "$total" ] || fail "not every miss was logged"
grep -qa 'camera backend: unavailable, movement backend: unavailable' "$LOG" || fail "features should be off"
"$TOOLS/nwpadctl" stick lx=1 ly=1 rx=1 ry=1 >/dev/null
f0=$("$TOOLS/nwpadctl" ping | python3 -c 'import json,sys; print(json.load(sys.stdin)["frame"])')
sleep 5
"$TOOLS/nwpadctl" release >/dev/null
f1=$("$TOOLS/nwpadctl" ping | python3 -c 'import json,sys; print(json.load(sys.stdin)["frame"])')
kill -0 "$PID" || fail "game exited with broken signatures"
[ "$f1" -gt "$f0" ] || fail "frames stopped advancing"
echo "  frames advanced $f0 -> $f1 with full stick input and no signatures"
grep -a '\[nwpad\]' "$LOG" | head -4 | sed 's/^/  /'
stop_pid "$PID"
echo "ok: game runs normally with every signature broken"
