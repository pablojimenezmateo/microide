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
  // Resident-growth envelopes. Growth is much steadier than wall but not as
  // deterministic as an allocation count (page granularity, allocator arena
  // behavior), so these sit between the two.
  double rss_p50_percent = 25.0;
  double rss_p95_percent = 35.0;
  double rss_max_percent = 60.0;
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
};

struct MetricComparison {
  std::string metric;
  double expected = 0.0;
  double actual = 0.0;
  double tolerance_percent = 0.0;
  bool passed = true;
};

struct BaselineComparison {
  std::string scenario_name;
  bool passed = true;
  std::vector<MetricComparison> metrics;
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

std::optional<BaselineRecord> LoadBaseline(const std::filesystem::path& path);
bool SaveBaseline(const std::filesystem::path& path, const BaselineRecord& baseline);
BaselineComparison CompareToBaseline(const BaselineRecord& baseline, const Aggregate& aggregate);

}  // namespace microide::tests::perf
