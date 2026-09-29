#!/usr/bin/env bash
# Build the release library and install it for Steam (plan §10). Run on the
# machine that plays the game.
#   tools/install.sh            # build + install to ~/.local/lib/nwpad/
#   tools/install.sh --uninstall
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$HOME/.local/lib/nwpad"

if [ "${1:-}" = --uninstall ]; then
  rm -f "$DEST/libnwpad.so"
  rmdir "$DEST" 2>/dev/null || true
  echo "Removed $DEST/libnwpad.so. Also clear NWN:EE's Steam launch options."
  exit 0
fi

cmake -S "$ROOT" -B "$ROOT/build-release" -DCMAKE_BUILD_TYPE=Release -DNWPAD_DEBUG_SURFACES=OFF >/dev/null
cmake --build "$ROOT/build-release" -j
mkdir -p "$DEST"
install -m 0644 "$ROOT/build-release/libnwpad.so" "$DEST/libnwpad.so"
cat <<MSG

Installed $DEST/libnwpad.so

In Steam: Neverwinter Nights: Enhanced Edition -> Properties -> Launch Options:

  LD_PRELOAD="\$HOME/.local/lib/nwpad/libnwpad.so:\$LD_PRELOAD" %command%

The game log shows "[nwpad] version ... loaded" when it's active.
MSG
