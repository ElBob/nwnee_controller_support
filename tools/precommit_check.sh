#!/usr/bin/env bash
# Block commits containing game-derived or build material (CLAUDE.md rule 3).
set -euo pipefail
bad=0
while IFS= read -r -d '' f; do
  case "$f" in
    re-work/*|artifacts/*|build/*|testdata-local/*)
      echo "blocked: $f (local-only directory)"; bad=1; continue ;;
    *.mod|*.sav|*.bif|*.key|*.erf|*.hak|*.png|*.jpg|*.tga|*.dds)
      echo "blocked: $f (game data or screenshot type)"; bad=1; continue ;;
  esac
  if [ -f "$f" ] && [ "$(head -c 4 "$f" | od -An -tx1 | tr -d ' ')" = "7f454c46" ]; then
    echo "blocked: $f (ELF binary)"; bad=1
  fi
done < <(git diff --cached --name-only --diff-filter=ACMR -z)
if [ "$bad" -ne 0 ]; then
  echo "Commit rejected: see CLAUDE.md rule 3."
  exit 1
fi
