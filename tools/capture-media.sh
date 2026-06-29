#!/usr/bin/env bash
#
# capture-media.sh — regenerate every showcase asset under docs/media/.
#
# This is the single command the release process (and the "the UI changed, so the
# media is stale" rule) calls. It drives a headless microide on a private Xvfb
# display through the control channel, captures the five feature screenshots, and
# records + encodes the hero video. Nothing touches the real desktop or the real
# microide config. See dev-docs/project/media-generation.md.
#
# Usage:
#   tools/capture-media.sh [--shots-only|--video-only] [--hidpi] [--keep-work]
#                          [--out <dir>] [--bin <microide>]
#
# Dependencies: Xvfb, ffmpeg, ImageMagick (import/convert), xdotool, and (for the
# debugger scene) gdb plus the bundled gdb-dap plugin. All are checked at runtime.

set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"

OUT_DIR="$REPO/docs/media"
DO_SHOTS=1; DO_VIDEO=1
export CM_HIDPI="${CM_HIDPI:-0}"
export CM_KEEP_WORK="${CM_KEEP_WORK:-0}"
BIN="${MICROIDE_BIN:-$REPO/build/microide/microide}"

usage() { sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --shots-only) DO_VIDEO=0 ;;
    --video-only) DO_SHOTS=0 ;;
    --hidpi)      export CM_HIDPI=1 ;;
    --keep-work)  export CM_KEEP_WORK=1 ;;
    --out)        OUT_DIR="$2"; shift ;;
    --bin)        BIN="$2"; shift ;;
    -h|--help)    usage 0 ;;
    *) echo "capture-media: unknown arg '$1'" >&2; usage 2 ;;
  esac
  shift
done
export MICROIDE_BIN="$BIN"

# Build the binary if it is missing (mirrors tools/gen-man.sh's expectation).
if [[ ! -x "$BIN" ]]; then
  echo "capture-media: $BIN missing — building it…" >&2
  cmake -S "$REPO" -B "$REPO/build" >/dev/null
  cmake --build "$REPO/build" --target microide -j"$(nproc)"
fi

mkdir -p "$OUT_DIR"
echo "capture-media: out=$OUT_DIR hidpi=$CM_HIDPI shots=$DO_SHOTS video=$DO_VIDEO"

[[ "$DO_SHOTS" == "1" ]] && bash "$HERE/capture-media/capture-shots.sh" "$OUT_DIR"
[[ "$DO_VIDEO" == "1" ]] && bash "$HERE/capture-media/record-hero.sh"  "$OUT_DIR"

echo "capture-media: done. Assets in $OUT_DIR:"
ls -1 "$OUT_DIR" | sed 's/^/  /'
