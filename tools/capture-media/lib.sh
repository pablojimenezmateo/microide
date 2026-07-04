#!/usr/bin/env bash
#
# lib.sh — shared helpers for the microide media-capture pipeline.
#
# Sourced by capture-shots.sh and record-hero.sh. Encapsulates the proven
# "headless capture" recipe: run microide on a private Xvfb display under the
# X11 SDL driver, isolate all on-disk state in a throwaway XDG tree (so the
# real user config / session is never touched), seed the bundled plugins, drive
# the instance through the control channel, and grab frames with ImageMagick.
#
# Every capture runs offscreen on Xvfb, so it never steals the real desktop and
# is reproducible on a headless box / CI. See dev-docs/project/media-generation.md.

set -euo pipefail

CM_REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CM_BIN="${MICROIDE_BIN:-$CM_REPO_ROOT/build/microide/microide}"

# Capture geometry. HiDPI doubles the framebuffer and the UI scale so text is
# rendered at 2x and downsamples crisply.
CM_HIDPI="${CM_HIDPI:-0}"
if [[ "$CM_HIDPI" == "1" ]]; then
  CM_W="${CM_W:-2880}"; CM_H="${CM_H:-1800}"; CM_UI_SCALE="${CM_UI_SCALE:-2}"
else
  CM_W="${CM_W:-1440}"; CM_H="${CM_H:-900}"; CM_UI_SCALE="${CM_UI_SCALE:-1}"
fi
# The showcase ships the built-in default theme — what users see out of the box.
CM_THEME="${CM_THEME:-default}"
CM_DISPLAY_NUM="${CM_DISPLAY_NUM:-:99}"

# Plugins seeded into the isolated config so adapters/highlighters are present.
CM_SEED_PLUGINS=(${CM_SEED_PLUGINS:-gdb-dap})

# Populated by cm_init / cm_start_xvfb / cm_launch.
CM_WORK=""; CM_XVFB_PID=""; CM_MID_PID=""

cm_log() { printf '  [capture] %s\n' "$*" >&2; }
cm_die() { printf 'capture-media: %s\n' "$*" >&2; exit 1; }

# cm_init [workdir] — create the isolated XDG tree + seed plugins.
cm_init() {
  [[ -x "$CM_BIN" ]] || cm_die "microide binary not found at $CM_BIN (build it, or set MICROIDE_BIN)"
  command -v Xvfb   >/dev/null || cm_die "Xvfb not installed (apt install xvfb)"
  command -v import >/dev/null || cm_die "ImageMagick 'import' not installed (apt install imagemagick)"

  # A fixed, neutral work root keeps the embedded terminal prompt and the control
  # channel's JSON (which echoes absolute paths) free of random scratch suffixes.
  CM_WORK="${1:-${CM_WORKDIR:-/tmp/microide-demo}}"
  case "$CM_WORK" in
    *microide*) rm -rf "$CM_WORK" ;;   # safe: only ever our own managed dir
  esac
  mkdir -p "$CM_WORK"
  # Point HOME at the work dir so the embedded terminal's shell prompt renders a
  # clean "~/taskflow" path instead of leaking the scratch mktemp path.
  export HOME="$CM_WORK"
  export XDG_CONFIG_HOME="$CM_WORK/config"
  export XDG_STATE_HOME="$CM_WORK/state"
  export XDG_DATA_HOME="$CM_WORK/data"
  export XDG_RUNTIME_DIR="$CM_WORK/run"
  mkdir -p "$XDG_CONFIG_HOME" "$XDG_STATE_HOME" "$XDG_DATA_HOME" "$XDG_RUNTIME_DIR"
  chmod 700 "$XDG_RUNTIME_DIR"

  # Clean shell rc for the embedded terminal: a tidy prompt + a clear so the
  # panel never shows distro MOTD noise or the scratch path.
  cat > "$CM_WORK/.bashrc" <<'RC'
PS1='\[\e[38;5;81m\]\u@dev\[\e[0m\]:\[\e[38;5;180m\]\w\[\e[0m\]$ '
clear
RC

  local plugin_dst="$XDG_CONFIG_HOME/microide/plugins"
  mkdir -p "$plugin_dst"
  local p
  for p in "${CM_SEED_PLUGINS[@]}"; do
    if [[ -d "$CM_REPO_ROOT/plugins/$p" ]]; then
      cp -r "$CM_REPO_ROOT/plugins/$p" "$plugin_dst/"
    else
      cm_log "warning: bundled plugin '$p' not found in repo, skipping"
    fi
  done
  cm_log "isolated state in $CM_WORK (plugins: ${CM_SEED_PLUGINS[*]})"
}

# cm_start_xvfb — launch the private virtual display.
cm_start_xvfb() {
  Xvfb "$CM_DISPLAY_NUM" -screen 0 "${CM_W}x${CM_H}x24" -nolisten tcp >/dev/null 2>&1 &
  CM_XVFB_PID=$!
  export DISPLAY="$CM_DISPLAY_NUM"
  export SDL_VIDEODRIVER=x11
  # Wait for the display to accept connections.
  local i ready=0
  for i in $(seq 1 40); do
    if command -v xdpyinfo >/dev/null && xdpyinfo -display "$CM_DISPLAY_NUM" >/dev/null 2>&1; then
      ready=1
      break
    fi
    kill -0 "$CM_XVFB_PID" 2>/dev/null || break
    sleep 0.1
  done
  if [[ "$ready" != "1" ]]; then
    cm_die "Xvfb failed to start on $CM_DISPLAY_NUM; if the log mentions /tmp/.X11-unix, repair it with: sudo chown root:root /tmp/.X11-unix && sudo chmod 1777 /tmp/.X11-unix"
  fi
  cm_log "Xvfb up on $CM_DISPLAY_NUM (${CM_W}x${CM_H})"
}

