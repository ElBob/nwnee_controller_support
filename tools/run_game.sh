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
if pgrep -f nwmain-linux >/dev/null; then echo "nwmain-linux already running; not launching"; exit 10; fi

# With no monitor connected (the box's KVM switched away), KWin never maps the
# window and SDL_CreateWindow blocks forever. Report it as an environment failure.
if ! grep -qx connected /sys/class/drm/card*-*/status 2>/dev/null; then
  echo "no display connected (monitor off or KVM switched away); not launching"; exit 13
fi

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

# SteamAppId stops the Steam API from relaunching the game through Steam, which
# would drop the preload and the isolated user directory.
if [ -z "${XAUTHORITY:-}" ]; then
  XAUTHORITY="$(ls -t /run/user/"$(id -u)"/xauth_* 2>/dev/null | head -1 || true)"
fi

cd "$(dirname "$NWN_BIN")"
env "${GAME_ENV[@]}" DISPLAY="$NWPAD_DISPLAY" XAUTHORITY="$XAUTHORITY" \
    SteamAppId="$NWN_APPID" SteamGameId="$NWN_APPID" NWPAD_SOCKET=1 \
    LD_PRELOAD="$LIB${LD_PRELOAD:+:$LD_PRELOAD}" \
    "$NWN_BIN" "${GAME_ARGS[@]}" "$@" >"$RUN/game.log" 2>&1 9>&- &
PID=$!
echo "$PID" > "$RUN/pid"

# Readiness: until the control socket exists (M0), "ready" means the library loaded.
for _ in $(seq "$TIMEOUT"); do
  if ! kill -0 "$PID" 2>/dev/null; then echo "game exited early; see $RUN/game.log"; exit 12; fi
  if grep -q '^\[nwpad\] version' "$RUN/game.log"; then echo "$RUN"; exit 0; fi
  sleep 1
done
kill "$PID" 2>/dev/null || true
echo "not ready after ${TIMEOUT}s; see $RUN/game.log"
exit 11
