#!/usr/bin/env bash
#
# record-hero.sh — record the showcase hero video.
#
# Runs one microide instance on the demo fixture, captures seven short Xvfb
# clips, then stitches them into a labeled hero trailer:
# large-file scroll → search → terminal → diff → merge → debugger → control channel.
# Encodes the result to docs/media/hero-demo.{mp4,webm} plus a poster still. See
# dev-docs/project/media-generation.md.
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
RAW_EDITOR="$CM_WORK/hero-editor.mkv"
RAW_SEARCH="$CM_WORK/hero-search.mkv"
RAW_TERMINAL="$CM_WORK/hero-terminal.mkv"
RAW_DIFF="$CM_WORK/hero-diff.mkv"
RAW_MERGE="$CM_WORK/hero-merge.mkv"
RAW_DEBUG="$CM_WORK/hero-debug.mkv"
RAW_CONTROL="$CM_WORK/hero-control.mkv"

# --- terminal helpers: type a command, run it, let output settle -------------
term_focus() { cm_send focus panel; cm_settle 0.3; }
run_cmd() { # run_cmd "<text>" [settle]
  cm_type "$1"; cm_key Return; cm_settle "${2:-1.2}"
}

# --- fixture enrichment ------------------------------------------------------
add_large_demo_file() {
  local file="$FIX/src/pipeline.cpp"
  cat > "$file" <<'EOF'
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string_view>

namespace taskflow::pipeline {

struct JobSample {
    std::string_view queue;
    std::uint32_t arrivals;
    std::uint32_t completed;
    std::uint32_t retries;
};

constexpr std::array<JobSample, 8> kSamples{{
    {"ingest", 124, 118, 3},
    {"index", 98, 96, 1},
    {"compact", 42, 39, 2},
    {"flush", 76, 75, 0},
    {"backup", 31, 29, 1},
    {"notify", 66, 63, 4},
    {"audit", 18, 18, 0},
    {"archive", 12, 11, 1},
}};

std::uint32_t weighted_score(const JobSample& sample) {
    return sample.completed * 5 + sample.arrivals * 2 - sample.retries * 7;
}

EOF
  local i
  for i in $(seq 1 96); do
    cat >> "$file" <<EOF
std::uint32_t stage_${i}_score(std::uint32_t salt) {
    std::uint32_t score = salt + ${i}u;
    for (const JobSample& sample : kSamples) {
        score += weighted_score(sample);
        score ^= (sample.arrivals + ${i}u) << (${i}u % 5u);
        score = (score << 3u) | (score >> 29u);
    }
    return score;
}

EOF
  done
  cat >> "$file" <<'EOF'
void print_pipeline_report() {
    std::uint32_t total = 0;
    for (const JobSample& sample : kSamples) {
        total += weighted_score(sample);
        std::printf("%-8.*s arrivals=%u completed=%u retries=%u score=%u\n",
                    static_cast<int>(sample.queue.size()), sample.queue.data(),
                    sample.arrivals, sample.completed, sample.retries,
                    weighted_score(sample));
    }
    std::printf("pipeline weighted total: %u\n", total);
}

}  // namespace taskflow::pipeline
EOF
  (
    cd "$FIX"
    git add src/pipeline.cpp
    GIT_AUTHOR_DATE="${CM_FIXTURE_DATE3:-2026-06-18T10:30:00}" \
    GIT_COMMITTER_DATE="${CM_FIXTURE_DATE3:-2026-06-18T10:30:00}" \
      git commit -q -m "Add pipeline workload showcase"
  )
}

