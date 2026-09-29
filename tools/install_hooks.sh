#!/usr/bin/env bash
# Install the pre-commit check that blocks game-derived material (CLAUDE.md rule 3).
set -euo pipefail
ROOT="$(git rev-parse --show-toplevel)"
ln -sf ../../tools/precommit_check.sh "$ROOT/.git/hooks/pre-commit"
echo "pre-commit hook installed"
