#!/usr/bin/env bash
#
# gen-man.sh — regenerate docs/microide.1 from the single source of truth.
#
# The man page's CONTROL CHANNEL and COMMANDS sections are generated from
# ControlChannelHelpText() + WorkspaceDocumentedCommandUsages() (see
# src/workspace/ManPage.cpp), so the shipped man page can never drift from the
# implementation. This is a thin wrapper over `microide control-man`; the
# ManPageMatchesGenerator test fails if docs/microide.1 is edited by hand or
# left stale, so run this after changing the help text or the command registry.
#
# Usage:
#   tools/gen-man.sh [path-to-microide-binary]
# Defaults to build/microide/microide.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
binary="${1:-$repo_root/build/microide/microide}"

if [[ ! -x "$binary" ]]; then
  echo "gen-man.sh: microide binary not found at $binary (build it first, or pass a path)" >&2
  exit 1
fi

"$binary" control-man > "$repo_root/docs/microide.1"
echo "wrote $repo_root/docs/microide.1"
