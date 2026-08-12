#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "perf/PerfHarness.h"

namespace microide::tests::perf {

struct Tolerances {
  // Wall-time envelopes. Wall carries machine scheduler jitter (context switches,
  // shared-runner load), so these are sometimes widened per scenario.
  double p50_percent = 10.0;
  double p95_percent = 20.0;
  double max_percent = 50.0;
  // Allocation-count envelopes. Allocation counts are deterministic run-to-run,
  // so they are the true complexity oracle and can stay tight even when the wall
  // envelopes are widened for jitter. LoadBaseline defaults these to the wall
  // values when a baseline omits them, so pre-decoupling baselines keep their
  // exact prior behavior (both metrics gated on the same percent).
  double alloc_p50_percent = 10.0;
  double alloc_p95_percent = 20.0;
  double alloc_max_percent = 50.0;
  // CPU-time envelopes. CPU carries the same scheduler jitter as wall, so these
  // default to the wall values.
  double cpu_p50_percent = 10.0;
  double cpu_p95_percent = 20.0;
  double cpu_max_percent = 50.0;
  // Resident-growth envelope, for the ONE resident gate (the trimmed mean; see
  // CompareToBaseline). Growth is much steadier than wall but not as deterministic
  // as an allocation count (page granularity, allocator arena behavior), so it
  // sits between the two.
  double rss_mean_percent = 25.0;
  // Net-heap-retention envelope, for `p50_net_heap_bytes`. Deterministic to the
  // byte across process states (52 of 52 scenarios reproduced exactly across
  // three different suite prefixes; worst spread 9 bytes), so this is an
  // allocation-class envelope, not an RSS-class one. It still gets a percentage
  // rather than an equality check because a scenario is allowed to change size
  // for a legitimate reason; what it may not do is change silently.
  double net_heap_percent = 10.0;
  // Per-phase allocation envelope, for the `phase[<name>].p50_allocations`
  // gates. A phase count is a strict subset of the scenario total measured on
  // the same thread, so it is at least as deterministic as the total and gets
  // the same class of envelope. Defaults to the scenario's allocation p50
  // tolerance when a baseline omits it.
  double phase_alloc_p50_percent = 10.0;
};

struct BaselineRecord {
  std::string scenario_name;
  MetricSet metrics;
  Tolerances tolerances;
  // Whether the baseline file actually carried cpu/rss numbers. The 93 baselines
  // recorded before these metrics existed do not, and a missing metric must not be
  // compared against an implicit 0.0 — that would fail every scenario instantly.
  // They stay ungated until re-recorded on the reference runner, which is a
  // deliberate act like any other rebaseline.
  bool has_cpu_metrics = false;
  bool has_rss_metrics = false;
  // Whether the baseline file carried `p50_net_heap_bytes`. Same
  // start-gating-when-re-recorded rule as cpu/rss: a baseline written before the
  // metric existed compares exactly as it did, rather than gating a measured
  // 28,470 against an implicit 0.
  bool has_net_heap_metrics = false;
  // Per-phase allocation baselines, in the order the scenario measures them.
  // Empty on a baseline written before phase gating existed, which leaves that
  // scenario gated on its total alone exactly as it was — and says so on the
  // verdict line, because a phase that is measured and not gated is precisely
  // the state TD-2026-08-06-153 was filed about.
  std::vector<PhaseMetricSet> phases;
  // Whether the baseline recorded the clock it was captured at
  // (`metrics.p50_cpu_calibration_ns`). Baselines written before that field
  // existed did not, and they compare unnormalised exactly as they did before.
  bool has_calibration = false;
  // How many measured iterations this baseline was recorded over. Zero on a
  // baseline written before the field existed, which leaves it comparing exactly
  // as it did.
  //
  // Only ONE metric needs it, and it is not a rounding detail:
  // `mean_rss_growth_bytes` is a trimmed mean over a SETTLING series, so its
  // value depends on how many iterations the run averaged. typing_large_file
  // read 84-95 KB at 10 iterations and 100-114 KB at 6 — a red gate at 6 and a
  // green one at 10, same binary, same box, with the failure line saying
  // "measured=113869 (+41%)" and nothing about the sample size
  // (TD-2026-08-06-148). Comparing a short run against a long baseline is not a
  // measurement, so CompareToBaseline declines to gate it and says why.
  std::size_t iterations = 0;
  // Which revision of the scenario produced these numbers. See
  // `Scenario::measurement_revision`. Absent in a baseline written before the
  // field existed, which reads as 1 — correct by construction, because 1 is what
  // every scenario declares until somebody changes what it measures.
  std::size_t measurement_revision = 1;
  // Set when the timing/resident half of this baseline was recorded on a runner
  // that is NOT the reference lane -- a developer box under load, where a wall,
  // cpu or resident number is a statement about that machine as much as about
  // the code.
  //
  // The alternative the harness had was all-or-nothing: refuse to write the
  // baseline at all, which is why two scenarios shipped with `baseline_gated =
  // false` and gated on nothing (TD-2026-08-12-186), and why five allocation
  // gates sat 12-40 % loose with no way to tighten them here
  // (TD-2026-08-11-184). Allocation counts and net-heap retention are
  // deterministic and portable; the timing half is not. So a baseline can now
  // say which half it is: the deterministic metrics gate normally, and the
  // machine-sensitive ones are reported and explicitly NOT enforced, with the
  // reason on the verdict line, until an idle reference run arms them.
  //
  // False on every baseline written before this field existed, which is correct:
  // those were all recorded on the reference runner.
  bool timing_is_advisory = false;
};

// Ceiling on the clock-normalisation factor, in either direction. A machine
// genuinely 3x off the one a baseline was captured on is not the same reference
// lane, and a probe reading that far out is much more likely to be a preempted
// probe than a real clock move -- either way, scaling a gate by it would loosen it
// past the point of measuring anything. Clamping and saying so beats going quietly
// vacuous. 3x sits well above the 2.0x the governor was measured to be worth
// (TD-2026-08-05-137) and well below "these are different machines".
inline constexpr double kMaxClockNormalizationFactor = 3.0;

struct MetricComparison {
  std::string metric;
  double expected = 0.0;
  double actual = 0.0;
  double tolerance_percent = 0.0;
  bool passed = true;
  // For a clock-normalised metric, the raw measurement before normalisation.
  // Equal to `actual` for every other metric. Reported so a verdict line can say
  // both what was measured and what it was worth on the baseline's machine state.
  double raw_actual = 0.0;
  // False when the metric was measured and reported but deliberately NOT gated,
  // because the run cannot be compared against this baseline for that metric
  // (today: a resident gate on a run shorter than the baseline it would be
  // compared against — see BaselineRecord::iterations). `passed` still carries
  // the raw arithmetic so the number is readable, but an unenforced metric never
  // turns BaselineComparison::passed red. Callers ranking headroom must skip
  // these: an unenforced metric has no envelope to consume.
  bool enforced = true;
  // Human-readable reason attached to this metric, printed on the verdict line.
  // Set whenever `enforced` is false, and also on an enforced-but-noteworthy
  // comparison (a longer run than the baseline, which reads a gate loose).
  // Empty on the ordinary case, so a clean run says nothing.
  std::string note;
};

// How a run's clock compared to the clock the baseline was captured at, and what
// the comparison did about it. See NormalizeCpuAgainstBaselineClock.
struct ClockNormalization {
  bool applied = false;
  // measured clock / baseline clock, as the probe reads it: >1 means this run's
  // machine was slower, so a CPU duration was expected to be proportionally larger.
  double factor = 1.0;
  // Set when `factor` hit the sanity clamp. A probe reading that far out is more
  // likely broken (a preempted probe, a baseline from another machine class) than
  // a real 3x clock move, and silently scaling a gate by it would make the gate
  // vacuous without anybody noticing.
  bool clamped = false;
};

struct BaselineComparison {
  std::string scenario_name;
  bool passed = true;
  std::vector<MetricComparison> metrics;
  ClockNormalization clock;
};

// Spread of the harness's fixed-work CPU calibration probe across one scenario's
// measured iterations.
//
// The probe is a dependent integer chain timed OUTSIDE the measured window, so it
// can only move when the machine does. That makes it the one number that
// separates "this code got slower" from "this core was clocked lower" — on
// idle_soak_30s it stepped 671 -> 857 us mid-run while cpu_ms stepped 14 -> 30 ms
// with every application counter byte-identical across the step
// (TD-2026-08-05-137). It was recorded but never read, so that diagnosis had to be
// done by hand from --report-json; this is what makes a duration-gate failure say
// whether it is even about the code.
struct CalibrationSpread {
  bool valid = false;
  std::uint64_t min_ns = 0;
  std::uint64_t max_ns = 0;
  double ratio = 1.0;
};

// 10% is well below the 28% swing that produced a real gate failure and well above
// the ~1% the probe drifts on a machine holding one clock.
inline constexpr double kCalibrationSpreadNoteRatio = 1.10;

CalibrationSpread MeasureCalibrationSpread(const Aggregate& aggregate);
// Empty when the run recorded no probe. Callers gate on
// `spread.ratio >= kCalibrationSpreadNoteRatio` first; a run on a steady clock
// should say nothing at all.
std::string DescribeCalibrationSpread(const CalibrationSpread& spread);

// How far past its baseline a metric landed, as a percentage. Positive means
// slower / more allocations. Zero when the baseline is zero (nothing to divide
// by, and a gate on a zero baseline is not measuring a ratio anyway).
double MetricDeltaPercent(const MetricComparison& metric);

// How much of a gated metric's tolerance the measurement consumed, in percent of
// the envelope. 0 = exactly on the baseline, 100 = exactly at the limit, >100 =
// failed, negative = the code got faster than the baseline records.
//
// This is the number TD-2026-08-06-139 needed and nobody had. Five allocation
// gates drifted up (one by 9.4% against a 10% tolerance) and every one of them
// reported PASS, because a pass/fail bit cannot distinguish "unchanged" from "one
// allocation short of red". Envelope consumption can, from a single run, without
// a second report to diff against.
double EnvelopeUsedPercent(const MetricComparison& metric);

// Envelope consumption at or above which a PASSING gated metric is worth saying
// out loud. 75% leaves a quarter of the envelope as a warning band: enough margin
// that a real regression is reported before it turns the suite red, tight enough
// that an unchanged scenario says nothing at all.
inline constexpr double kEnvelopeNoticePercent = 75.0;

// Fold a fresh run's DETERMINISTIC metrics into a committed baseline, leaving
// every clock- and machine-state-dependent number exactly as it was recorded.
//
// The two halves of a baseline have different requirements on the runner. Wall,
// CPU, the calibration probe and resident growth are only meaningful when the box
// is idle: recording them on a loaded machine records the load, and the gate that
// results is loose by whatever the load was worth. Allocation counts, per-phase
// allocation counts and `p50_net_heap_bytes` are none of those things — they are
// properties of the code path, reproduce to the byte across process states, and
// are exactly as valid recorded under load as on a quiet box.
//
// That asymmetry used to force an all-or-nothing choice, and the all-or-nothing
// choice is what left the whole committed set 82x loose on its ALLOCATION gates
// (TD-2026-08-07-161): the code moved, the runner was busy, so nothing was
// rerecorded and the deterministic gates stayed stale alongside the timing ones
// that genuinely had to wait. This is the half that never had to wait.
//
// `fresh` is the record this run would have written; `existing` is what is
// committed. Metrics taken from `fresh`: p50/p95/max_allocations,
// p50_net_heap_bytes, and every phase's allocation counts. Everything else —
// wall, cpu, rss, the calibration probe, the recorded iteration count and the
// cpu/rss/calibration presence flags — is carried over from `existing`, because
// those are the numbers this run is not entitled to replace.
//
// A phase in `fresh` with no counterpart in `existing` is taken whole (there is
// no prior wall reading to preserve), and a phase in `existing` that this run no
// longer measures is dropped, both matching a full rebaseline.
BaselineRecord MergeDeterministicMetrics(const BaselineRecord& existing,
                                         const BaselineRecord& fresh);

std::optional<BaselineRecord> LoadBaseline(const std::filesystem::path& path);
bool SaveBaseline(const std::filesystem::path& path, const BaselineRecord& baseline);
BaselineComparison CompareToBaseline(const BaselineRecord& baseline, const Aggregate& aggregate);

}  // namespace microide::tests::perf
