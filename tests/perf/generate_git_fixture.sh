#!/usr/bin/env bash
# Generate the git_status_project perf fixture: a git working tree with 1,000 tracked files.
# Usage: bash generate_git_fixture.sh [output_dir]
set -euo pipefail

OUTPUT="${1:-tests/perf/fixtures/git_status_project}"

rm -rf "$OUTPUT"
mkdir -p "$OUTPUT"

git -C "$OUTPUT" init -q
git -C "$OUTPUT" config user.email "fixture@microide"
git -C "$OUTPUT" config user.name "Fixture"

for i in $(seq -w 1 1000); do
    bucket="src_$(( (10#$i - 1) / 50 ))"
    mkdir -p "$OUTPUT/$bucket"
    printf '// git_status_project fixture %s\nsymbol_%s=%d\n' "$i" "$i" "$((10#$i * 7))" \
        > "$OUTPUT/$bucket/file_$i.cpp"
done

git -C "$OUTPUT" add -A
git -C "$OUTPUT" commit -q -m "initial: 1000 tracked files"
echo "git_status_project fixture written to $OUTPUT"
