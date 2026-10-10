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
PID=
fail() { echo "FAIL: $*"; exit 1; }
trap 'if [ -n "$PID" ]; then stop_pid "$PID"; fi' EXIT   # the game this script started, however it ends
# Signature counts from signatures/ee.yaml: all entries, and those a release build has.
read -r ALL RELEASE < <(python3 - "$ROOT/signatures/ee.yaml" <<'PY'
import yaml, sys
sigs = yaml.safe_load(open(sys.argv[1]))["signatures"]
print(len(sigs), sum(not v.get("debug") for v in sigs.values()))
PY
)

echo "== 1. release build"
cmake -S "$ROOT" -B /tmp/nwpad-release -DCMAKE_BUILD_TYPE=Release -DNWPAD_DEBUG_SURFACES=OFF >/dev/null
cmake --build /tmp/nwpad-release -j >/dev/null
RUN=$("$TOOLS/run_game.sh" --timeout 90 --ready log --lib /tmp/nwpad-release/libnwpad.so -- "${MODULE[@]}") || fail "release launch: $RUN"
sleep 15   # into the module
PID=$(cat "$RUN/pid")
kill -0 "$PID" || fail "release build: game exited"
LOG="$RUN/game.log"
grep -qa "\[nwpad\] signatures: $RELEASE/$RELEASE resolved" "$LOG" || fail "release build: signatures didn't all resolve"
grep -qa 'camera backend: available, movement backend: available' "$LOG" || fail "release build: features not available"
[ -S "$XDG_RUNTIME_DIR/nwpad.sock" ] && fail "release build has a control socket"
grep -a '\[nwpad\]' "$LOG" | sed 's/^/  /'
stop_pid "$PID"; PID=
echo "ok: release build"

echo "== 2. every signature broken"
# Every entry of ee.yaml, deliberately broken: symbols that don't exist, patterns
# that can't match.
BROKEN=/tmp/nwpad-broken-signatures.yaml
python3 - "$ROOT/signatures/ee.yaml" "$BROKEN" <<'PY'
import re, sys
text = open(sys.argv[1]).read()
text = re.sub(r"(\n    symbol: )(\S+)", lambda m: m.group(1) + "_ZN6nwpad6broken" + m.group(2), text)
text = re.sub(r"(\n    pattern: )\"[^\"]*\"", r'\1"de ad be ef de ad be ef de ad be ef"', text)
open(sys.argv[2], "w").write(text)
PY
cmake -S "$ROOT" -B /tmp/nwpad-broken -DNWPAD_SIGNATURES="$BROKEN" >/dev/null
cmake --build /tmp/nwpad-broken -j >/dev/null
RUN=$("$TOOLS/run_game.sh" --timeout 90 --lib /tmp/nwpad-broken/libnwpad.so -- "${MODULE[@]}") || fail "broken launch: $RUN"
sleep 15
PID=$(cat "$RUN/pid")
LOG="$RUN/game.log"
total=$ALL
grep -qa "\[nwpad\] signatures: 0/$total resolved" "$LOG" || fail "expected 0/$total resolved"
[ "$(grep -ca '\[nwpad\] signature .*not found' "$LOG")" = "$total" ] || fail "not every miss was logged"
grep -qa 'camera backend: unavailable, movement backend: unavailable' "$LOG" || fail "features should be off"
"$TOOLS/nwpadctl" --wait-ready 30 ping >/dev/null || fail "broken build: control socket never answered"
"$TOOLS/nwpadctl" stick lx=1 ly=1 rx=1 ry=1 >/dev/null || fail "broken build: stick command"
f0=$("$TOOLS/nwpadctl" ping | python3 -c 'import json,sys; print(json.load(sys.stdin)["frame"])') || fail "broken build: ping"
sleep 5
"$TOOLS/nwpadctl" release >/dev/null || fail "broken build: release command"
f1=$("$TOOLS/nwpadctl" ping | python3 -c 'import json,sys; print(json.load(sys.stdin)["frame"])') || fail "broken build: ping"
kill -0 "$PID" || fail "game exited with broken signatures"
[ "$f1" -gt "$f0" ] || fail "frames stopped advancing"
echo "  frames advanced $f0 -> $f1 with full stick input and no signatures"
grep -a -m4 '\[nwpad\]' "$LOG" | sed 's/^/  /'
stop_pid "$PID"; PID=
echo "ok: game runs normally with every signature broken"
