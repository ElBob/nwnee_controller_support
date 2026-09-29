# Shared settings for tools/*.sh. Override any of these in the environment
# or in ~/.nwpad/env on the test box.
[ -f "$HOME/.nwpad/env" ] && . "$HOME/.nwpad/env"

NWPAD_HOST="${NWPAD_HOST:-nwpad-box}"                 # SSH alias of the test box
NWPAD_REMOTE_DIR="${NWPAD_REMOTE_DIR:-nwpad}"         # repo checkout on the box, relative to ~
NWPAD_STATE="${NWPAD_STATE:-$HOME/.nwpad}"            # lock, userdir, logs on the box
NWN_ROOT="${NWN_ROOT:-$HOME/.local/share/Steam/steamapps/common/Neverwinter Nights}"
NWN_BIN="${NWN_BIN:-$NWN_ROOT/bin/linux-x86/nwmain-linux}"
NWPAD_DISPLAY="${NWPAD_DISPLAY:-:0}"
NWN_APPID="${NWN_APPID:-704450}"                  # Steam app id of NWN:EE

die() { echo "error: $*" >&2; exit 1; }
