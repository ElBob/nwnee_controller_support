#!/usr/bin/env bash
# Settings "lite" checks (docs/settings-plan.md §9), in the test profile only:
# seeding + import, persistence across the game's own rewrite, settings.tml as the
# source of truth, enabled = false, the config.toml fallback, and the native entries
# in the Options window (Input > Camera): live apply, Cancel, Save, relaunch.
#   tools/settings_checks.sh    (on the box)
set -euo pipefail
. "$(dirname "$0")/common.sh"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; TOOLS="$ROOT/tools"
session_env
S="$NWPAD_STATE/userdir/settings.tml"
CFGDIR=$(mktemp -d); export XDG_CONFIG_HOME="$CFGDIR"   # never Robert's real config.toml
mkdir -p "$CFGDIR/nwpad"
fail() { echo "FAIL: $*"; [ -n "${PID:-}" ] && stop_pid "$PID" 5; exit 1; }
status() { "$TOOLS/nwpadctl" status | python3 -c "import json,sys; c=json.load(sys.stdin)['config']; print(' '.join(f'{k}={v}' for k,v in c.items()))"; }
launch() { RUN=$(NWPAD_CONFIG= "$TOOLS/run_game.sh" --timeout 90) || fail "launch: $RUN"; PID=$(cat "$RUN/pid"); }
quit() { stop_pid "$PID" 10; sleep 1; }
native() { "$TOOLS/nwpadctl" status | python3 -c "import json,sys; n=json.load(sys.stdin)['native']; print(n['$1'][${2:-0}] if n else 'none')"; }
config() { "$TOOLS/nwpadctl" status | python3 -c "import json,sys; print(json.load(sys.stdin)['config']['$1'])"; }
# Clicks at game-window coordinates (the test profile's 1024x768 window).
click() {
  local geo; geo=$(DISPLAY=:0 xdotool search --name "Neverwinter Nights: Enhanced" getwindowgeometry | awk '/Position/{print $2}' | tail -1)
  echo "click $1,$2 in window at $geo" >> "$RUN/clicks.log"
  DISPLAY=:0 python3 "$TOOLS/uinput_mouse.py" --to $(( ${geo%,*} + $1 )) $(( ${geo#*,} + $2 )) --click --settle 0.5
  sleep "${3:-1.5}"
}
shot() { DISPLAY=:0 spectacle -b -n -f -o "$RUN/$1.png" 2>/dev/null || true; }  # artifacts only
open_options() {  # (title screen) > main menu > Options > Game Options > Input
  local try added
  added=$(grep -ac "native settings: added" "$RUN/game.log" || true)
  for try in 1 2 3; do
    if [ "${1:-}" != again ]; then  # "again": back on the Options panel after Save/Cancel
      for _ in $(seq 60); do grep -qa "enabling main menu" "$RUN/game.log" && break; sleep 0.5; done
      DISPLAY=:0 xdotool search --name "Neverwinter Nights: Enhanced" windowactivate 2>/dev/null || true
      click 512 100 2   # focus (an unfocused window eats the first click) and dismiss the
      click 512 100 3   # title screen; the logo is inert on the main menu
      click 512 554 2
    fi
    click 512 244 3
    [ "$(grep -ac "native settings: added" "$RUN/game.log" || true)" -gt "$added" ] && break
    shot "no-rows-$try"
  done
  [ "$(grep -ac "native settings: added" "$RUN/game.log" || true)" -gt "$added" ] || fail "rows not added"
  click 150 447 2
}
strip_section() { python3 - "$S" <<'PY'
import re, sys
p = sys.argv[1]; t = open(p).read()
t = re.sub(r'(?ms)^\[nwpad\]\n(?:[ \t].*\n|\[nwpad\.[^\]]*\]\n)*', '', t)
open(p, "w").write(t)
PY
}
cp -a "$S" "$S.checks-orig"
python3 - "$S" <<'PY'  # no intro movies or splash: straight to the main menu
import sys; p = sys.argv[1]; t = open(p).read()
t = t.replace("[graphics.intro.splash]\n\t\t\tenabled = true", "[graphics.intro.splash]\n\t\t\tenabled = false")
t = t.replace("[graphics.movies.intro]\n\t\t\tenabled = true", "[graphics.movies.intro]\n\t\t\tenabled = false")
open(p, "w").write(t)
PY
trap 'mv -f "$S.checks-orig" "$S"; rm -f "$S.bak-nwpad"; rm -rf "$CFGDIR"' EXIT

if [ -z "${ONLY_NATIVE:-}" ]; then
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
launch; grep -qa 'settings .*settings.tml: 10 setting(s) applied' "$RUN/game.log" || fail "settings.tml not read"
status; status | grep -q 'turn_speed=300.0' || fail "settings.tml value not used"
quit; echo "ok: settings.tml wins"

echo "== 3. enabled = false"
python3 - "$S" <<'PY'
import sys; p = sys.argv[1]; t = open(p).read()
import re; t = re.sub(r"(?m)(^\[nwpad\]\n(?:\t.*\n)*?)\tenabled = true", r"\1\tenabled = false", t, 1)
open(p, "w").write(t)
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
fi
echo "== 5. native entries in Options > Input > Camera"
strip_section; rm -f "$CFGDIR/nwpad/config.toml"
launch; sleep 8
grep -qa 'native settings: registered' "$RUN/game.log" || fail "not registered"
[ "$(native nwpad.camera.tilt-speed)" = 90.0 ] || fail "tilt default"
open_options; shot options
click 820 426 1   # Controller Camera Tilt Speed slider
t=$(native nwpad.camera.tilt-speed); c=$(native nwpad.camera.tilt-speed 1)
[ "$t" != 90.0 ] && [ "$c" = 90.0 ] || fail "slider: working $t committed $c"
[ "$(config tilt_speed)" = "$t" ] || fail "not applied live"
click 684 315 1   # Controller Support off
[ "$(config enabled)" = False ] || fail "enabled not live"
click 589 696 2   # Cancel
[ "$(native nwpad.camera.tilt-speed)" = 90.0 ] && [ "$(config enabled)" = True ] || fail "Cancel didn't roll back"
[ "$(native nwpad.movement.run-point)" = 0.7875 ] || fail "run point moved by Cancel"
echo "ok: live apply, Cancel rolls back"
open_options again; click 820 426 1; t=$(native nwpad.camera.tilt-speed); click 434 696 2   # Save
[ "$(native nwpad.camera.tilt-speed 1)" = "$t" ] || fail "Save didn't commit"
grep -qE "tilt-speed = ${t%.0}" "$S" || fail "Save didn't write settings.tml"
quit; launch; sleep 5
[ "$(config tilt_speed)" = "$t" ] || fail "tilt not kept across relaunch"
quit; echo "ok: Save commits, writes settings.tml, survives relaunch"
echo "ALL SETTINGS CHECKS PASSED"
