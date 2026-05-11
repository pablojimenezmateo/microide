#!/usr/bin/env python3
"""Generate deterministic editor-essentials performance fixtures (OpenSpec 13.A.1-13.A.3).

Regenerate after changing generators:
    python3 tests/perf/generate_editor_essentials_perf_fixtures.py --fixture all

C++ body lines suffix `// seedhit perfocc` (first 10k lines) or `// perfocc` so perf scenarios
§13.D.1 / §13.D.2 can use one synthetic_kernel.cpp.
"""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path

TARGET_LINES_CPP = 50_000
TARGET_LINES_PY = 50_000
TARGET_BYTES_1MB = 1 << 20  # 1 MiB
BLOCK_DEPTH = 64  # deep nesting chunks; 4 spaces each => conventional C++/Python indentation.
INDENT_UNIT = "    "
PY_LINE_COMMENT = "# perf fixture"


def wipe_tree(root: Path) -> None:
    if not root.exists():
        return
    for p in sorted(root.rglob("*"), reverse=True):
        if p.is_file():
            p.unlink()
        elif p.is_dir():
            p.rmdir()


def tree_hash(root: Path) -> str:
    digest = hashlib.sha256()
    for path in sorted(p for p in root.rglob("*") if p.is_file()):
        rel = path.relative_to(root).as_posix().encode("utf-8")
        digest.update(rel)
        digest.update(b"\0")
        digest.update(path.read_bytes())
        digest.update(b"\0")
    return digest.hexdigest()


def write_fixture_cpp(root: Path) -> None:
    """50k-line C++-style synthetic buffer: contiguous brace nests, 4-column indents."""
    wipe_tree(root)
    root.mkdir(parents=True, exist_ok=True)
    path = root / "synthetic_kernel.cpp"

    cpp_header = [
        "// Auto-generated fixture for editor_essentials_50k_cpp (OpenSpec 13.A.1).",
        "// Regenerate: python3 tests/perf/generate_editor_essentials_perf_fixtures.py --fixture cpp",
        "",
        "namespace microide::perf::synthetic::editor_essentials {",
        "",
        "inline void fifty_k_deep_brace_cycles() noexcept {",
    ]
    cpp_footer = [
        "}",
        "",
        "}  // namespace microide::perf::synthetic::editor_essentials",
    ]

    body_target = TARGET_LINES_CPP - len(cpp_header) - len(cpp_footer)
    body: list[str] = []
    nested = 0  # unmatched inner `{` pairs inside `fifty_k_deep_brace_cycles`
    serial = 0

    lines_per_cycle = BLOCK_DEPTH * 2 + BLOCK_DEPTH
    cycles = body_target // lines_per_cycle

    for _ in range(cycles):
        for __ in range(BLOCK_DEPTH):
            body.append(f"{INDENT_UNIT * (nested + 1)}{{")
            nested += 1
            body.append(
                f"{INDENT_UNIT * (nested + 1)}volatile unsigned cell_{serial} = __LINE__;",
            )
            serial += 1
        while nested > 0:
            nested -= 1
            body.append(f"{INDENT_UNIT * (nested + 1)}" + "}  // nest unwind")

    for i in range(body_target - len(body)):
        body.append(f"{INDENT_UNIT * (nested + 1)}volatile unsigned filler_{serial + i} = __LINE__;")

    body = body[:body_target]

    # Perf scenarios 13.D.1 / 13.D.2: `perfocc` once per line for occurrence highlight;
    # `seedhit` on the first 10k body lines for add-cursor-at-match corpus density.
    processed_body: list[str] = []
    for i, line in enumerate(body):
        marker = ("seedhit " if i < 10_000 else "") + "perfocc"
        processed_body.append(line.rstrip() + " // " + marker)
    body = processed_body

    lines = [*cpp_header, *body, *cpp_footer]

    if len(lines) != TARGET_LINES_CPP:
        raise RuntimeError(f"cpp line count mismatch: got {len(lines)}, want {TARGET_LINES_CPP}")

    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_fixture_py(root: Path) -> None:
    """50k-line Python-style synthetic buffer: indent-driven blocks (no `{}` wrappers)."""
    wipe_tree(root)
    root.mkdir(parents=True, exist_ok=True)
    path = root / "synthetic_kernel.py"

    py_header = [
        PY_LINE_COMMENT + " 13.A.2 editor_essentials_50k_py (generator output)",
        PY_LINE_COMMENT + " Regenerate: python3 tests/perf/generate_editor_essentials_perf_fixtures.py --fixture py",
        "",
        "def _editor_essentials_kernel():",
    ]
    py_footer: list[str] = []

    body_target = TARGET_LINES_PY - len(py_header) - len(py_footer)
    body: list[str] = []
    nested = 1  # one indent unit deep inside `_editor_essentials_kernel`
    idx = 0

    lines_per_cycle = BLOCK_DEPTH * 2 + BLOCK_DEPTH
    cycles = body_target // lines_per_cycle
    for _ in range(cycles):
        for __ in range(BLOCK_DEPTH):
            body.append(
                f"{INDENT_UNIT * nested}if __name__ != '__skipped___{idx}' or True:"
            )
            nested += 1
            inner = INDENT_UNIT * nested
            body.append(f"{inner}_probe_slot_{idx} = {idx}")
            idx += 1
        while nested > 1:
            nested -= 1
            lead = INDENT_UNIT * nested
            body.append(f"{lead}_close_slot_{idx} = {repr(str(idx))}")
            idx += 1

    filler = body_target - len(body)
    for i in range(filler):
        lead = INDENT_UNIT * nested
        body.append(f"{lead}_pad_cell_{idx + i} = {repr(str(idx + i))}")

    body = body[:body_target]

    lines = [*py_header, *body, *py_footer]

    if len(lines) != TARGET_LINES_PY:
        raise RuntimeError(f"py line count mismatch: got {len(lines)}, want {TARGET_LINES_PY}")

    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_fixture_mixed_1mb(root: Path) -> None:
    """Exactly 1 MiB mixed ASCII: CRLF+LFs, trailing spaces, tabs versus leading spaces."""
    wipe_tree(root)
    root.mkdir(parents=True, exist_ok=True)
    path = root / "mixed_content.txt"
    blob = bytearray(
        b"# Mixed fixture 13.A.3 editor_save_normalization editor_indent_detect_open\r\n"
    )

    for i in range(448):
        newline = b"\r\n" if i % 13 == 0 else b"\n"
        trail_spaces = ("x  " if i % 9 != 0 else "z    ") + ("soft " if i % 4 != 3 else "wide ")
        embedded_tab = "\t" if i % 5 == 2 else ""

        if i % 7 == 0:
            indent = "\t" * (1 + (i % 5))
            line_txt = indent + f"tab_leader_{i:08d}" + trail_spaces
        elif i % 6 == 0:
            base = INDENT_UNIT * ((((i % 9) + 1)))
            step_pad = INDENT_UNIT * ((((i % 4) + 2)))
            line_txt = base + step_pad + f"space_indent_{i:08d}" + embedded_tab + trail_spaces
        else:
            line_txt = INDENT_UNIT * ((((i % 8) + 1))) + f"plain_indent_{i:08d}" + embedded_tab + trail_spaces

        blob.extend(line_txt.encode("ascii") + newline)

    filler = INDENT_UNIT * 4 + "bulk_expand_line mixed_space\tmid\talt\ttrail_payload "

    seq = 0
    while len(blob) < TARGET_BYTES_1MB:
        ln = filler + f"{seq:010d}_" + ("y  " if seq % 3 else "z    ")
        blob.extend(ln.encode("ascii"))
        blob.extend(b"\n" if seq % 17 != 0 else b"\r\n")
        seq += 1

    if len(blob) > TARGET_BYTES_1MB:
        del blob[TARGET_BYTES_1MB:]
    if len(blob) < TARGET_BYTES_1MB:
        blob.extend(b"@" * (TARGET_BYTES_1MB - len(blob)))
    path.write_bytes(bytes(blob))