# cm_launch <project> [extra microide args...] — start an instance, wait ready.
cm_launch() {
  local proj="$1"; shift || true
  "$CM_BIN" "$proj" --set control.enabled true "$@" >"$CM_WORK/microide.log" 2>&1 &
  CM_MID_PID=$!
  local i
  for i in $(seq 1 80); do
    if "$CM_BIN" control-list 2>/dev/null | grep -q .; then
      # one extra settle so the first frame is fully painted
      sleep 0.6
      cm_log "instance ready (pid $CM_MID_PID)"
      return 0
    fi
    kill -0 "$CM_MID_PID" 2>/dev/null || cm_die "microide exited early; see $CM_WORK/microide.log"
    sleep 0.25
  done
  cm_die "microide control channel never came up; see $CM_WORK/microide.log"
}

# cm_send <command words...> — run a control command, fail on a not-ok reply.
cm_send() {
  "$CM_BIN" control-send "$@" >/dev/null 2>&1 \
    || cm_log "warning: control-send '$*' returned non-ok"
}

# cm_query <verb> — print the JSONL reply for a query verb to stdout.
cm_query() { "$CM_BIN" control-send --query "$1"; }

# cm_send_wait <event> <timeout> <command words...> — run and block for an event.
cm_send_wait() {
  local ev="$1" to="$2"; shift 2
  "$CM_BIN" control-send --timeout "$to" --wait "$ev" "$@" >"$CM_WORK/wait.log" 2>&1 || true
}

# cm_kill_instance — stop the current microide, wait for the socket to clear.
cm_kill_instance() {
  [[ -n "$CM_MID_PID" ]] || return 0
  kill "$CM_MID_PID" 2>/dev/null || true
  wait "$CM_MID_PID" 2>/dev/null || true
  CM_MID_PID=""
  # Let the descriptor file disappear so the next cm_launch auto-discovers cleanly.
  local i
  for i in $(seq 1 20); do
    "$CM_BIN" control-list 2>/dev/null | grep -q . || break
    sleep 0.1
  done
}

# cm_settle [seconds] — give the renderer time to paint (toasts fade ~2.5s).
cm_settle() { sleep "${1:-0.8}"; }

# --- keyboard injection (xdotool) -----------------------------------------
# The SDL window is the only client on the bare Xvfb display, so it owns focus.
CM_WIN=""
cm_window() {
  if [[ -z "$CM_WIN" ]]; then
    local id i
    for i in $(seq 1 20); do
      id="$(xdotool search --onlyvisible --name microide 2>/dev/null | tail -1)"
      [[ -z "$id" ]] && id="$(xdotool search --name microide 2>/dev/null | tail -1)"
      [[ -n "$id" ]] && { CM_WIN="$id"; break; }
      sleep 0.2
    done
  fi
  printf '%s' "$CM_WIN"
}
# Give the SDL window real input focus (no WM on Xvfb). windowactivate BLOCKS
# without a WM, so use windowfocus only, guarded by a timeout.
cm_focus_window() { local w; w="$(cm_window)"; [[ -n "$w" ]] && timeout 3 xdotool windowfocus "$w" 2>/dev/null || true; }
cm_key()  { local w; w="$(cm_window)"; cm_focus_window; timeout 4 xdotool key  --clearmodifiers ${w:+--window "$w"} "$@" 2>/dev/null || true; }
cm_type() { local w; w="$(cm_window)"; cm_focus_window; timeout 30 xdotool type --clearmodifiers ${w:+--window "$w"} --delay "${CM_TYPE_DELAY:-55}" -- "$1" 2>/dev/null || true; }
# Park the pointer in a corner so idle hover popups (value tooltips, blame) never
# bleed into a still.
cm_park_mouse() { timeout 3 xdotool mousemove 8 8 2>/dev/null || true; }
# Close the auto-opened welcome tab (README / implementation-guide) so the
# status bar + command target follow the file we actually open.
cm_close_welcome() { cm_key ctrl+w; cm_settle 0.4; }

# cm_capture <outfile.png> [--crop] — grab the current frame.
# Default grabs the whole virtual screen (window fills it). --no-toast waits for
# transient toasts to fade first.
cm_capture() {
  local out="$1"; shift || true
  local fade=0
  while [[ $# -gt 0 ]]; do case "$1" in --no-toast) fade=1;; esac; shift; done
  cm_park_mouse
  [[ "$fade" == "1" ]] && sleep 2.8
  mkdir -p "$(dirname "$out")"
  import -display "$CM_DISPLAY_NUM" -window root "$out"
  if [[ "$CM_HIDPI" == "1" ]]; then
    # downsample 2x -> crisp 1x deliverable
    convert "$out" -resize 50% "$out"
  fi
  cm_log "wrote $out ($(identify -format '%wx%h' "$out" 2>/dev/null))"
}

cm_cleanup() {
  [[ -n "$CM_MID_PID"  ]] && kill "$CM_MID_PID"  2>/dev/null || true
  [[ -n "$CM_XVFB_PID" ]] && kill "$CM_XVFB_PID" 2>/dev/null || true
  wait 2>/dev/null || true
  if [[ -n "$CM_WORK" && "${CM_KEEP_WORK:-0}" != "1" ]]; then
    rm -rf "$CM_WORK"
  fi
}
