#!/usr/bin/env python3
"""Delta-debug a monkey replay down to the shortest action list that still
reproduces a finding (safety-plan W1.5).

    tools/soak/minimize.py REPLAY.json --signature SIG [--binary PATH] [--jobs N]
                           [--out DIR] [--max-tests N] [--time-budget S]

`REPLAY.json` is the monkey's replay record (the actions up to the finding). A
candidate reproduces when replaying it on a fresh fixture shows a finding with
signature SIG (findings.signature). Zeller's ddmin, with the complements of one
granularity tested in parallel on separate displays; then a last pass drops
single actions. Writes `minimal.json` (a replay record) and prints its length.

Also `--check-only`: replays the record `--runs` times and prints the
reproduction rate, which is how the soak runner decides deterministic vs flaky.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import sys
import tempfile
import time
from pathlib import Path

QA = Path(__file__).resolve().parents[1] / "qa"
sys.path.insert(0, str(QA))
import microide_qa as q  # noqa: E402
import run as qa_run  # noqa: E402

# Rows that are not actions of their own: replay skips them anyway.
_PASSIVE = ("check", "note", "type-via-control", "corpus")


def actions_of(record: dict) -> list[dict]:
    return [a for a in record.get("actions", []) if not any(key in a for key in _PASSIVE)]


class Tester:
    def __init__(self, record: dict, signature: str, binary: Path, scratch: Path, jobs: int):
        self.record = record
        self.signature = signature
        self.binary = binary
        self.scratch = scratch
        self.jobs = max(1, jobs)
        self.tests = 0
        self.cache: dict[tuple[int, ...], bool] = {}
        # Whether every candidate runs the App's per-action checks. A log, crash
        # or data finding does not need them -- the redraw verifier and the
        # sanitizers write the log, which one check at the end reads, and the data
        # oracle runs at the end -- and they are most of a replay's time.
        self.check_each = True
        self.kind = ""

    def learn_kind(self, signatures: list[dict]) -> None:
        for entry in signatures:
            if entry.get("signature") == self.signature:
                self.kind = entry.get("kind", "")

    def try_skipping_checks(self, actions: list[dict]) -> None:
        """Candidates may skip the per-action checks for a log, crash or data
        finding -- but only once a full replay without them still reproduces:
        the checks are also time between actions, and a timing-sensitive finding
        replayed without them never showed, so ddmin reduced nothing in 323
        tests (seed 4, 2026-11-08)."""
        if self.kind not in ("log", "crash", "data"):
            return
        self.check_each = False
        if not self._run(actions):
            self.check_each = True
            print("  the finding needs the per-action checks' timing; keeping them", flush=True)

    def _run(self, actions: list[dict]) -> bool:
        out = Path(tempfile.mkdtemp(prefix="t-", dir=self.scratch))
        try:
            app, signatures = qa_run.replay_record(self.record, self.binary, out, actions=actions,
                                                   check=self.check_each)
            if not self.kind:
                self.learn_kind(signatures)
            return any(entry.get("signature") == self.signature for entry in signatures)
        except Exception as error:  # noqa: BLE001 - a candidate that breaks the driver does not reproduce
            print(f"    candidate failed to run: {type(error).__name__}: {error}", flush=True)
            return False
        finally:
            import shutil
            shutil.rmtree(out, ignore_errors=True)

    def test_many(self, candidates: list[list[int]], indices_to_actions) -> list[bool]:
        todo = [c for c in candidates if tuple(c) not in self.cache]
        if todo:
            with concurrent.futures.ThreadPoolExecutor(max_workers=self.jobs) as pool:
                results = list(pool.map(lambda c: self._run(indices_to_actions(c)), todo))
            for candidate, result in zip(todo, results):
                self.cache[tuple(candidate)] = result
            self.tests += len(todo)
        return [self.cache[tuple(c)] for c in candidates]


def ddmin(tester: Tester, actions: list[dict], max_tests: int, deadline: float) -> list[int]:
    def to_actions(indices: list[int]) -> list[dict]:
        return [actions[i] for i in indices]

    current = list(range(len(actions)))
    n = 2
    while len(current) >= 2 and tester.tests < max_tests and time.monotonic() < deadline:
        size = len(current)
        chunk = max(1, size // n)
        subsets = [current[i:i + chunk] for i in range(0, size, chunk)]
        complements = [[i for i in current if i not in set(subset)] for subset in subsets]
        # Complements first (they shrink fastest when they hold), then subsets.
        results = tester.test_many(complements, to_actions)
        reduced = next((c for c, ok in zip(complements, results) if ok and c), None)
        if reduced is not None:
            current = reduced
            n = max(n - 1, 2)
            print(f"  ddmin: {len(current)} actions ({tester.tests} tests)", flush=True)
            continue
        if n < size:
            results = tester.test_many(subsets, to_actions)
            reduced = next((s for s, ok in zip(subsets, results) if ok), None)
            if reduced is not None:
                current = reduced
                n = 2
                print(f"  ddmin: {len(current)} actions ({tester.tests} tests)", flush=True)
                continue
        if n >= size:
            break
        n = min(size, n * 2)
    # One-minimality: try dropping each remaining action alone.
    changed = True
    while changed and len(current) > 1 and tester.tests < max_tests and time.monotonic() < deadline:
        changed = False
        candidates = [[i for i in current if i != drop] for drop in current]
        results = tester.test_many(candidates, to_actions)
        for candidate, ok in zip(candidates, results):
            if ok:
                current = candidate
                changed = True
                print(f"  one-minimal pass: {len(current)} actions", flush=True)
                break
    return current


def reproduction_rate(record: dict, signature: str, binary: Path, scratch: Path, runs: int, jobs: int) -> float:
    tester = Tester(record, signature, binary, scratch, jobs)
    actions = actions_of(record)
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, min(jobs, runs))) as pool:
        hits = list(pool.map(lambda _: tester._run(actions), range(runs)))
    return sum(hits) / max(1, runs)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("replay", type=Path)
    parser.add_argument("--signature", required=True,
                        help="the finding's signature, or `auto`: the last one a full replay shows")
    parser.add_argument("--binary", type=Path, default=q.REPO / "build/microide/microide")
    parser.add_argument("--jobs", type=int, default=6)
    parser.add_argument("--out", type=Path, help="directory for minimal.json (default: beside the replay)")
    parser.add_argument("--max-tests", type=int, default=150)
    parser.add_argument("--time-budget", type=float, default=3600)
    parser.add_argument("--check-only", action="store_true", help="print the reproduction rate and exit")
    parser.add_argument("--runs", type=int, default=3)
    args = parser.parse_args()
    record = json.loads(args.replay.read_text())
    out = args.out or args.replay.parent
    out.mkdir(parents=True, exist_ok=True)
    scratch = Path(tempfile.mkdtemp(prefix="minimize-", dir=out))
    # A private copy of the binary: a rebuild of the tree it came from replaces
    # it mid-run, and every candidate after that "did not reproduce" for want of
    # a binary to run (2026-11-08).
    import shutil
    pinned = scratch / "microide"
    shutil.copy2(args.binary, pinned)
    args.binary = pinned
    try:
        actions = actions_of(record)
        if args.signature == "auto":
            # The replay record ends at its finding: the last signature a full
            # replay shows is the one to keep. Resolved before --check-only too,
            # which compared every replay against the literal "auto" and always
            # reported a rate of 0.
            app, signatures = qa_run.replay_record(record, args.binary, Path(tempfile.mkdtemp(dir=scratch)))
            if not signatures:
                print(json.dumps({"reproduced": False, "actions": len(actions)}))
                return 2
            args.signature = max(signatures, key=lambda entry: entry.get("actions", 0))["signature"]
            print(f"  signature: {args.signature}", flush=True)
        if args.check_only:
            rate = reproduction_rate(record, args.signature, args.binary, scratch, args.runs, args.jobs)
            print(json.dumps({"rate": rate, "runs": args.runs}))
            return 0
        tester = Tester(record, args.signature, args.binary, scratch, args.jobs)
        if not tester.test_many([list(range(len(actions)))], lambda idx: [actions[i] for i in idx])[0]:
            print(json.dumps({"reproduced": False, "actions": len(actions)}))
            return 2
        tester.try_skipping_checks(actions)
        kept = ddmin(tester, actions, args.max_tests, time.monotonic() + args.time_budget)
        minimal = {k: v for k, v in record.items() if k != "actions"}
        minimal["actions"] = [actions[i] for i in kept]
        minimal["minimized_from"] = len(actions)
        (out / "minimal.json").write_text(json.dumps(minimal, indent=1))
        print(json.dumps({"reproduced": True, "actions": len(kept), "from": len(actions), "tests": tester.tests}))
        return 0
    finally:
        import shutil
        shutil.rmtree(scratch, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
