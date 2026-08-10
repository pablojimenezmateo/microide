#!/usr/bin/env python3
import argparse
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
    for i in range(1, 1201):
        bucket = root / f"subsys_{(i - 1) // 60:02d}"
        bucket.mkdir(parents=True, exist_ok=True)
        ext = ".cc" if i % 3 == 0 else ".h" if i % 3 == 1 else ".md"
        file_path = bucket / f"node_{i:04d}{ext}"
        file_path.write_text(
            f"// kernel fixture {i:04d}\n"
            f"symbol_{i:04d}={i * 17}\n"
            f"group={(i - 1) // 60}\n",
            encoding="utf-8",
        )


def main() -> int:
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", default="tests/perf/fixtures/kernel_sized_project")
    parser.add_argument("--hash-out", default="tests/perf/fixtures/kernel_sized_project.sha256")
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

    specs = [("kernel", Path(args.output), Path(args.hash_out), write_fixture)]
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
