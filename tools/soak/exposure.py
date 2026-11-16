"""How much soak a finding has had without showing (safety-plan W1.5).

A finding no replay reproduces cannot be verified by a replay: three replays
that show nothing after a fix say what three replays said before it. For those
the soak itself is the test, and this module does its arithmetic.

    rate      the chance one run of the job types that showed the finding shows
              it: runs with a sighting over those job types' runs up to the last
              sighting. The whole history up to then is the denominator, so a
              finding that arrived with a regression reads rarer than it was
              and is asked for more quiet runs, never fewer.
    quiet     the runs of those job types since -- since the last sighting, or,
              for a fix, on builds that contain the fix -- and how many
              sightings that many runs were due at the old rate (`expected`).

`expected` sightings due and none seen is the evidence: the chance of that
silence from a defect still there at its old rate is exp(-expected), 5% at 3
and 1% at 4.6.

Sightings come from the ledger's `new`/`seen` events (one per run that showed
the signature), runs from `runs.jsonl` (one row per reaped job).
"""

from __future__ import annotations

import bisect
import math
from dataclasses import dataclass
from typing import Any, Callable, Iterable

# Sightings due, and none seen, before an open finding is called quiet (5%).
QUIET_EXPECTED = 3.0
# ... and before a fix no replay can check is called verified (1%).
VERIFIED_EXPECTED = 4.6
# ... and before an open finding nobody fixed is closed: the same 1% bar as a
# verification, since closing says the same thing -- it is gone.
CLOSED_EXPECTED = VERIFIED_EXPECTED


def job_of(cell: str) -> str:
    """The job type of a ledger cell "<job>/<build>/<W>x<H>@<scale>"."""
    return cell.split("/", 1)[0]


@dataclass
class Quiet:
    rate: float       # sightings per run of the job types that showed it
    runs: int         # runs of those job types since
    expected: float   # sightings that many runs were due

    @property
    def silence_chance(self) -> float:
        """How likely this silence is if the defect is still there at its old rate."""
        return math.exp(-self.expected)

    def describe(self) -> str:
        return (f"{self.runs} run(s) since, {self.expected:.1f} sighting(s) due at its old rate of 1 in "
                f"{round(1 / self.rate) if self.rate else 0}, none seen")


class Exposure:
    """The soak's runs, by job type and time."""

    def __init__(self, rows: Iterable[dict[str, Any]]):
        by_job: dict[str, list[tuple[float, str]]] = {}
        for row in rows:
            # A run that never took a step exposed nothing (a dead cell).
            if row.get("steps", 1) <= 0:
                continue
            by_job.setdefault(str(row.get("job", "")), []).append((float(row.get("t", 0)), str(row.get("sha", ""))))
        self._runs = {job: sorted(runs) for job, runs in by_job.items()}
        self._times = {job: [t for t, _ in runs] for job, runs in self._runs.items()}

    def runs_until(self, jobs: Iterable[str], end: float) -> int:
        return sum(bisect.bisect_right(self._times.get(job, []), end) for job in set(jobs))

    def runs_after(self, jobs: Iterable[str], start: float,
                   on_build: Callable[[str], bool] | None = None) -> int:
        """Runs of `jobs` later than `start`; with `on_build`, only those whose
        build it accepts (the builds that contain a fix)."""
        total = 0
        for job in set(jobs):
            times = self._times.get(job, [])
            first = bisect.bisect_right(times, start)
            if on_build is None:
                total += len(times) - first
            else:
                total += sum(1 for _, sha in self._runs[job][first:] if on_build(sha))
        return total

    def rate(self, sightings: list[tuple[float, str, str]]) -> float | None:
        """Sightings per run, over the job types that showed it. None without a
        sighting or without a run to count it against."""
        if not sightings:
            return None
        jobs = {job_of(cell) for _, cell, _ in sightings}
        last = max(t for t, _, _ in sightings)
        # The sighting's own run is logged a moment after its ledger line.
        runs = self.runs_until(jobs, last + 5.0)
        if runs <= 0:
            return None
        return min(1.0, len(sightings) / runs)

    def quiet(self, sightings: list[tuple[float, str, str]], since: float | None = None,
              on_build: Callable[[str], bool] | None = None) -> Quiet | None:
        """The silence after the last sighting (or after `since`, on the builds
        `on_build` accepts). None when the finding has no measurable rate."""
        rate = self.rate(sightings)
        if rate is None:
            return None
        jobs = {job_of(cell) for _, cell, _ in sightings}
        last = max(t for t, _, _ in sightings)
        start = (last if since is None else since) + 5.0
        runs = self.runs_after(jobs, start, on_build)
        return Quiet(rate=rate, runs=runs, expected=rate * runs)
