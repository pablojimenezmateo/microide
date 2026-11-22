#!/usr/bin/env python3
"""Delta-debug a monkey replay down to the shortest action list that still
reproduces a finding (safety-plan W1.5).

    tools/soak/minimize.py REPLAY.json --signature SIG [--binary PATH] [--jobs N]
                           [--out DIR] [--max-tests N] [--time-budget S] [--stop-file PATH]

`REPLAY.json` is the monkey's replay record (the actions up to the finding). A
candidate reproduces when replaying it on a fresh fixture shows a finding with
signature SIG (findings.signature). Zeller's ddmin, with the complements of one
granularity tested in parallel on separate displays; then a last pass drops
single actions. Writes `minimal.json` (a replay record) and prints its length.

It stops when `--time-budget` seconds have passed since it started, or when
`--stop-file` exists (the soak passes its PAUSE: a pause preempts a minimize
rather than waiting hours for it). `minimal.json` is rewritten each time the
list shrinks, so a stop -- or a kill -- keeps the smallest replay found so far;
the printed result names why it stopped (`stopped`).

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

class Budget:
    """When a minimize stops: `seconds` after it started -- its first full
    replay and the skip-checks probe included; the budget used to start after
    them, and with the check's replays before it a check held the machine for
    hours -- or as soon as `stop_file` exists.

    >>> Budget(0).spent()
    'budget spent'
    >>> Budget(3600).spent() is None
    True
    >>> import tempfile, pathlib
    >>> with tempfile.TemporaryDirectory() as d:
    ...     stop = pathlib.Path(d) / "PAUSE"
    ...     before = Budget(3600, stop).spent()
    ...     _ = stop.write_text("lanes")
    ...     (before, Budget(3600, stop).spent())
    (None, 'stopped: PAUSE exists')
    """

    def __init__(self, seconds: float, stop_file: Path | None = None):
        self.deadline = time.monotonic() + seconds
        self.stop_file = stop_file
        # The first stop a caller was told of: what cut the work short, which a
        # later look (a PAUSE taken just after a minimize converged) is not.
        self.reason: str | None = None

    def spent(self) -> str | None:
        if self.reason is None:
            if self.stop_file is not None and self.stop_file.exists():
                self.reason = f"stopped: {self.stop_file.name} exists"
            elif time.monotonic() >= self.deadline:
                self.reason = "budget spent"
        return self.reason


# Rows that are not actions of their own: replay skips them anyway.
_PASSIVE = ("check", "note", "type-via-control", "corpus")


def actions_of(record: dict) -> list[dict]:
    return [a for a in record.get("actions", []) if not any(key in a for key in _PASSIVE)]


class Tester:
    def __init__(self, record: dict, signature: str, binary: Path, scratch: Path, jobs: int,
                 budget: Budget | None = None):
        self.record = record
        self.budget = budget
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
        reproduced = self._run(actions)
        if reproduced is None:
            return  # stopped: nothing learned, and nothing runs after it
        if not reproduced:
            self.check_each = True
            print("  the finding needs the per-action checks' timing; keeping them", flush=True)

    def _run(self, actions: list[dict]) -> bool | None:
        return self.run_detailed(actions)["hit"]

    def _stopping(self) -> bool:
        return self.budget is not None and self.budget.spent() is not None

    def run_detailed(self, actions: list[dict]) -> dict:
        """One replay of `actions`: whether it showed the signature (`hit`),
        whether the driver got it to the end (`ran`; else `error`), where it
        left the run's path (`path`, monkey_model.Divergence) and what else it
        showed (`others`)."""
        out = Path(tempfile.mkdtemp(prefix="t-", dir=self.scratch))
        try:
            app, signatures = qa_run.replay_record(self.record, self.binary, out, actions=actions,
                                                   check=self.check_each, stop=self._stopping)
            if not self.kind:
                self.learn_kind(signatures)
            return {"hit": any(entry.get("signature") == self.signature for entry in signatures),
                    "ran": bool(getattr(app, "replay_completed", True)),
                    "path": app.divergence.summary() if getattr(app, "divergence", None) is not None else {},
                    "others": sorted({f"{entry.get('signature')} {entry.get('label', '')[:80]}"
                                      for entry in signatures if entry.get("signature") != self.signature})}
        except q.ReplayStopped:
            # Ended between actions by the budget: unknown, as a candidate the
            # stop kept from starting is (test_many).
            return {"hit": None, "ran": False, "path": {}, "others": []}
        except Exception as error:  # noqa: BLE001 - a candidate that breaks the driver does not reproduce
            print(f"    candidate failed to run: {type(error).__name__}: {error}", flush=True)
            return {"hit": False, "ran": False, "error": f"{type(error).__name__}: {error}"[:300],
                    "path": {}, "others": []}
        finally:
            import shutil
            shutil.rmtree(out, ignore_errors=True)

    def test_many(self, candidates: list[list[int]], indices_to_actions) -> list[bool | None]:
        """Whether each candidate reproduces; None for one the budget stopped
        before it started (unknown: falsy for the callers, and not cached)."""
        def run(candidate: list[int]) -> bool | None:
            # Checked as each replay starts, not per batch: a stop waits for the
            # replays in flight only.
            if self.budget is not None and self.budget.spent():
                return None
            return self._run(indices_to_actions(candidate))

        todo = [c for c in candidates if tuple(c) not in self.cache]
        ran: dict[tuple[int, ...], bool | None] = {}
        if todo:
            with concurrent.futures.ThreadPoolExecutor(max_workers=self.jobs) as pool:
                results = list(pool.map(run, todo))
            for candidate, result in zip(todo, results):
                ran[tuple(candidate)] = result
                if result is not None:
                    self.cache[tuple(candidate)] = result
                    self.tests += 1
        return [self.cache.get(tuple(c), ran.get(tuple(c))) for c in candidates]


_META = ("t", "s", "b")


def kinds_of(action: dict) -> tuple[str, str]:
    """An action's kind, coarse and fine: what the by-kind pass drops together.

    >>> kinds_of({"key": "ctrl+s", "t": 1.0, "s": "="})
    ('key', 'key:ctrl+s')
    >>> kinds_of({"line": "set-setting editor.wrap word", "t": 2.0})
    ('line:set-setting', 'line:set-setting editor.wrap')
    >>> kinds_of({"external": "diff-variant", "target": "", "counter": 3})
    ('external:diff-variant', 'external:diff-variant')
    >>> kinds_of({"kill9": True, "waited": True})
    ('kill9', 'kill9')
    """
    keys = [key for key in action if key not in _META]
    first = keys[0] if keys else "?"
    if first == "line":
        words = str(action["line"]).split()
        coarse = "line:" + (words[0] if words else "")
        return coarse, "line:" + " ".join(words[:2])
    if first in ("external", "oracle", "churn"):
        coarse = f"{first}:{action.get(first)}"
        return coarse, coarse
    detail = action.get(first)
    if isinstance(detail, str):
        return first, f"{first}:{detail}"
    return first, first


def kind_groups(actions: list[dict], current: list[int],
                skip: frozenset[str] = frozenset()) -> list[tuple[str, list[int]]]:
    """`current`'s indices grouped by kind, coarse and fine, two or more to a
    group, largest first -- each one candidate to drop whole -- as (kind,
    indices), leaving out the kinds in `skip`.

    >>> acts = [{"key": "a"}, {"move": [1, 2]}, {"key": "a"}, {"move": [3, 4]}, {"key": "b"}]
    >>> kind_groups(acts, [0, 1, 2, 3, 4])
    [('key', [0, 2, 4]), ('key:a', [0, 2]), ('move', [1, 3])]
    >>> kind_groups(acts, [0, 1, 2, 3, 4], skip=frozenset({"key"}))
    [('key:a', [0, 2]), ('move', [1, 3])]
    """
    groups: dict[str, list[int]] = {}
    for index in current:
        coarse, fine = kinds_of(actions[index])
        groups.setdefault(coarse, []).append(index)
        if fine != coarse:
            groups.setdefault(fine, []).append(index)
    unique: dict[tuple[int, ...], tuple[str, list[int]]] = {}
    for kind, members in groups.items():
        if kind not in skip and 2 <= len(members) < len(current):
            unique.setdefault(tuple(members), (kind, members))
    return sorted(unique.values(), key=lambda entry: len(entry[1]), reverse=True)


def reduce_by_kind(tester: Tester, actions: list[dict], current: list[int], max_tests: int,
                   budget: Budget, width: int = 24, progress=None) -> list[int]:
    """Drop whole kinds of action -- every pointer move, every wheel notch, every
    `set-setting` of one key, every kill -- before ddmin cuts by position.

    ddmin cuts contiguous chunks, and a run's incidental actions are spread
    evenly through it: a chunk always holds something the finding needs, so
    the complements fail until the chunks are a few actions wide, and by then
    the test budget is spent (minimal replays of 924 and 1829 actions, a median
    of 420 over 289 findings, 2026-11-20). One test per kind removes a kind
    wherever it is.

    Each round tests the `width` largest groups at once. When several drops
    hold, their union is tried too (one more test): kinds are mostly
    independent, and taking one drop a round cost a round per kind. A kind
    whose drop lost the finding is not tried again -- it was needed in a larger
    list, and a smaller one rarely needs it less."""
    needed: set[str] = set()
    while tester.tests < max_tests and not budget.spent():
        groups = kind_groups(actions, current, frozenset(needed))[:min(width, max_tests - tester.tests)]
        if not groups:
            break
        candidates = [[i for i in current if i not in set(members)] for _, members in groups]
        results = tester.test_many(candidates, lambda idx: [actions[i] for i in idx])
        held = [(kind, candidate) for (kind, _), candidate, ok in zip(groups, candidates, results)
                if ok and candidate]
        needed.update(kind for (kind, _), ok in zip(groups, results) if ok is False)
        if not held:
            break
        best = min((candidate for _, candidate in held), key=len)
        if len(held) > 1:
            dropped = set(current).difference(*[set(candidate) for _, candidate in held])
            union = [i for i in current if i not in dropped]
            if union and len(union) < len(best) and tester.test_many([union], lambda idx: [actions[i] for i in idx])[0]:
                best = union
        current = best
        print(f"  by kind: {len(current)} actions ({tester.tests} tests)", flush=True)
        if progress is not None:
            progress(current)
    return current


def ddmin(tester: Tester, actions: list[dict], max_tests: int, budget: Budget,
          start: list[int] | None = None, progress=None) -> list[int]:
    def to_actions(indices: list[int]) -> list[dict]:
        return [actions[i] for i in indices]

    current = list(range(len(actions))) if start is None else list(start)
    n = 2
    while len(current) >= 2 and tester.tests < max_tests and not budget.spent():
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
            if progress is not None:
                progress(current)
            continue
        if n < size:
            results = tester.test_many(subsets, to_actions)
            reduced = next((s for s, ok in zip(subsets, results) if ok), None)
            if reduced is not None:
                current = reduced
                n = 2
                print(f"  ddmin: {len(current)} actions ({tester.tests} tests)", flush=True)
                if progress is not None:
                    progress(current)
                continue
        if n >= size or budget.spent():
            break  # (a stop mid-round left candidates untested: not a verdict on this granularity)
        n = min(size, n * 2)
    # One-minimality: try dropping each remaining action alone.
    changed = True
    while changed and len(current) > 1 and tester.tests < max_tests and not budget.spent():
        changed = False
        candidates = [[i for i in current if i != drop] for drop in current]
        results = tester.test_many(candidates, to_actions)
        for candidate, ok in zip(candidates, results):
            if ok:
                current = candidate
                changed = True
                print(f"  one-minimal pass: {len(current)} actions", flush=True)
                if progress is not None:
                    progress(current)
                break
    return current


def reproduction_report(record: dict, signature: str, binary: Path, scratch: Path, runs: int,
                        jobs: int) -> dict:
    """`runs` replays of the record, summed up (summarize_runs)."""
    tester = Tester(record, signature, binary, scratch, jobs)
    actions = actions_of(record)
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, min(jobs, runs))) as pool:
        results = list(pool.map(lambda _: tester.run_detailed(actions), range(runs)))
    return summarize_runs(results)


def summarize_runs(results: list[dict]) -> dict:
    """What a set of replays says about a finding, beyond how many showed it.

    "0 of 12" used to be the whole answer, and it had three meanings: twelve
    replays that went where the run went and saw nothing (a race: the actions
    are not enough), twelve that went somewhere else (the record does not
    rebuild the run: a harness gap), and twelve the driver could not finish
    (nothing was tested). `ran` counts the replays that reached the end,
    `diverged` those that left the run's path, with the first place one did;
    `others` is what they showed instead.

    >>> r = summarize_runs([{"hit": False, "ran": True, "path": {"verdict": "followed"}, "others": []},
    ...                     {"hit": True, "ran": True, "path": {"verdict": "diverged", "first": {"action": 7}},
    ...                      "others": ["abc x"]},
    ...                     {"hit": False, "ran": False, "error": "QaFailure: no window", "path": {}, "others": []}])
    >>> r["rate"], r["runs"], r["ran"], r["diverged"], r["followed"], r["divergence"], r["errors"], r["others"]
    (0.5, 3, 2, 1, 1, {'action': 7}, ['QaFailure: no window'], ['abc x'])
    """
    ran = [r for r in results if r.get("ran")]
    hits = sum(1 for r in results if r.get("hit"))
    diverged = [r for r in ran if (r.get("path") or {}).get("verdict") == "diverged"]
    # Over the replays that ran: one the driver could not finish tested nothing.
    return {"rate": hits / max(1, len(ran)) if ran else 0.0, "runs": len(results), "ran": len(ran),
            "followed": sum(1 for r in ran if (r.get("path") or {}).get("verdict") in ("followed", "wobbled")),
            "diverged": len(diverged),
            "divergence": (diverged[0]["path"].get("first") if diverged else None),
            "errors": sorted({r["error"] for r in results if r.get("error")})[:3],
            "others": sorted({other for r in results for other in r.get("others", [])})[:8]}


def reproduction_rate(record: dict, signature: str, binary: Path, scratch: Path, runs: int, jobs: int) -> float:
    return reproduction_report(record, signature, binary, scratch, runs, jobs)["rate"]


def rate_and_pace(record: dict, signature: str, binary: Path, scratch: Path, runs: int,
                  jobs: int) -> tuple[float, bool]:
    report = report_and_pace(record, signature, binary, scratch, runs, jobs)
    return report["rate"], report["paced"]


def report_and_pace(record: dict, signature: str, binary: Path, scratch: Path, runs: int, jobs: int) -> dict:
    """The reproduction report at the replay's default pace, and, when that
    never shows the finding, at the run's recorded pace (q.replay `paced`): a
    race needs the run's timing, which settle-and-go replays lose -- most
    one-off findings measured 0% that way. `paced` says which it is."""
    report = reproduction_report({**record, "paced": False}, signature, binary, scratch, runs, jobs)
    if report["rate"] > 0:
        return {**report, "paced": False}
    paced = reproduction_report({**record, "paced": True}, signature, binary, scratch, runs, jobs)
    if paced["rate"] > 0:
        return {**paced, "paced": True}
    # Neither showed it: both sets are the evidence of that.
    merged = dict(paced if paced["ran"] >= report["ran"] else report)
    merged.update(runs=report["runs"], paced=False,
                  errors=sorted(set(report["errors"]) | set(paced["errors"]))[:3],
                  others=sorted(set(report["others"]) | set(paced["others"]))[:8])
    return merged


def pin_binaries(binary: Path, scratch: Path) -> Path:
    """A private copy of the binary under `scratch`, and of the microide-server
    beside it; returns the copy's path.

    A copy, because a rebuild of the tree the binary came from replaces it
    mid-run, and every candidate after that "did not reproduce" for want of a
    binary to run (2026-11-08). The server too, because a remote replay installs
    the one beside its binary on the run's host (run.py server_binary): with
    the app alone copied, every replay of every finding of the `remote` job
    stopped at "no microide-server at <scratch>/microide-server" before its
    first action -- booked as a rate of zero, then as "not tested", for the one
    job that reaches the remote code at all."""
    import shutil
    pinned = scratch / "microide"
    shutil.copy2(binary, pinned)
    server = binary.parent / "microide-server"
    if server.is_file():
        shutil.copy2(server, scratch / "microide-server")
    return pinned


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("replay", type=Path)
    parser.add_argument("--signature", required=True,
                        help="the finding's signature, or `auto`: the last one a full replay shows")
    parser.add_argument("--binary", type=Path, default=q.REPO / "build/microide/microide")
    parser.add_argument("--jobs", type=int, default=6)
    parser.add_argument("--out", type=Path, help="directory for minimal.json (default: beside the replay)")
    parser.add_argument("--max-tests", type=int, default=150)
    parser.add_argument("--time-budget", type=float, default=3600,
                        help="seconds from the start, the first full replay included")
    parser.add_argument("--stop-file", type=Path,
                        help="stop (keeping the smallest replay so far) as soon as this file exists")
    parser.add_argument("--check-only", action="store_true", help="print the reproduction rate and exit")
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--paced", action="store_true",
                        help="replay every candidate at the run's recorded pace (what --check-only "
                             "reports a race needs)")
    args = parser.parse_args()
    budget = Budget(args.time_budget, args.stop_file)
    record = json.loads(args.replay.read_text())
    out = args.out or args.replay.parent
    out.mkdir(parents=True, exist_ok=True)
    scratch = Path(tempfile.mkdtemp(prefix="minimize-", dir=out))
    args.binary = pin_binaries(args.binary, scratch)
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
            print(json.dumps(report_and_pace(record, args.signature, args.binary, scratch, args.runs, args.jobs)))
            return 0
        if args.paced:
            record = {**record, "paced": True}
        tester = Tester(record, args.signature, args.binary, scratch, args.jobs, budget)
        reproduced = tester.test_many([list(range(len(actions)))], lambda idx: [actions[i] for i in idx])[0]
        if reproduced is None:
            print(json.dumps({"reproduced": None, "actions": len(actions), "stopped": budget.spent()}))
            return 3
        if not reproduced:
            print(json.dumps({"reproduced": False, "actions": len(actions)}))
            return 2
        tester.try_skipping_checks(actions)

        def save(kept: list[int], stopped: str | None = None) -> None:
            # Every list passed here reproduced, so a stop or a kill after it
            # leaves a replay that does.
            minimal = {k: v for k, v in record.items() if k != "actions"}
            minimal["actions"] = [actions[i] for i in kept]
            minimal["minimized_from"] = len(actions)
            if stopped:
                minimal["minimize_stopped"] = stopped
            (out / "minimal.json").write_text(json.dumps(minimal, indent=1))

        # A third of the tests at most: ddmin and the one-minimal pass need the rest.
        by_kind = reduce_by_kind(tester, actions, list(range(len(actions))),
                                 tester.tests + max(1, args.max_tests // 3), budget, progress=save)
        kept = ddmin(tester, actions, args.max_tests, budget, start=by_kind, progress=save)
        stopped = budget.reason
        save(kept, stopped)
        print(json.dumps({"reproduced": True, "actions": len(kept), "from": len(actions), "tests": tester.tests,
                          "stopped": stopped}))
        return 0
    finally:
        import shutil
        shutil.rmtree(scratch, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