apply_rich_diff_edit() {
  python3 - "$FIX/src/scheduler.cpp" <<'PY'
import pathlib
import sys

p = pathlib.Path(sys.argv[1])
s = p.read_text()
old_include = '''#include <algorithm>
#include <cstdio>
'''
new_include = '''#include <algorithm>
#include <cstdio>
#include <string_view>
'''
old_body = '''namespace taskflow {

void Scheduler::add(std::string name, int priority, int cost) {
    queue_.push_back(Task{std::move(name), priority, cost});
}

int Scheduler::run() {
    std::sort(queue_.begin(), queue_.end(),
              [](const Task& a, const Task& b) { return a.priority > b.priority; });

    int total = 0;
    int ran = 0;
    for (const Task& task : queue_) {
        total += task.cost;
        std::printf("[%d/%zu] %-8s  p=%d  cost=%d  total=%d\\n",
                    ++ran, queue_.size(), task.name.c_str(), task.priority, task.cost, total);
    }
    return total;
}
'''
new_body = '''namespace taskflow {

namespace {

int clamp_cost(int cost) {
    return std::max(1, std::min(cost, 99));
}

int normalized_priority(int priority) {
    return std::max(0, std::min(priority, 10));
}

std::string_view bucket_for(const Task& task) {
    if (task.priority >= 8) return "hot";
    if (task.priority >= 4) return "warm";
    return "cold";
}

}  // namespace

void Scheduler::add(std::string name, int priority, int cost) {
    if (name.empty()) {
        std::printf("skip empty task\\n");
        return;
    }
    queue_.push_back(Task{std::move(name), normalized_priority(priority), clamp_cost(cost)});
}

int Scheduler::run() {
    std::stable_sort(queue_.begin(), queue_.end(), [](const Task& a, const Task& b) {
        if (a.priority != b.priority) return a.priority > b.priority;
        if (a.cost != b.cost) return a.cost < b.cost;
        return a.name < b.name;
    });

    int total = 0;
    int ran = 0;
    int hot_path_cost = 0;
    for (const Task& task : queue_) {
        total += task.cost;
        if (task.priority >= 8) {
            hot_path_cost += task.cost;
        }
        std::printf("[%d/%zu] %-8s  bucket=%-4.*s  p=%02d  cost=%02d  total=%03d\\n",
                    ++ran, queue_.size(), task.name.c_str(),
                    static_cast<int>(bucket_for(task).size()), bucket_for(task).data(),
                    task.priority, task.cost, total);
    }
    std::printf("hot path cost: %d / total: %d\\n", hot_path_cost, total);
    return total;
}
'''
if old_include not in s:
    raise SystemExit("expected scheduler include block not found")
if old_body not in s:
    raise SystemExit("expected scheduler body block not found")
s = s.replace(old_include, new_include)
s = s.replace(old_body, new_body)
p.write_text(s)
PY
}

# --- ffmpeg capture ----------------------------------------------------------
FF_PID=""
start_capture() {
  local out="$1"
  ffmpeg -nostdin -loglevel error -y \
    -f x11grab -draw_mouse 0 -video_size "${CM_W}x${CM_H}" -framerate 30 -i "$CM_DISPLAY_NUM" \
    -codec:v libx264 -preset ultrafast -qp 0 "$out" &
  FF_PID=$!
  sleep 0.5   # let the encoder spin up before the beat starts
}
stop_capture() {
  [[ -n "$FF_PID" ]] || return 0
  kill -INT "$FF_PID" 2>/dev/null || true
  wait "$FF_PID" 2>/dev/null || true
  FF_PID=""
}

