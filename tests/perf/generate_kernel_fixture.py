#!/usr/bin/env python3
import argparse
import hashlib
from pathlib import Path


def write_fixture(root: Path) -> None:
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


def tree_hash(root: Path) -> str:
    digest = hashlib.sha256()
    for path in sorted(p for p in root.rglob("*") if p.is_file()):
        rel = path.relative_to(root).as_posix().encode("utf-8")
        digest.update(rel)
        digest.update(b"\0")
        digest.update(path.read_bytes())
        digest.update(b"\0")
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", default="tests/perf/fixtures/kernel_sized_project")
    parser.add_argument("--hash-out", default="tests/perf/fixtures/kernel_sized_project.sha256")
    args = parser.parse_args()

    out = Path(args.output)
    if out.exists():
        for p in sorted(out.rglob("*"), reverse=True):
            if p.is_file():
                p.unlink()
            elif p.is_dir():
                p.rmdir()
    write_fixture(out)
    h = tree_hash(out)
    Path(args.hash_out).write_text(h + "\n", encoding="utf-8")
    print(h)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
