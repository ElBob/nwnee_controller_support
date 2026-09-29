#!/usr/bin/env bash
# Collect crash evidence for a run directory produced by run_game.sh.
#   tools/collect_crash.sh <run-dir>
set -euo pipefail
. "$(dirname "$0")/common.sh"
RUN="${1:?usage: $0 <run-dir>}"
OUT="$HOME/$NWPAD_REMOTE_DIR/artifacts/$(basename "$RUN")"
mkdir -p "$OUT"
cp "$RUN"/game.log "$OUT"/ 2>/dev/null || true
tail -n 200 "$RUN/game.log" > "$OUT/game.tail.log" 2>/dev/null || true
PID="$(cat "$RUN/pid" 2>/dev/null || true)"
if command -v coredumpctl >/dev/null && [ -n "$PID" ]; then
  coredumpctl info "$PID" > "$OUT/coredump-info.txt" 2>&1 || true
  coredumpctl debug "$PID" --debugger-arguments="-batch -ex 'thread apply all bt'" \
    > "$OUT/backtrace.txt" 2>&1 || true
fi
echo "$OUT"
