#!/usr/bin/env bash
#
# make-fixture.sh — build the deterministic demo project used by the capture
# pipeline. Creates a small, attractive C++ git repo with pinned author/dates
# (so blame/log/diffs are byte-stable), a compiled -g binary for the debugger
# scene, and a divergent branch engineered to conflict for the merge scene.
#
# Usage: make-fixture.sh <dest-dir>
# The working tree is left CLEAN on the default branch; capture-shots.sh applies
# the per-scene git state (working-tree edit for the diff, merge for conflicts).

set -euo pipefail

DEST="${1:?usage: make-fixture.sh <dest-dir>}"
AUTHOR_NAME="${CM_FIXTURE_AUTHOR:-Ada Lovelace}"
AUTHOR_EMAIL="${CM_FIXTURE_EMAIL:-ada@example.com}"
D0="${CM_FIXTURE_DATE0:-2026-05-04T09:15:00}"
D1="${CM_FIXTURE_DATE1:-2026-05-21T16:40:00}"
D2="${CM_FIXTURE_DATE2:-2026-06-09T11:05:00}"

rm -rf "$DEST"
mkdir -p "$DEST/include" "$DEST/src"
cd "$DEST"

git init -q -b main
git config user.name  "$AUTHOR_NAME"
git config user.email "$AUTHOR_EMAIL"
git config commit.gpgsign false
git config advice.detachedHead false

commit() { # commit <iso-date> <message>
  GIT_AUTHOR_DATE="$1" GIT_COMMITTER_DATE="$1" \
  GIT_AUTHOR_NAME="$AUTHOR_NAME" GIT_AUTHOR_EMAIL="$AUTHOR_EMAIL" \
  GIT_COMMITTER_NAME="$AUTHOR_NAME" GIT_COMMITTER_EMAIL="$AUTHOR_EMAIL" \
    git commit -q -m "$2"
}

# ---- commit 1: project skeleton ------------------------------------------
cat > .gitignore <<'EOF'
/taskflow
/build/
EOF

cat > README.md <<'EOF'
# taskflow

A tiny, dependency-free task scheduler — the demo project that ships in the
microide showcase. It runs a fixed batch of jobs by priority and prints a
short trace, which makes it a clean target for the debugger walkthrough.

    cmake -S . -B build && cmake --build build
    ./build/taskflow
EOF

cat > include/scheduler.hpp <<'EOF'
#pragma once
#include <string>
#include <vector>

namespace taskflow {

// A unit of work with a name and a scheduling priority (higher runs first).
struct Task {
    std::string name;
    int priority = 0;
    int cost = 1;
};

// Orders tasks by descending priority and returns the total cost executed.
class Scheduler {
public:
    void add(std::string name, int priority, int cost);
    int  run();                         // returns total cost of all tasks
    const std::vector<Task>& queue() const { return queue_; }

private:
    std::vector<Task> queue_;
};

}  // namespace taskflow
EOF

cat > src/scheduler.cpp <<'EOF'
#include "scheduler.hpp"

#include <algorithm>
#include <cstdio>

namespace taskflow {

void Scheduler::add(std::string name, int priority, int cost) {
    queue_.push_back(Task{std::move(name), priority, cost});
}

int Scheduler::run() {
    std::sort(queue_.begin(), queue_.end(),
              [](const Task& a, const Task& b) { return a.priority > b.priority; });

    int total = 0;
    for (const Task& task : queue_) {
        total += task.cost;
        std::printf("run %-8s p=%d cost=%d  (total=%d)\n",
                    task.name.c_str(), task.priority, task.cost, total);
    }
    return total;
}

}  // namespace taskflow
EOF

cat > src/main.cpp <<'EOF'
#include "scheduler.hpp"

#include <cstdio>

int main() {
    taskflow::Scheduler scheduler;
    scheduler.add("ingest",  5, 3);
    scheduler.add("index",   8, 4);
    scheduler.add("compact", 2, 2);
    scheduler.add("flush",   9, 1);

    const int total = scheduler.run();
    std::printf("scheduled %zu tasks, total cost %d\n",
                scheduler.queue().size(), total);
    return total;
}
EOF

cat > CMakeLists.txt <<'EOF'
cmake_minimum_required(VERSION 3.16)
project(taskflow LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
add_executable(taskflow src/main.cpp src/scheduler.cpp)
target_include_directories(taskflow PRIVATE include)
EOF

git add -A
commit "$D0" "Add taskflow scheduler skeleton"

# ---- commit 2: a follow-up so blame/history has depth --------------------
cat >> README.md <<'EOF'

## Priorities

Jobs run highest-priority first; ties keep insertion order. The trace prints a
running cost total so you can watch the schedule unfold.
EOF
git add -A
commit "$D1" "Document priority ordering"

# ---- divergence: both branches rewrite the SAME trace block --------------
# The two branches replace the identical printf lines with different formats, so
# the merge genuinely overlaps and cannot auto-resolve — microide surfaces a real
# conflict region (not a silently auto-merged hunk).
BASE_TRACE='        std::printf("run %-8s p=%d cost=%d  (total=%d)\n",
                    task.name.c_str(), task.priority, task.cost, total);'

retrace() { # retrace <file> <replacement>
  python3 - "$1" "$BASE_TRACE" "$2" <<'PY'
import sys, pathlib
p = pathlib.Path(sys.argv[1]); s = p.read_text()
assert sys.argv[2] in s, "base trace block not found"
p.write_text(s.replace(sys.argv[2], sys.argv[3]))
PY
}

# feature branch: a "dispatch" bullet trace.
git switch -q -c feature/stable-sort
retrace src/scheduler.cpp '        std::printf("  -> dispatch %-8s  (priority %d, cost %d)  running total %d\n",
                    task.name.c_str(), task.priority, task.cost, total);'
git add -A
commit "$D2" "Reword the run trace as a dispatch log"

# main: a compact "[i/n]" progress trace on the very same lines.
git switch -q main
applied_d2="${CM_FIXTURE_DATE2B:-2026-06-12T14:20:00}"
retrace src/scheduler.cpp '        std::printf("[%d/%zu] %-8s  p=%d  cost=%d  total=%d\n",
                    ++ran, queue_.size(), task.name.c_str(), task.priority, task.cost, total);'
# the [i/n] format needs a counter; declare it just above the loop
sed -i 's#    int total = 0;#    int total = 0;\n    int ran = 0;#' src/scheduler.cpp
git add -A
commit "$applied_d2" "Switch the run trace to an [i/total] progress format"

# ---- compile the debuggee with symbols -----------------------------------
if command -v g++ >/dev/null; then
  g++ -g -O0 -std=c++20 -Iinclude -o taskflow src/main.cpp src/scheduler.cpp
fi

git switch -q main
echo "fixture ready: $DEST (branch main, clean tree, binary $( [[ -x taskflow ]] && echo built || echo SKIPPED ))"
