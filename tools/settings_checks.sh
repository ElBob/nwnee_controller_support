#!/usr/bin/env bash
# Settings "lite" checks (docs/settings-plan.md §9), in the test profile only:
# seeding + import, persistence across the game's own rewrite, settings.tml as the
# source of truth, enabled = false, and the config.toml fallback.
#   tools/settings_checks.sh    (on the box)
set -euo pipefail
. "$(dirname "$0")/common.sh"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; TOOLS="$ROOT/tools"
session_env
S="$NWPAD_STATE/userdir/settings.tml"
CFGDIR=$(mktemp -d); export XDG_CONFIG_HOME="$CFGDIR"   # never Robert's real config.toml
mkdir -p "$CFGDIR/nwpad"
fail() { echo "FAIL: $*"; exit 1; }
status() { "$TOOLS/nwpadctl" status | python3 -c "import json,sys; c=json.load(sys.stdin)['config']; print(' '.join(f'{k}={v}' for k,v in c.items()))"; }
launch() { RUN=$(NWPAD_CONFIG= "$TOOLS/run_game.sh" --timeout 90) || fail "launch: $RUN"; PID=$(cat "$RUN/pid"); }
quit() { stop_pid "$PID"; sleep 1; }
strip_section() { python3 - "$S" <<'PY'
import re, sys
p = sys.argv[1]; t = open(p).read()
t = re.sub(r'(?ms)^\[nwpad\]\n(?:[ \t].*\n|\[nwpad\.[^\]]*\]\n)*', '', t)
open(p, "w").write(t)
PY
}
cp -a "$S" "$S.checks-orig"
trap 'mv -f "$S.checks-orig" "$S"; rm -f "$S.bak-nwpad"; rm -rf "$CFGDIR"' EXIT

echo "== 1. seeding: no [nwpad], config.toml says turn speed 240"
strip_section; rm -f "$S.bak-nwpad"
printf 'camera_yaw_speed = 240\n' > "$CFGDIR/nwpad/config.toml"
launch
grep -qa 'added \[nwpad\] to .*imported from config.toml' "$RUN/game.log" || fail "section not seeded"
status; status | grep -q 'turn_speed=240.0' || fail "import not applied"
grep -q '^\[nwpad\]' "$S" || fail "no [nwpad] in settings.tml"
[ -f "$S.bak-nwpad" ] || fail "no backup"
quit
grep -q '^\[nwpad\]' "$S" && grep -qE 'turn-speed = 240' "$S" || fail "[nwpad] lost after the game rewrote settings.tml"
echo "ok: seeded, imported, and kept after the game's own save"

echo "== 2. settings.tml is the source of truth; config.toml is only a fallback"
printf 'camera_yaw_speed = 100\n' > "$CFGDIR/nwpad/config.toml"
python3 - "$S" <<'PY'
import sys; p = sys.argv[1]; t = open(p).read()
t = t.replace("turn-speed = 240", "turn-speed = 300", 1); open(p, "w").write(t)
PY
launch; grep -qa 'settings .*settings.tml: 8 setting(s) applied' "$RUN/game.log" || fail "settings.tml not read"
status; status | grep -q 'turn_speed=300.0' || fail "settings.tml value not used"
quit; echo "ok: settings.tml wins"

echo "== 3. enabled = false"
python3 - "$S" <<'PY'
import sys; p = sys.argv[1]; t = open(p).read()
t = t.replace("[nwpad]\n\tenabled = true", "[nwpad]\n\tenabled = false", 1); open(p, "w").write(t)
PY
RUN=$(NWPAD_CONFIG= "$TOOLS/run_game.sh" --timeout 90 -- +TestNewModule "Contest Of Champions 0492") || fail "launch"; PID=$(cat "$RUN/pid")
for i in $(seq 30); do "$TOOLS/nwpadctl" state 2>/dev/null | grep -q '"in_game": true' && break; sleep 1; done; sleep 2
status | grep -q 'enabled=False' || fail "not disabled"
y0=$("$TOOLS/nwpadctl" state | python3 -c 'import json,sys; print(json.load(sys.stdin)["camera"]["yaw"])')
"$TOOLS/nwpadctl" stick rx=1 >/dev/null; sleep 0.8; "$TOOLS/nwpadctl" release >/dev/null
y1=$("$TOOLS/nwpadctl" state | python3 -c 'import json,sys; print(json.load(sys.stdin)["camera"]["yaw"])')
[ "$y0" = "$y1" ] || fail "camera moved while disabled ($y0 -> $y1)"
hidden=$("$TOOLS/nwpadctl" state | python3 -c 'import json,sys; print(json.load(sys.stdin)["cursor"]["stick_hidden"])')
[ "$hidden" = False ] || fail "cursor hidden while disabled"
quit; echo "ok: disabled means idle"

echo "== 4. fallback: settings.tml unreadable -> config.toml"
chmod 000 "$S"
launch; chmod 644 "$S"
status; status | grep -q 'turn_speed=100.0' || fail "config.toml fallback not used"
quit; echo "ok: fallback"
echo "ALL SETTINGS CHECKS PASSED"
