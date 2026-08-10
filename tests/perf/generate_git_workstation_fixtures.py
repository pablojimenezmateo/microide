#!/usr/bin/env python3
"""Generate the deterministic Git workstation perf fixtures (seven repositories).

    python3 tests/perf/generate_git_workstation_fixtures.py            # rewrite all + .sha256
    python3 tests/perf/generate_git_workstation_fixtures.py --ensure   # ctest setup form

This replaces `generate_git_workstation_fixtures.sh`, which was the one fixture
family with no contract and no ctest wiring (TD-2026-08-10-172): a human had to
remember to run it, a fresh checkout could not run a single git-workstation
scenario, and nothing detected a fixture tree that had drifted. Two things had
already drifted unnoticed on a long-lived checkout:

  - `git_status_project` was never produced at all, so `git_sidebar_activate`
    skipped quietly for long enough that nobody could say when it stopped.
  - `SimulateExternalFileChange` had appended to `git_large_diff_project` and
    `git_many_conflicts_project` hundreds of times before TD-2026-08-06-155
    taught the harness to truncate back, and the residue stayed: a 3-line
    worktree delta was still a 263-line one here.

The contract is therefore not a plain content hash. `git_hash` digests the
worktree (skipping `.git`, whose object/index bytes are not reproducible) and
then folds in `git status --porcelain`, so **index** state is part of the
contract too. That is what closes the still-open half of TD-2026-08-06-155: the
staging scenarios run `git add` against these repositories, and a left-behind
index now invalidates the manifest and gets regenerated on the next `--ensure`
instead of silently becoming the next run's starting state.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
# One `--ensure` implementation for every perf fixture, not four.
from generate_editor_essentials_perf_fixtures import ensure_fixtures  # noqa: E402

FIXTURE_ROOT = Path("tests/perf/fixtures")

# Pinned so the repositories are a function of this script and nothing else. The
# `-c` overrides matter more than the dates: a developer's global gitconfig can
# change what `git status` reports (core.excludesFile), what lands in the
# worktree (core.autocrlf), and whether `git commit` even succeeds
# (commit.gpgsign), and every one of those would show up as manifest drift on
# one machine and not another.
GIT_CONFIG_ARGS = [
    "-c", "core.excludesFile=/dev/null",
    "-c", "core.attributesFile=/dev/null",
    "-c", "core.autocrlf=false",
    "-c", "core.safecrlf=false",
    "-c", "core.hooksPath=/dev/null",
    "-c", "commit.gpgsign=false",
    "-c", "gc.auto=0",
    "-c", "init.defaultBranch=main",
    "-c", "user.name=Fixture",
    "-c", "user.email=fixture@microide",
]
GIT_ENV = {
    **os.environ,
    "GIT_AUTHOR_DATE": "2020-01-01T00:00:00+0000",
    "GIT_COMMITTER_DATE": "2020-01-01T00:00:00+0000",
    "GIT_CONFIG_NOSYSTEM": "1",
    "HOME": "/nonexistent-microide-fixture-home",
    "XDG_CONFIG_HOME": "/nonexistent-microide-fixture-home",
}


def git(repo: Path, *args: str) -> bytes:
    result = subprocess.run(
        ["git", "-C", str(repo), *GIT_CONFIG_ARGS, *args],
        env=GIT_ENV,
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    return result.stdout


def git_hash(root: Path) -> str:
    """Manifest digest for a fixture repository: worktree bytes plus index state.

    Worktree files use the same `<relative posix path> NUL <bytes> NUL` scheme as
    `tree_hash`, so the two contracts read the same way; `.git` is skipped because
    loose-object layout, pack ordering and index stat data are not reproducible.
    `git status --porcelain` is folded in last so a scenario that leaves a file
    staged invalidates the manifest — the worktree bytes alone cannot see that.
    """
    digest = hashlib.sha256()
    files = sorted(
        p for p in root.rglob("*")
        if p.is_file() and ".git" not in p.relative_to(root).parts
    )
    for path in files:
        digest.update(path.relative_to(root).as_posix().encode("utf-8"))
        digest.update(b"\0")
        digest.update(path.read_bytes())
        digest.update(b"\0")
    digest.update(b"git-status\0")
    digest.update(git(root, "status", "--porcelain=v1", "-uall", "-z"))
    return digest.hexdigest()


def init_repo(root: Path) -> None:
    shutil.rmtree(root, ignore_errors=True)
    root.mkdir(parents=True, exist_ok=True)
    git(root, "init", "-q")


def width(count: int) -> int:
    """`seq -w`'s padding: the width of the LARGEST value in the range.

    The shell script this replaces carried a comment about getting this wrong once
    — `seq -w 1 40` writes file_01 while `seq -w 1 200 | head -n 40` writes
    file_001, so the short form produced 40 untracked files where the fixture
    wanted 40 modified tracked ones and `git status` reported the opposite of
    what the fixture is for.
    """
    return len(str(count))


def write_tracked_files(root: Path, count: int, prefix: str) -> None:
    pad = width(count)
    for i in range(1, count + 1):
        bucket = root / f"{prefix}_{(i - 1) // 50}"
        bucket.mkdir(parents=True, exist_ok=True)
        (bucket / f"file_{i:0{pad}d}.cpp").write_text(
            f"// fixture {prefix} file_{i:0{pad}d}\nsymbol_{i:0{pad}d}={i * 7}\n",
            encoding="utf-8",
        )


def write_untracked_files(root: Path, count: int, prefix: str) -> None:
    pad = width(count)
    for i in range(1, count + 1):
        bucket = root / f"untracked_{prefix}_{(i - 1) // 50}"
        bucket.mkdir(parents=True, exist_ok=True)
        (bucket / f"file_{i:0{pad}d}.txt").write_text(
            f"// untracked {prefix} file_{i:0{pad}d}\n", encoding="utf-8"
        )


def append_worktree_delta(root: Path, count: int, prefix: str, marker: str, limit: int) -> None:
    pad = width(count)
    for i in range(1, limit + 1):
        bucket = root / f"{prefix}_{(i - 1) // 50}"
        with (bucket / f"file_{i:0{pad}d}.cpp").open("a", encoding="utf-8") as out:
            out.write(f"\n// {marker} {i:0{pad}d}\n")


def write_status_project(root: Path) -> None:
    """200 tracked files, 40 modified, 10 untracked — `git_sidebar_activate`'s tree."""
    init_repo(root)
    write_tracked_files(root, 200, "status")
    git(root, "add", "-A")
    git(root, "commit", "-q", "-m", "initial: 200 tracked files")
    append_worktree_delta(root, 200, "status", "working-tree delta", 40)
    write_untracked_files(root, 10, "status")


