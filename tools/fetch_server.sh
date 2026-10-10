#!/usr/bin/env bash
# Download and unpack an NWN:EE dedicated server package for research (R1, R3).
#   tools/fetch_server.sh <package-url>    # URL from https://nwn.beamdog.net/downloads
# Files go under ~/.nwpad/server/ (never into the repo). Records the nwserver-linux hash.
set -euo pipefail
. "$(dirname "$0")/common.sh"
URL="${1:?usage: $0 <package-url>}"
DEST="$NWPAD_STATE/server/$(basename "$URL" .zip)"
mkdir -p "$DEST"
curl -fL --retry 3 -o "$DEST/package.zip" "$URL"
unzip -oq "$DEST/package.zip" -d "$DEST"
BIN="$(find "$DEST" -path '*linux*' -name nwserver-linux -type f -print -quit)"
[ -n "$BIN" ] || die "nwserver-linux not found in package"
sha256sum "$BIN" | tee "$DEST/nwserver-linux.sha256"
echo "binary: $BIN"
echo "Record the hash and build in docs/re-notes.md (Binaries table)."
