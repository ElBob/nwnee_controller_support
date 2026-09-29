#!/usr/bin/env bash
# Robert's manual session lock on the test box.
#   tools/lock.sh hold      # agent will not launch the game
#   tools/lock.sh release
#   tools/lock.sh status
set -euo pipefail
. "$(dirname "$0")/common.sh"
mkdir -p "$NWPAD_STATE"
HELD="$NWPAD_STATE/session.held"
case "${1:-status}" in
  hold)    echo "held by $(whoami) at $(date -Is)" > "$HELD"; echo "held" ;;
  release) rm -f "$HELD"; echo "released" ;;
  status)
    if [ -f "$HELD" ]; then echo "HELD: $(cat "$HELD")"; else echo "free"; fi
    if pgrep -f nwmain-linux >/dev/null; then echo "nwmain-linux is running"; fi ;;
  *) die "usage: $0 hold|release|status" ;;
esac