def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--fixture",
        choices=("cpp", "py", "mb", "all"),
        default="all",
        help="Which fixture subtree to regenerate (default: all).",
    )
    parser.add_argument(
        "--output-cpp",
        default="tests/perf/fixtures/editor_essentials_50k_cpp",
    )
    parser.add_argument(
        "--output-py",
        default="tests/perf/fixtures/editor_essentials_50k_py",
    )
    parser.add_argument(
        "--output-1mb",
        default="tests/perf/fixtures/editor_essentials_1mb",
    )
    parser.add_argument("--hash-cpp", default="tests/perf/fixtures/editor_essentials_50k_cpp.sha256")
    parser.add_argument("--hash-py", default="tests/perf/fixtures/editor_essentials_50k_py.sha256")
    parser.add_argument("--hash-1mb", default="tests/perf/fixtures/editor_essentials_1mb.sha256")
    args = parser.parse_args()

    cwd = Path.cwd()
    specs: list[tuple[str, Path, Path, callable]] = []
    if args.fixture in ("cpp", "all"):
        specs.append(("cpp", Path(args.output_cpp), Path(args.hash_cpp), write_fixture_cpp))
    if args.fixture in ("py", "all"):
        specs.append(("py", Path(args.output_py), Path(args.hash_py), write_fixture_py))
    if args.fixture in ("mb", "all"):
        specs.append(("1mb", Path(args.output_1mb), Path(args.hash_1mb), write_fixture_mixed_1mb))

    for name, root, hash_out, writer in specs:
        root_abs = cwd / root
        writer(root_abs)
        h = tree_hash(root_abs)
        ho = cwd / hash_out
        ho.parent.mkdir(parents=True, exist_ok=True)
        ho.write_text(h + "\n", encoding="utf-8")
        print(f"[{name}] wrote {root} sha256={h}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