def write_large_status_project(root: Path) -> None:
    init_repo(root)
    write_tracked_files(root, 5000, "large_status")
    git(root, "add", "-A")
    git(root, "commit", "-q", "-m", "initial: 5000 tracked files")


def write_many_untracked_project(root: Path) -> None:
    init_repo(root)
    write_tracked_files(root, 1000, "tracked")
    git(root, "add", "-A")
    git(root, "commit", "-q", "-m", "initial: 1000 tracked files")
    write_untracked_files(root, 1500, "extra")


def write_1000_changed_project(root: Path) -> None:
    init_repo(root)
    write_tracked_files(root, 1000, "changed")
    git(root, "add", "-A")
    git(root, "commit", "-q", "-m", "initial: 1000 tracked files")
    append_worktree_delta(root, 1000, "changed", "working-tree delta", 1000)


def write_large_diff_project(root: Path) -> None:
    init_repo(root)
    (root / "src").mkdir(parents=True, exist_ok=True)
    body = ["// large diff fixture seed=1337"]
    body += [f"line_{line:05d} = {line * 3};" for line in range(1, 12001)]
    large = root / "src" / "large.cpp"
    large.write_text("\n".join(body) + "\n", encoding="utf-8")
    git(root, "add", "-A")
    git(root, "commit", "-q", "-m", "add large.cpp")
    with large.open("a", encoding="utf-8") as out:
        out.write("\n// worktree tail\nWORKTREE_DELTA=1337\n")


def write_large_staged_project(root: Path) -> None:
    init_repo(root)
    write_tracked_files(root, 800, "staged")
    git(root, "add", "-A")
    git(root, "commit", "-q", "-m", "initial: 800 tracked files")
    append_worktree_delta(root, 800, "staged", "staged delta", 800)
    git(root, "add", "-A")


def write_many_conflicts_project(root: Path) -> None:
    init_repo(root)

    def blocks(bumped_remainder: int, bump: int) -> str:
        out = []
        for block in range(420):
            value = block + bump if block % 3 == bumped_remainder else block
            out.append(f"void unit_{block}() {{")
            out.append(f"  int value_{block} = {value};")
            out.append(f"  sink(value_{block});")
            out.append("}")
            out.append("")
        return "\n".join(out) + "\n"

    (root / "base.cpp").write_text(blocks(-1, 0), encoding="utf-8")
    (root / "incoming.cpp").write_text(blocks(0, 1000), encoding="utf-8")
    (root / "current.cpp").write_text(blocks(1, 2000), encoding="utf-8")
    git(root, "add", "base.cpp", "incoming.cpp", "current.cpp")
    git(root, "commit", "-q", "-m", "add merge conflict inputs")


FIXTURES = [
    ("git_status_project", write_status_project),
    ("git_large_status_project", write_large_status_project),
    ("git_many_untracked_project", write_many_untracked_project),
    ("git_1000_changed_project", write_1000_changed_project),
    ("git_large_diff_project", write_large_diff_project),
    ("git_large_staged_project", write_large_staged_project),
    ("git_many_conflicts_project", write_many_conflicts_project),
]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=str(FIXTURE_ROOT))
    parser.add_argument(
        "--ensure",
        action="store_true",
        help=(
            "Idempotent generate-on-demand: regenerate only the repositories that are "
            "missing or stale versus their committed .sha256, and treat that .sha256 as "
            "authoritative (do not overwrite it)."
        ),
    )
    parser.add_argument("--fixture", default="all", help="one fixture name, or 'all'")
    args = parser.parse_args()

    root = Path(args.root)
    selected = [f for f in FIXTURES if args.fixture in ("all", f[0])]
    if not selected:
        print(f"unknown fixture {args.fixture!r}; known: {', '.join(n for n, _ in FIXTURES)}")
        return 2

    specs = [
        (name, root / name, root / f"{name}.sha256", writer, git_hash)
        for name, writer in selected
    ]
    if args.ensure:
        return ensure_fixtures(Path.cwd(), specs)

    for name, tree, hash_out, writer, hasher in specs:
        writer(tree)
        digest = hasher(tree)
        hash_out.write_text(digest + "\n", encoding="utf-8")
        print(f"[{name}] wrote {tree} sha256={digest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
