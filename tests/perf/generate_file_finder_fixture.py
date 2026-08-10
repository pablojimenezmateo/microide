#!/usr/bin/env python3
"""Generate the file_finder_large perf fixture: 10,000 synthetic files in a flat project tree."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
# One `--ensure` implementation for every perf fixture, not three: the
# committed .sha256 is the contract, a matching tree is left alone, and a
# regeneration that does not reproduce the hash aborts (TD-2026-08-10-170).
from generate_editor_essentials_perf_fixtures import (  # noqa: E402
    ensure_fixtures,
    tree_hash,
    wipe_tree,
)


def write_fixture(root: Path) -> None:
    wipe_tree(root)
    root.mkdir(parents=True, exist_ok=True)
    for i in range(1, 10001):
        bucket = root / f"src_{(i - 1) // 200:02d}"
        bucket.mkdir(parents=True, exist_ok=True)
        ext = ".cpp" if i % 3 == 0 else ".h" if i % 3 == 1 else ".md"
        file_path = bucket / f"file_{i:05d}{ext}"
        file_path.write_text(
            f"// file_finder_large fixture {i:05d}\n"
            f"symbol_{i:05d}={i * 13}\n"
            f"group={(i - 1) // 200}\n",
            encoding="utf-8",
        )


def main() -> int:
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", default="tests/perf/fixtures/file_finder_large")
    parser.add_argument("--hash-out", default="tests/perf/fixtures/file_finder_large.sha256")
    parser.add_argument(
        "--ensure",
        action="store_true",
        help=(
            "Idempotent generate-on-demand: regenerate only when the tree is missing "
            "or stale versus the committed .sha256, and treat that .sha256 as "
            "authoritative (do not overwrite it)."
        ),
    )
    args = parser.parse_args()

    specs = [("file_finder", Path(args.output), Path(args.hash_out), write_fixture)]
    if args.ensure:
        return ensure_fixtures(Path.cwd(), specs)

    out = Path(args.output)
    write_fixture(out)
    h = tree_hash(out)
    Path(args.hash_out).write_text(h + "\n", encoding="utf-8")
    print(h)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
