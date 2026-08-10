#!/usr/bin/env bash
# Generate deterministic Git workstation perf fixtures (fixed layout; seed 1337 in file content).
# Usage: bash tests/perf/generate_git_workstation_fixtures.sh
set -euo pipefail

ROOT="${1:-tests/perf/fixtures}"

git_init_repo() {
  local dir="$1"
  rm -rf "$dir"
  mkdir -p "$dir"
  git -C "$dir" init -q
  git -C "$dir" config user.email "fixture@microide"
  git -C "$dir" config user.name "Fixture"
}

write_tracked_files() {
  local dir="$1"
  local count="$2"
  local prefix="$3"
  for i in $(seq -w 1 "$count"); do
    local bucket="${prefix}_$(( (10#$i - 1) / 50 ))"
    mkdir -p "$dir/$bucket"
    printf '// fixture %s file_%s\nsymbol_%s=%d\n' "$prefix" "$i" "$i" "$((10#$i * 7))" \
      > "$dir/$bucket/file_${i}.cpp"
  done
}

write_untracked_files() {
  local dir="$1"
  local count="$2"
  local prefix="$3"
  for i in $(seq -w 1 "$count"); do
    local bucket="untracked_${prefix}_$(( (10#$i - 1) / 50 ))"
    mkdir -p "$dir/$bucket"
    printf '// untracked %s file_%s\n' "$prefix" "$i" > "$dir/$bucket/file_${i}.txt"
  done
}

# Modest tracked status set with a working-tree delta, for `git_sidebar_activate`
# — the scenario that measures activating the Source Control view and running its
# first `git status`. This tree is what that scenario has always named, and this
# script never produced it: the fixture was simply absent, and the scenario's
# fixture guard skipped QUIETLY while its baseline sat in the committed set and
# got differenced across releases. It only surfaced once a skip started failing
# the run (TD-2026-08-10-170). Note the comment three blocks down that already
# refers to "the same layout as git_status_project".
status_project="${ROOT}/git_status_project"
git_init_repo "$status_project"
write_tracked_files "$status_project" 200 "status"
git -C "$status_project" add -A
git -C "$status_project" commit -q -m "initial: 200 tracked files"
# `seq -w 1 200 | head -n 40`, not `seq -w 1 40`: -w pads to the width of the
# LARGEST value, so the short form writes file_01.cpp while the tracked files are
# file_001.cpp — 40 new untracked files instead of 40 modified tracked ones, and
# `git status` reports the opposite of what this fixture is for.
for i in $(seq -w 1 200 | head -n 40); do
  bucket="status_$(( (10#$i - 1) / 50 ))"
  printf '\n// working-tree delta %s\n' "$i" >> "$status_project/$bucket/file_${i}.cpp"
done
write_untracked_files "$status_project" 10 "status"
echo "wrote $status_project"

# Large tracked status set (5 000 files).
large_status="${ROOT}/git_large_status_project"
git_init_repo "$large_status"
write_tracked_files "$large_status" 5000 "large_status"
git -C "$large_status" add -A
git -C "$large_status" commit -q -m "initial: 5000 tracked files"
echo "wrote $large_status"

# Many untracked files (1 000 tracked + 1 500 untracked).
many_untracked="${ROOT}/git_many_untracked_project"
git_init_repo "$many_untracked"
write_tracked_files "$many_untracked" 1000 "tracked"
git -C "$many_untracked" add -A
git -C "$many_untracked" commit -q -m "initial: 1000 tracked files"
write_untracked_files "$many_untracked" 1500 "extra"
echo "wrote $many_untracked"

# One thousand modified tracked files (same layout as git_status_project).
changed="${ROOT}/git_1000_changed_project"
git_init_repo "$changed"
write_tracked_files "$changed" 1000 "changed"
git -C "$changed" add -A
git -C "$changed" commit -q -m "initial: 1000 tracked files"
for i in $(seq -w 1 1000); do
  bucket="changed_$(( (10#$i - 1) / 50 ))"
  printf '\n// working-tree delta %s\n' "$i" >> "$changed/$bucket/file_${i}.cpp"
done
echo "wrote $changed"

# Large single-file text diff.
large_diff="${ROOT}/git_large_diff_project"
git_init_repo "$large_diff"
mkdir -p "$large_diff/src"
{
  echo '// large diff fixture seed=1337'
  for line in $(seq 1 12000); do
    printf 'line_%05d = %d;\n' "$line" "$((line * 3))"
  done
} > "$large_diff/src/large.cpp"
git -C "$large_diff" add -A
git -C "$large_diff" commit -q -m "add large.cpp"
printf '\n// worktree tail\nWORKTREE_DELTA=1337\n' >> "$large_diff/src/large.cpp"
echo "wrote $large_diff"

# Large staged set (800 modified files staged in the index).
large_staged="${ROOT}/git_large_staged_project"
git_init_repo "$large_staged"
write_tracked_files "$large_staged" 800 "staged"
git -C "$large_staged" add -A
git -C "$large_staged" commit -q -m "initial: 800 tracked files"
for i in $(seq -w 1 800); do
  bucket="staged_$(( (10#$i - 1) / 50 ))"
  printf '\n// staged delta %s\n' "$i" >> "$large_staged/$bucket/file_${i}.cpp"
done
git -C "$large_staged" add -A
echo "wrote $large_staged"

# Many merge conflicts (three-way inputs with interleaved hunks).
many_conflicts="${ROOT}/git_many_conflicts_project"
git_init_repo "$many_conflicts"
mkdir -p "$many_conflicts"
{
  for block in $(seq 0 419); do
    echo "void unit_${block}() {"
    echo "  int value_${block} = ${block};"
    echo "  sink(value_${block});"
    echo "}"
    echo
  done
} > "$many_conflicts/base.cpp"
{
  for block in $(seq 0 419); do
    echo "void unit_${block}() {"
    if (( block % 3 == 0 )); then
      echo "  int value_${block} = $((block + 1000));"
    else
      echo "  int value_${block} = ${block};"
    fi
    echo "  sink(value_${block});"
    echo "}"
    echo
  done
} > "$many_conflicts/incoming.cpp"
{
  for block in $(seq 0 419); do
    echo "void unit_${block}() {"
    if (( block % 3 == 1 )); then
      echo "  int value_${block} = $((block + 2000));"
    else
      echo "  int value_${block} = ${block};"
    fi
    echo "  sink(value_${block});"
    echo "}"
    echo
  done
} > "$many_conflicts/current.cpp"
git -C "$many_conflicts" add base.cpp incoming.cpp current.cpp
git -C "$many_conflicts" commit -q -m "add merge conflict inputs"
echo "wrote $many_conflicts"

echo "Git workstation perf fixtures complete under $ROOT"
