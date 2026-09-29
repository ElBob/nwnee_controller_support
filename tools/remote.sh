#!/usr/bin/env bash
# Run project work on the test box from the agent's machine.
#   tools/remote.sh sync                 # rsync the repo to the box
#   tools/remote.sh build                # sync + configure + build + unit tests on the box
#   tools/remote.sh run <cmd...>         # sync + build, then run a command in the repo on the box
#   tools/remote.sh test [pytest args]   # sync + build + sigcheck + live tests, then pull artifacts
#   tools/remote.sh pull                 # pull box artifacts into ./artifacts/
set -euo pipefail
. "$(dirname "$0")/common.sh"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

sync() {
  rsync -az --delete --exclude build/ --exclude artifacts/ --exclude re-work/ \
    --exclude .git/ "$ROOT/" "$NWPAD_HOST:$NWPAD_REMOTE_DIR/"
}
remote() { ssh "$NWPAD_HOST" "cd ~/$NWPAD_REMOTE_DIR && $*"; }
build() {
  remote "cmake -S . -B build >/dev/null && cmake --build build -j && ctest --test-dir build --output-on-failure"
}
pull() {
  mkdir -p "$ROOT/artifacts"
  rsync -az "$NWPAD_HOST:$NWPAD_REMOTE_DIR/artifacts/" "$ROOT/artifacts/" 2>/dev/null || true
}

cmd="${1:-}"; shift || true
case "$cmd" in
  sync) sync ;;
  build) sync; build ;;
  run) sync; build; remote "$*" ;;
  test)
    sync; build
    remote "python3 tools/sigcheck"
    rc=0; remote "NWPAD_LIVE=1 python3 -m pytest tests/live -v $*" || rc=$?
    pull; exit $rc ;;
  pull) pull ;;
  *) die "usage: $0 sync|build|run|test|pull" ;;
esac
