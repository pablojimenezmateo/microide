#!/usr/bin/env bash
#
# record-hero.sh — record the showcase hero video.
#
# Runs one microide instance on the demo fixture, captures the Xvfb display with
# ffmpeg, and drives a choreographed tour (editor → three-way merge → diff →
# debugger → control channel) using live keystrokes (xdotool) and the control
# channel. Encodes the raw capture to docs/media/hero-demo.{mp4,webm} plus a
# poster still. See dev-docs/project/media-generation.md.
#
# Usage: record-hero.sh [out-dir]   (default: <repo>/docs/media)

set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$HERE/lib.sh"

OUT_DIR="${1:-$CM_REPO_ROOT/docs/media}"
mkdir -p "$OUT_DIR"

cm_init
trap cm_cleanup EXIT
cm_start_xvfb

FIX="$CM_WORK/taskflow"
bash "$HERE/make-fixture.sh" "$FIX" >/dev/null
GIT() { git -C "$FIX" "$@"; }
RAW="$CM_WORK/hero-raw.mkv"

# --- terminal helpers: type a command, run it, let output settle -------------
term_focus() { cm_send focus panel; cm_settle 0.3; }
run_cmd() { # run_cmd "<text>" [settle]
  cm_type "$1"; cm_key Return; cm_settle "${2:-1.2}"
}

# --- ffmpeg capture ----------------------------------------------------------
FF_PID=""
start_capture() {
  ffmpeg -nostdin -loglevel error -y \
    -f x11grab -video_size "${CM_W}x${CM_H}" -framerate 30 -i "$CM_DISPLAY_NUM" \
    -codec:v libx264 -preset ultrafast -qp 0 "$RAW" &
  FF_PID=$!
  sleep 1.0   # let the encoder spin up before the show starts
}
stop_capture() {
  [[ -n "$FF_PID" ]] || return 0
  kill -INT "$FF_PID" 2>/dev/null || true
  wait "$FF_PID" 2>/dev/null || true
  FF_PID=""
}

# --- the choreography --------------------------------------------------------
# Order matters: the debugger runs on a clean buffer BEFORE the live diff edit,
# so buffer and disk line up and there is no stray "reloaded from disk" churn.
choreograph() {
  cm_send colorscheme "$CM_THEME"
  if [[ "$CM_UI_SCALE" != "1" ]]; then cm_send ui-scale "$CM_UI_SCALE"; fi
  cm_close_welcome
  cm_send open src/scheduler.cpp
  cm_send sidebar-show tree
  cm_send focus editor
  cm_park_mouse
  cm_settle 2.2                                   # ── 1. editor beat

  # ── 2. three-way merge: run the merge live, then open the conflict tools
  term_focus
  run_cmd "git merge feature/stable-sort" 1.8
  cm_send review-conflicts
  cm_settle 3.2
  term_focus
  run_cmd "git merge --abort" 1.0
  cm_send git-refresh
  cm_send sidebar-show tree

  # ── 3. debugger: break, run, inspect locals, step (clean buffer)
  cm_send open src/scheduler.cpp
  cm_send focus editor
  local bpline
  bpline="$(grep -n 'std::printf' "$FIX/src/scheduler.cpp" | head -1 | cut -d: -f1)"
  cm_send breakpoint-set src/scheduler.cpp "$bpline"
  cm_send debug-pane-variables
  cm_send_wait stopped 40 debug-run --type gdb ./taskflow
  cm_park_mouse
  cm_settle 3.2
  cm_send debug-step-over; cm_settle 1.5
  cm_send debug-step-over; cm_settle 1.5
  cm_send debug-stop || true
  cm_settle 0.8

  # ── 4. working-tree diff: live edit at the top of the file, then review
  cm_send open src/scheduler.cpp
  cm_send focus editor
  cm_send goto 1
  cm_key Home
  cm_type "// taskflow - a tiny priority scheduler"
  cm_key Return
  cm_type "// (demo project for the microide showcase)"
  cm_key Return
  cm_settle 0.4
  cm_send save                                    # write so the working tree differs
  cm_settle 0.4
  cm_send review-branch
  cm_settle 3.2

  # ── 5. control channel: an external agent drives the same chokepoint
  cm_send open src/main.cpp
  cm_send focus editor
  cm_settle 0.5
  term_focus
  run_cmd "microide control-send breakpoint-set src/main.cpp 9" 1.4
  run_cmd "microide control-send --query breakpoints" 2.2
  cm_settle 1.5
}

main() {
  command -v ffmpeg >/dev/null || cm_die "ffmpeg not installed"
  GIT reset -q --hard main 2>/dev/null || true
  # compile the debuggee
  command -v g++ >/dev/null && g++ -g -O0 -std=c++20 -I"$FIX/include" \
    -o "$FIX/taskflow" "$FIX/src/main.cpp" "$FIX/src/scheduler.cpp"
  # microide must see 'microide' on PATH for the control-channel demo line
  export PATH="$(dirname "$CM_BIN"):$PATH"

  cm_launch "$FIX"
  start_capture
  choreograph
  stop_capture
  cm_kill_instance

  cm_log "encoding deliverables…"
  # H.264 MP4 — broadly compatible source, capped at 1080p height.
  ffmpeg -nostdin -loglevel error -y -i "$RAW" \
    -vf "scale=-2:'min(1080,ih)':flags=lanczos,format=yuv420p" \
    -codec:v libx264 -preset slow -crf 24 -movflags +faststart \
    -an "$OUT_DIR/hero-demo.mp4"
  # VP9 WebM — smaller alternate source.
  ffmpeg -nostdin -loglevel error -y -i "$RAW" \
    -vf "scale=-2:'min(1080,ih)':flags=lanczos" \
    -codec:v libvpx-vp9 -b:v 0 -crf 34 -row-mt 1 -an "$OUT_DIR/hero-demo.webm"
  # Poster: prefer the deterministic editor screenshot if it was captured this run;
  # otherwise grab the debugger beat (~middle of the tour) from the recording.
  if [[ -f "$OUT_DIR/shot-dap.png" ]]; then
    cp "$OUT_DIR/shot-dap.png" "$OUT_DIR/hero-poster.png"
  else
    local dur
    dur="$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$RAW" 2>/dev/null || echo 30)"
    ffmpeg -nostdin -loglevel error -y -ss "$(awk "BEGIN{printf \"%.2f\", $dur*0.5}")" -i "$RAW" \
      -vframes 1 -vf "scale=-2:'min(1080,ih)':flags=lanczos" "$OUT_DIR/hero-poster.png"
  fi

  cm_log "hero video written to $OUT_DIR (mp4 $(du -h "$OUT_DIR/hero-demo.mp4" | cut -f1), webm $(du -h "$OUT_DIR/hero-demo.webm" | cut -f1))"
}
main
