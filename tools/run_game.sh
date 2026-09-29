#!/usr/bin/env bash
# Launch NWN:EE on the test box with libnwpad.so, under the session lock.
#   tools/run_game.sh [--timeout SECS] [--lib PATH] [-- extra game args]
# Prints the run directory (log, pid) on success. Exit codes:
#   0 ready, 10 lock held / game already running, 11 not ready before timeout, 12 exited early,
#   13 no display connected
set -euo pipefail
. "$(dirname "$0")/common.sh"

TIMEOUT=120
LIB="$HOME/$NWPAD_REMOTE_DIR/build/libnwpad.so"
while [ $# -gt 0 ]; do
  case "$1" in
    --timeout) TIMEOUT="$2"; shift 2 ;;
    --lib) LIB="$2"; shift 2 ;;
    --) shift; break ;;
    *) die "unknown option $1" ;;
  esac
done

[ -x "$NWN_BIN" ] || die "game binary not found: $NWN_BIN (set NWN_BIN)"
[ -f "$LIB" ] || die "library not found: $LIB"
mkdir -p "$NWPAD_STATE/runs"

# Session lock (CLAUDE.md rule 1).
[ -f "$NWPAD_STATE/session.held" ] && { echo "lock held: $(cat "$NWPAD_STATE/session.held")"; exit 10; }
exec 9>"$NWPAD_STATE/session.lock"
flock -n 9 || { echo "another agent session holds the lock"; exit 10; }
# The game process itself (by name, so tools with the binary path in their arguments,
# like Ghidra, don't match), live only: a killed game can linger as a zombie.
if pgrep -r R,S,D -x nwmain-linux >/dev/null; then echo "nwmain-linux already running; not launching"; exit 10; fi

# With no monitor connected (the box's KVM switched away), KWin never maps the
# window and SDL_CreateWindow blocks forever. Report it as an environment failure.
if ! grep -qx connected /sys/class/drm/card*-*/status 2>/dev/null; then
  echo "no display connected (monitor off or KVM switched away); not launching"; exit 13
fi

# KDE powers the monitor down when idle, and with the output off KWin doesn't map
# the window either (re-notes: test box environment). Wake it before launching;
# an inhibit below keeps it on while the game runs.
session_env || die "no Plasma session found for $(id -un)"
kscreen-doctor --dpms on >/dev/null 2>&1 || true
for _ in $(seq 20); do grep -qx On /sys/class/drm/card*-*/dpms 2>/dev/null && break; sleep 0.25; done

RUN="$NWPAD_STATE/runs/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$RUN"

# Isolation from Robert's NWN data (plan §8.2, re-notes F10): the game's own
# -userdirectory option. "home" overrides HOME instead, which also hides Steam.
ISOLATION="${NWPAD_ISOLATION:-userdir}"
GAME_ENV=() GAME_ARGS=()
case "$ISOLATION" in
  userdir) GAME_ARGS=(-userdirectory "$NWPAD_STATE/userdir"); mkdir -p "$NWPAD_STATE/userdir" ;;
  home) GAME_ENV=(HOME="$NWPAD_STATE/userdir-home"); mkdir -p "$NWPAD_STATE/userdir-home" ;;
  none) ;;  # only for manual debugging
  *) die "unknown NWPAD_ISOLATION=$ISOLATION" ;;
esac

TOOLS="$(cd "$(dirname "$0")" && pwd)"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
rm -f "$XDG_RUNTIME_DIR/nwpad.sock"  # stale socket from an earlier run

# SteamAppId stops the Steam API from relaunching the game through Steam, which
# would drop the preload and the isolated user directory. The game runs on
# XWayland, so it gets DISPLAY but not the session's WAYLAND_DISPLAY.
cd "$(dirname "$NWN_BIN")"
env -u WAYLAND_DISPLAY "${GAME_ENV[@]}" DISPLAY="$NWPAD_DISPLAY" XAUTHORITY="${XAUTHORITY:-}" \
    SteamAppId="$NWN_APPID" SteamGameId="$NWN_APPID" NWPAD_SOCKET=1 \
    LD_PRELOAD="$LIB${LD_PRELOAD:+:$LD_PRELOAD}" \
    "$NWN_BIN" "${GAME_ARGS[@]}" "$@" >"$RUN/game.log" 2>&1 9>&- &
PID=$!
echo "$PID" > "$RUN/pid"
# Hold a screen/power inhibit exactly as long as the game runs.
kde-inhibit --power --screenSaver tail --pid="$PID" -f /dev/null >/dev/null 2>&1 9>&- &

# Readiness: the control socket answers a ping from the game's frame loop.
for _ in $(seq "$TIMEOUT"); do
  if ! kill -0 "$PID" 2>/dev/null; then echo "game exited early; see $RUN/game.log"; exit 12; fi
  if "$TOOLS/nwpadctl" --timeout 3 ping >/dev/null 2>&1; then echo "$RUN"; exit 0; fi
  sleep 1
done
stop_pid "$PID"
echo "not ready after ${TIMEOUT}s; see $RUN/game.log"
exit 11
