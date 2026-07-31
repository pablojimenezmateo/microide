#!/usr/bin/env bash
#
# overlay-backdrop-repro.sh — reproduce the modal-backdrop stacking bug.
#
# Opens the compare (commit) picker over a live editor on a private Xvfb display
# and samples the editor area at 0.35s intervals. The editor behind the overlay
# should stay dimmed-but-readable (as it does for the command palette and the
# file finder); instead it fades to a flat rectangle within about a second.
#
# Usage: tools/repro/overlay-backdrop-repro.sh <out-dir>
# Then:  python3 -c "from PIL import Image; ..."  or just look at c1..c6.png.
#
# Set REPRO_CMD to compare against a healthy overlay, e.g.
#   REPRO_CMD=command-palette tools/repro/overlay-backdrop-repro.sh /tmp/out
#
# See dev-docs/project/known-tech-debt.md, "Modal overlay backdrop stacks".
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$REPO/tools/capture-media/lib.sh"
OUT="${1:?out}"
mkdir -p "$OUT"
export CM_WORKDIR=/tmp/microide-repro
cm_init
trap cm_cleanup EXIT
cm_start_xvfb
FIX="$CM_WORK/taskflow"
bash "$REPO/tools/capture-media/make-fixture.sh" "$FIX" >/dev/null
python3 - "$FIX/src/scheduler.cpp" <<'PY'
import sys, pathlib
p = pathlib.Path(sys.argv[1]); s = p.read_text()
p.write_text(s.replace("queue_.push_back", "// tweak\n    queue_.push_back", 1))
PY

cm_launch "$FIX"
cm_send colorscheme default
cm_settle 0.8
cm_capture "$OUT/a-initial.png"
cm_send open src/scheduler.cpp
cm_send focus editor
cm_settle 0.8
cm_capture "$OUT/b-opened.png"
cm_query tabs > "$OUT/tabs-before.json" 2>&1 || true
cm_send ${REPRO_CMD:-compare src/scheduler.cpp}
for i in 1 2 3 4 5 6; do
  sleep 0.35
  cm_grab "$OUT/c$i.png"
done
cm_capture "$OUT/c-compare-picker.png"
cm_key Escape
cm_settle 0.8
cm_capture "$OUT/d-after-escape.png"
cp "$CM_WORK/microide.log" "$OUT/microide.log" || true
echo "done" >&2