# --- the choreography --------------------------------------------------------
choreograph() {
  cm_send colorscheme "$CM_THEME"
  if [[ "$CM_UI_SCALE" != "1" ]]; then cm_send ui-scale "$CM_UI_SCALE"; fi
  cm_close_welcome

  # ── 1. editor: open a larger file and scroll through real code.
  cm_send open src/pipeline.cpp
  cm_send sidebar-show tree
  cm_send focus editor
  cm_park_mouse
  cm_settle 1.2
  start_capture "$RAW_EDITOR"
  cm_settle 0.8
  cm_key Page_Down; cm_settle 0.8
  cm_key Page_Down; cm_settle 0.8
  cm_key Page_Up; cm_settle 0.8
  cm_key End; cm_settle 0.8
  cm_key Home; cm_settle 0.8
  stop_capture

  # ── 2. project search: async search results with match highlighting.
  cm_send project-search weighted_score
  cm_park_mouse
  cm_settle 0.8
  start_capture "$RAW_SEARCH"
  cm_settle 4.0
  stop_capture
  cm_send focus editor
  cm_settle 0.4

  # ── 3. terminal: PTY-backed panel running the fixture binary.
  term_focus
  start_capture "$RAW_TERMINAL"
  run_cmd "./taskflow" 3.4
  stop_capture
  cm_send focus editor
  cm_settle 0.4

  # ── 4. working-tree diff: rewrite scheduler logic, then review the generated hunk.
  cm_send open src/scheduler.cpp
  cm_send focus editor
  apply_rich_diff_edit
  cm_send git-refresh
  cm_send review-branch
  cm_settle 1.6
  start_capture "$RAW_DIFF"
  cm_settle 3.0
  cm_key Page_Down; cm_settle 2.4
  stop_capture

  # Reset through git before the merge/debug beats so the app sees a clean tree.
  GIT reset -q --hard main
  GIT clean -qfd
  cm_send git-refresh
  cm_settle 0.8

  # ── 5. three-way merge: run the real merge and surface conflict tooling.
  term_focus
  run_cmd "git merge feature/stable-sort" 1.4
  cm_send review-conflicts
  cm_settle 1.4
  start_capture "$RAW_MERGE"
  cm_settle 5.6
  stop_capture
  term_focus
  run_cmd "git merge --abort" 0.8
  cm_send git-refresh
  cm_send sidebar-show tree

  # ── 6. debugger: break, run, inspect variables, then step.
  cm_send open src/scheduler.cpp
  cm_send focus editor
  local bpline
  bpline="$(grep -n 'std::printf' "$FIX/src/scheduler.cpp" | head -1 | cut -d: -f1)"
  cm_send breakpoint-set src/scheduler.cpp "$bpline"
  cm_send debug-pane-variables
  cm_send_wait stopped 40 debug-run --type gdb ./taskflow
  cm_park_mouse
  cm_settle 1.4
  start_capture "$RAW_DEBUG"
  cm_settle 3.0
  cm_send debug-step-over; cm_settle 1.8
  cm_send debug-step-over; cm_settle 1.8
  stop_capture
  cm_send debug-stop || true
  cm_settle 0.6

  # ── 7. control channel: an external agent drives the same chokepoint.
  cm_send open src/main.cpp
  cm_send focus editor
  cm_settle 0.5
  cat > "$CM_WORK/.control-demo.sh" <<EOF
#!/usr/bin/env bash
BIN="$CM_BIN"
p(){ printf '\033[38;5;81m➜\033[0m \033[1m%s\033[0m\n' "\$*"; sleep 0.15; }
clear
printf '\033[38;5;245m# external agent driving microide over JSONL\033[0m\n\n'
p 'microide control-send breakpoint-set src/main.cpp 9'; "\$BIN" control-send breakpoint-set src/main.cpp 9
p 'microide control-send --query breakpoints';           "\$BIN" control-send --query breakpoints
EOF
  chmod +x "$CM_WORK/.control-demo.sh"
  term_focus
  start_capture "$RAW_CONTROL"
  run_cmd "bash ~/.control-demo.sh" 4.2
  stop_capture
}

# --- post-processing --------------------------------------------------------
caption_filter() {
  local text="$1"
  printf "scale=1440:900:flags=lanczos,drawtext=text='%s':x=w-tw-36:y=h-th-58:fontsize=32:fontcolor=0xE6EDF3:box=1:boxcolor=0x0D1117DD:boxborderw=16" "$text"
}

