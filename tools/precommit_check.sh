#!/usr/bin/env bash
# Block commits containing game-derived or build material (CLAUDE.md rule 3).
set -euo pipefail
shopt -s nocasematch   # .PNG as .png
bad=0
while IFS= read -r -d '' f; do
  case "$f" in
    re-work/*|artifacts/*|build/*|testdata-local/*)
      echo "blocked: $f (local-only directory)"; bad=1; continue ;;
    *.mod|*.nwm|*.sav|*.bif|*.key|*.erf|*.hak|*.tlk|*.2da|*.dlg|*.bic|*.utc|*.uti|*.utp|*.utm|*.utd|*.ute|*.uts|*.utt|*.utw|*.are|*.ifo|*.gic|*.jrl|*.fac|*.ncs|*.mdl|*.wok|*.plt|*.ssf|*.bik|*.wav|*.bmu|*.png|*.jpg|*.jpeg|*.bmp|*.webp|*.tga|*.dds)
      echo "blocked: $f (game data or screenshot type)"; bad=1; continue ;;
  esac
  # GFF files (conversations, characters, areas...) under any name: "XXXXV3.2".
  if [ -f "$f" ] && head -c 8 "$f" | LC_ALL=C grep -qaE '^[A-Z0-9 ]{4}V3\.[0-9]$'; then
    echo "blocked: $f (game GFF data)"; bad=1; continue
  fi
  if [ -f "$f" ] && [ "$(head -c 4 "$f" | od -An -tx1 | tr -d ' ')" = "7f454c46" ]; then
    echo "blocked: $f (ELF binary)"; bad=1
  fi
done < <(git diff --cached --name-only --diff-filter=ACMR -z)
if [ "$bad" -ne 0 ]; then
  echo "Commit rejected: see CLAUDE.md rule 3."
  exit 1
fi
