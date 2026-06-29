#!/usr/bin/env bash
#
# capture-shots.sh — produce the five feature screenshots for the showcase site.
#
# Each scene relaunches a fresh microide on the demo fixture (so state never
# bleeds between shots), puts the repo in the right git state, drives the UI
# through the control channel, settles, and grabs the frame. Output PNGs land in
# docs/media/. See dev-docs/project/media-generation.md.
#
# Usage: capture-shots.sh [out-dir]   (default: <repo>/docs/media)

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

# Dark theme + (optional) HiDPI scale, applied at the top of every scene.
scene_prep() {
  cm_send colorscheme "$CM_THEME"
  if [[ "$CM_UI_SCALE" != "1" ]]; then cm_send ui-scale "$CM_UI_SCALE"; fi
  return 0
}

# Make <file> the focused editor with git detected and the file tree showing.
# Closes the auto-opened welcome tab and bounces through the SCM view so the
# status bar reports the right language + branch.
open_focused() {
  local file="$1"
  cm_close_welcome
  cm_send open "$file"
  cm_send sidebar-show git
  cm_send git-refresh
  cm_settle 1.0
  cm_send sidebar-show tree
  cm_send focus editor
}

# ---- 1. editor -----------------------------------------------------------
shot_editor() {
  cm_log "scene: editor"
  GIT reset -q --hard main
  cm_launch "$FIX"
  scene_prep
  open_focused src/scheduler.cpp
  cm_settle 1.2
  cm_capture "$OUT_DIR/shot-editor.png" --no-toast
  cm_kill_instance
}

# ---- 2. control channel (LLM-driven) -------------------------------------
shot_control() {
  cm_log "scene: control"
  GIT reset -q --hard main
  # A transcript script run inside the embedded terminal: the channel sets a
  # breakpoint (its gutter dot lands on main.cpp, proving the UI was driven) and
  # queries state, so the panel shows real request/response JSON.
  cat > "$CM_WORK/.control-demo.sh" <<EOF
#!/usr/bin/env bash
BIN="$CM_BIN"
p(){ printf '\033[38;5;81m➜\033[0m \033[1m%s\033[0m\n' "\$*"; sleep 0.15; }
clear
printf '\033[38;5;245m# an external agent / LLM drives microide over the JSON control channel\033[0m\n\n'
p 'microide control-send breakpoint-set src/main.cpp 9'; "\$BIN" control-send breakpoint-set src/main.cpp 9
p 'microide control-send --query breakpoints';           "\$BIN" control-send --query breakpoints
p 'microide control-send --query tabs';                  "\$BIN" control-send --query tabs
echo
EOF
  chmod +x "$CM_WORK/.control-demo.sh"
  cm_launch "$FIX"
  scene_prep
  cm_close_welcome
  cm_send open src/main.cpp
  cm_send sidebar-show tree
  cm_send focus editor
  # Drive the transcript by typing into the panel's default terminal.
  cm_send focus panel
  cm_settle 0.4
  cm_type "bash ~/.control-demo.sh"
  cm_key Return
  cm_settle 2.5
  cm_capture "$OUT_DIR/shot-control.png" --no-toast
  cm_kill_instance
}

# ---- 3. git diff ---------------------------------------------------------
shot_git_diff() {
  cm_log "scene: git-diff"
  GIT reset -q --hard main
  # Working-tree edit on the stable add() method (adds + removes) so the hunk is
  # visually rich and independent of the run() trace the merge branches rewrite.
  python3 - "$FIX/src/scheduler.cpp" <<'PY'
import sys, pathlib
p = pathlib.Path(sys.argv[1]); s = p.read_text()
s = s.replace(
'''void Scheduler::add(std::string name, int priority, int cost) {
    queue_.push_back(Task{std::move(name), priority, cost});
}''',
'''void Scheduler::add(std::string name, int priority, int cost) {
    // skip empty jobs and never schedule a negative cost
    if (name.empty()) {
        return;
    }
    queue_.push_back(Task{std::move(name), priority, std::max(0, cost)});
}''')
p.write_text(s)
PY
  cm_launch "$FIX"
  scene_prep
  cm_close_welcome
  cm_send review-branch
  cm_settle 1.4
  cm_capture "$OUT_DIR/shot-git-diff.png" --no-toast
  cm_kill_instance
}

# ---- 4. git merge (three-way conflict) -----------------------------------
shot_git_merge() {
  cm_log "scene: git-merge"
  GIT reset -q --hard main
  GIT clean -qfd
  GIT merge --no-edit feature/stable-sort >/dev/null 2>&1 || true   # leaves UU conflict
  cm_launch "$FIX"
  scene_prep
  cm_close_welcome
  cm_send review-conflicts
  cm_settle 1.4
  cm_capture "$OUT_DIR/shot-git-merge.png" --no-toast
  cm_kill_instance
  GIT merge --abort >/dev/null 2>&1 || true
}

# ---- 5. DAP debugger paused ----------------------------------------------
shot_dap() {
  cm_log "scene: dap"
  GIT reset -q --hard main
  GIT clean -qfd
  command -v g++ >/dev/null && g++ -g -O0 -std=c++20 -I"$FIX/include" \
    -o "$FIX/taskflow" "$FIX/src/main.cpp" "$FIX/src/scheduler.cpp"
  local bpline
  bpline="$(grep -n 'std::printf' "$FIX/src/scheduler.cpp" | head -1 | cut -d: -f1)"
  cm_launch "$FIX"
  scene_prep
  cm_close_welcome
  cm_send open src/scheduler.cpp
  cm_send sidebar-show tree
  cm_send breakpoint-set src/scheduler.cpp "$bpline"
  cm_send debug-pane-variables
  cm_send_wait stopped 40 debug-run --type gdb ./taskflow
  cm_settle 2.0
  cm_capture "$OUT_DIR/shot-dap.png" --no-toast
  cm_send debug-stop || true
  cm_kill_instance
}

main() {
  local only="${SHOT_ONLY:-}"
  if [[ -n "$only" ]]; then
    "shot_$only"
  else
    shot_editor
    shot_control
    shot_git_diff
    shot_git_merge
    shot_dap
  fi
  cm_log "screenshots written to $OUT_DIR"
}
main