hero_filter() {
  local c0 c1 c2 c3 c4 c5 c6
  c0="$(caption_filter "Large file editing with smooth scrolling")"
  c1="$(caption_filter "Project search across the workspace")"
  c2="$(caption_filter "PTY terminal running inside the IDE")"
  c3="$(caption_filter "Git working-tree diff review")"
  c4="$(caption_filter "Three-way merge conflict resolution")"
  c5="$(caption_filter "DAP debugger at a breakpoint")"
  c6="$(caption_filter "JSONL control channel for agents")"
  cat <<EOF
[0:v]trim=duration=4.8,setpts=PTS-STARTPTS,${c0}[v0];
[1:v]trim=duration=4.0,setpts=PTS-STARTPTS,${c1}[v1];
[2:v]trim=duration=4.4,setpts=PTS-STARTPTS,${c2}[v2];
[3:v]trim=duration=5.4,setpts=PTS-STARTPTS,${c3}[v3];
[4:v]trim=duration=5.6,setpts=PTS-STARTPTS,${c4}[v4];
[5:v]trim=duration=6.6,setpts=PTS-STARTPTS,${c5}[v5];
[6:v]trim=duration=5.2,setpts=PTS-STARTPTS,${c6}[v6];
[v0][v1][v2][v3][v4][v5][v6]concat=n=7:v=1:a=0,format=yuv420p[out]
EOF
}

log_clip_durations() {
  command -v ffprobe >/dev/null || return 0
  local name path duration
  for name in editor search terminal diff merge debug control; do
    case "$name" in
      editor) path="$RAW_EDITOR" ;;
      search) path="$RAW_SEARCH" ;;
      terminal) path="$RAW_TERMINAL" ;;
      diff) path="$RAW_DIFF" ;;
      merge) path="$RAW_MERGE" ;;
      debug) path="$RAW_DEBUG" ;;
      control) path="$RAW_CONTROL" ;;
    esac
    duration="$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$path" 2>/dev/null || true)"
    cm_log "hero clip $name: ${duration:-unreadable}s"
  done
}

encode_hero() {
  local filter
  log_clip_durations
  filter="$(hero_filter)"
  ffmpeg -nostdin -loglevel error -y \
    -i "$RAW_EDITOR" -i "$RAW_SEARCH" -i "$RAW_TERMINAL" -i "$RAW_DIFF" \
    -i "$RAW_MERGE" -i "$RAW_DEBUG" -i "$RAW_CONTROL" \
    -filter_complex "$filter" -map "[out]" \
    -codec:v libx264 -preset slow -crf 19 -movflags +faststart \
    -an "$OUT_DIR/hero-demo.mp4"

  ffmpeg -nostdin -loglevel error -y \
    -i "$RAW_EDITOR" -i "$RAW_SEARCH" -i "$RAW_TERMINAL" -i "$RAW_DIFF" \
    -i "$RAW_MERGE" -i "$RAW_DEBUG" -i "$RAW_CONTROL" \
    -filter_complex "$filter" -map "[out]" \
    -codec:v libvpx-vp9 -b:v 0 -crf 30 -row-mt 1 \
    -an "$OUT_DIR/hero-demo.webm"

  ffmpeg -nostdin -loglevel error -y -ss 27.0 -i "$OUT_DIR/hero-demo.mp4" \
    -vframes 1 "$OUT_DIR/hero-poster.png"
}

main() {
  command -v ffmpeg >/dev/null || cm_die "ffmpeg not installed"
  GIT reset -q --hard main 2>/dev/null || true
  add_large_demo_file
  # compile the debuggee
  command -v g++ >/dev/null && g++ -g -O0 -std=c++20 -I"$FIX/include" \
    -o "$FIX/taskflow" "$FIX/src/main.cpp" "$FIX/src/scheduler.cpp"
  # microide must see 'microide' on PATH for the control-channel demo line
  export PATH="$(dirname "$CM_BIN"):$PATH"
  export CM_TYPE_DELAY="${CM_TYPE_DELAY:-25}"

  cm_launch "$FIX"
  choreograph
  cm_kill_instance

  cm_log "encoding deliverables…"
  encode_hero

  cm_log "hero video written to $OUT_DIR (mp4 $(du -h "$OUT_DIR/hero-demo.mp4" | cut -f1), webm $(du -h "$OUT_DIR/hero-demo.webm" | cut -f1))"
}
main
