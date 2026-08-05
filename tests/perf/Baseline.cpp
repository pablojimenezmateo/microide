#include "perf/Baseline.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

#include "util/JsonValue.h"

namespace microide::tests::perf {
namespace {

// JsonValue has no IsNumber(), and a metric may be written as either an integer
// or a double depending on its value, so both count as "the baseline recorded it".
bool IsNumber(const util::JsonValue& value) {
  return value.IsInt() || value.IsDouble();
}

double AllowedDelta(double expected, double tolerance_percent) {
  return std::abs(expected) * (tolerance_percent / 100.0);
}

bool WithinTolerance(double expected, double actual, double tolerance_percent) {
  return actual <= expected + AllowedDelta(expected, tolerance_percent);
}

void AddMetric(BaselineComparison* out,
               std::string_view name,
               double expected,
               double actual,
               double tolerance_percent,
               double raw_actual) {
  MetricComparison metric{
      .metric = std::string(name),
      .expected = expected,
      .actual = actual,
      .tolerance_percent = tolerance_percent,
      .passed = WithinTolerance(expected, actual, tolerance_percent),
      .raw_actual = raw_actual,
  };
  if (!metric.passed) {
    out->passed = false;
  }
  out->metrics.push_back(std::move(metric));
}

void AddMetric(BaselineComparison* out,
               std::string_view name,
               double expected,
               double actual,
               double tolerance_percent) {
  AddMetric(out, name, expected, actual, tolerance_percent, actual);
}

double ClampNormalizationFactor(double factor, bool* clamped) {
  if (factor > kMaxClockNormalizationFactor) {
    *clamped = true;
    return kMaxClockNormalizationFactor;
  }
  if (factor < 1.0 / kMaxClockNormalizationFactor) {
    *clamped = true;
    return 1.0 / kMaxClockNormalizationFactor;
  }
  return factor;
}

// Re-express this run's CPU durations in the machine state the baseline was
// captured in, so the gate compares code against code.
//
// cpu_ms is a duration; it scales with the machine's effective clock, and the
// baselines never recorded what that clock was. So `p50_cpu_ms: baseline=14.7
// measured=29.9` was a statement about the governor as much as about the binary
// — the same binary passed the same gate on other runs of the same suite
// (TD-2026-08-05-137). Recording the clock in the baseline and scaling by the
// ratio makes every CPU gate machine-state-independent instead of exempting
// scenarios from CPU gating one at a time.
//
// PER ITERATION, against that iteration's own probe. The motivating failure was a
// clock that stepped in the middle of a run — five iterations at one clock and
// five at another — which a single per-run factor would smear across both halves.
// Each iteration's reading cancels its own clock exactly, and the percentiles are
// then taken over the normalised series.
struct NormalizedCpu {
  MetricSet metrics;
  ClockNormalization clock;
};

NormalizedCpu NormalizeCpuAgainstBaselineClock(const BaselineRecord& baseline,
                                               const Aggregate& aggregate) {
  NormalizedCpu out;
  out.metrics = aggregate.metrics;
  const double baseline_ns = baseline.metrics.p50_cpu_calibration_ns;
  if (!baseline.has_calibration || baseline_ns <= 0.0) {
    return out;
  }

  std::vector<double> normalized;
  normalized.reserve(aggregate.iterations.size());
  std::vector<double> measured_ns;
  measured_ns.reserve(aggregate.iterations.size());
  for (const Iteration& iteration : aggregate.iterations) {
    if (iteration.metrics.cpu_calibration_ns == 0) {
      continue;
    }
    const double iteration_ns = static_cast<double>(iteration.metrics.cpu_calibration_ns);
    measured_ns.push_back(iteration_ns);
    normalized.push_back(iteration.metrics.cpu_ms *
                         ClampNormalizationFactor(baseline_ns / iteration_ns, &out.clock.clamped));
  }

  if (normalized.empty()) {
    // No per-iteration probe. A caller that supplied only aggregate metrics (the
    // baseline unit tests, and any future report replay) can still normalise on
    // the run-level median, which is exact whenever the clock held steady.
    if (aggregate.metrics.p50_cpu_calibration_ns <= 0.0) {
      return out;
    }
    const double factor =
        ClampNormalizationFactor(baseline_ns / aggregate.metrics.p50_cpu_calibration_ns,
                                 &out.clock.clamped);
    out.metrics.p50_cpu_ms = aggregate.metrics.p50_cpu_ms * factor;
    out.metrics.p95_cpu_ms = aggregate.metrics.p95_cpu_ms * factor;
    out.metrics.max_cpu_ms = aggregate.metrics.max_cpu_ms * factor;
    out.clock.applied = true;
    out.clock.factor = 1.0 / factor;
    return out;
  }

  out.metrics.p50_cpu_ms = Percentile(normalized, 0.50);
  out.metrics.p95_cpu_ms = Percentile(normalized, 0.95);
  out.metrics.max_cpu_ms = *std::max_element(normalized.begin(), normalized.end());
  out.clock.applied = true;
  out.clock.factor = Percentile(measured_ns, 0.50) / baseline_ns;
  return out;
}

}  // namespace

std::optional<BaselineRecord> LoadBaseline(const std::filesystem::path& path) {
  std::ifstream file(path);
  if (!file) {
    return std::nullopt;
  }
  std::string json((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  const std::optional<util::JsonValue> parsed = util::ParseJson(json);
  if (!parsed.has_value() || !parsed->IsObject()) {
    return std::nullopt;
  }
  const util::JsonValue& metrics = (*parsed)["metrics"];
  const util::JsonValue& tolerances = (*parsed)["tolerances"];

  BaselineRecord record;
  record.scenario_name = (*parsed)["scenario"].AsString();
  record.metrics.p50_wall_ms = metrics["p50_wall_ms"].AsDouble();
  record.metrics.p95_wall_ms = metrics["p95_wall_ms"].AsDouble();
  record.metrics.max_wall_ms = metrics["max_wall_ms"].AsDouble();
  record.metrics.p50_allocations = metrics["p50_allocations"].AsDouble();
  record.metrics.p95_allocations = metrics["p95_allocations"].AsDouble();
  record.metrics.max_allocations = metrics["max_allocations"].AsDouble();

  record.tolerances.p50_percent = tolerances["p50_percent"].AsDouble(10.0);
  record.tolerances.p95_percent = tolerances["p95_percent"].AsDouble(20.0);
  record.tolerances.max_percent = tolerances["max_percent"].AsDouble(50.0);
  // Allocation envelopes default to the wall envelopes when absent, so a baseline
  // written before wall/alloc tolerances were decoupled behaves exactly as before
  // (both metrics gated on the same percent).
  record.tolerances.alloc_p50_percent =
      tolerances["alloc_p50_percent"].AsDouble(record.tolerances.p50_percent);
  record.tolerances.alloc_p95_percent =
      tolerances["alloc_p95_percent"].AsDouble(record.tolerances.p95_percent);
  record.tolerances.alloc_max_percent =
      tolerances["alloc_max_percent"].AsDouble(record.tolerances.max_percent);

  // cpu/rss are gated only when the baseline actually recorded them. Treating an
  // absent metric as 0.0 would fail every pre-existing baseline on the first run;
  // treating it as "not recorded" lets the 93 existing files keep working and
  // start gating when they are next re-recorded on the reference runner.
  record.has_cpu_metrics = IsNumber(metrics["p50_cpu_ms"]);
  if (record.has_cpu_metrics) {
    record.metrics.p50_cpu_ms = metrics["p50_cpu_ms"].AsDouble();
    record.metrics.p95_cpu_ms = metrics["p95_cpu_ms"].AsDouble();
    record.metrics.max_cpu_ms = metrics["max_cpu_ms"].AsDouble();
  }
  // The clock the baseline was captured at. Absent on every baseline written
  // before it existed, which leaves those comparing unnormalised -- the same
  // deliberate "start gating when re-recorded" rule cpu/rss already follow.
  record.has_calibration = IsNumber(metrics["p50_cpu_calibration_ns"]);
  if (record.has_calibration) {
    record.metrics.p50_cpu_calibration_ns = metrics["p50_cpu_calibration_ns"].AsDouble();
  }
  // The GATED resident statistic is the trimmed mean. A baseline that predates it
  // records only the percentiles, and gating those was a coin flip in both
  // directions (TD-2026-08-05-136), so such a baseline stays ungated on resident
  // growth until it is re-recorded rather than being gated on the wrong number.
  record.has_rss_metrics = IsNumber(metrics["mean_rss_growth_bytes"]);
  if (record.has_rss_metrics) {
    record.metrics.mean_rss_growth_bytes = metrics["mean_rss_growth_bytes"].AsDouble();
    record.metrics.p50_rss_growth_bytes = metrics["p50_rss_growth_bytes"].AsDouble();
    record.metrics.p95_rss_growth_bytes = metrics["p95_rss_growth_bytes"].AsDouble();
    record.metrics.max_rss_growth_bytes = metrics["max_rss_growth_bytes"].AsDouble();
  }

  record.tolerances.cpu_p50_percent =
      tolerances["cpu_p50_percent"].AsDouble(record.tolerances.p50_percent);
  record.tolerances.cpu_p95_percent =
      tolerances["cpu_p95_percent"].AsDouble(record.tolerances.p95_percent);
  record.tolerances.cpu_max_percent =
      tolerances["cpu_max_percent"].AsDouble(record.tolerances.max_percent);
  record.tolerances.rss_mean_percent = tolerances["rss_mean_percent"].AsDouble(25.0);
  return record;
}

bool SaveBaseline(const std::filesystem::path& path, const BaselineRecord& baseline) {
  util::JsonObject root;
  root["scenario"] = baseline.scenario_name;
  util::JsonObject metrics{
      {"p50_wall_ms", baseline.metrics.p50_wall_ms},
      {"p95_wall_ms", baseline.metrics.p95_wall_ms},
      {"max_wall_ms", baseline.metrics.max_wall_ms},
      {"p50_allocations", baseline.metrics.p50_allocations},
      {"p95_allocations", baseline.metrics.p95_allocations},
      {"max_allocations", baseline.metrics.max_allocations},
      {"mean_rss_growth_bytes", baseline.metrics.mean_rss_growth_bytes},
      {"p50_rss_growth_bytes", baseline.metrics.p50_rss_growth_bytes},
      {"p95_rss_growth_bytes", baseline.metrics.p95_rss_growth_bytes},
      {"max_rss_growth_bytes", baseline.metrics.max_rss_growth_bytes},
  };
  // Omitted entirely, not written as zero, when the scenario opts out: the
  // loader decides `has_cpu_metrics` from whether p50_cpu_ms is a number, and a
  // zero baseline would gate every future run against 0 ms.
  if (baseline.has_cpu_metrics) {
    metrics["p50_cpu_ms"] = baseline.metrics.p50_cpu_ms;
    metrics["p95_cpu_ms"] = baseline.metrics.p95_cpu_ms;
    metrics["max_cpu_ms"] = baseline.metrics.max_cpu_ms;
  }
  // Same rule: omitted rather than zeroed when the run had no probe, because a
  // zero here would come back as `has_calibration` with a divide-by-zero clock.
  if (baseline.has_calibration && baseline.metrics.p50_cpu_calibration_ns > 0.0) {
    metrics["p50_cpu_calibration_ns"] = baseline.metrics.p50_cpu_calibration_ns;
  }
  root["metrics"] = std::move(metrics);
  root["tolerances"] = util::JsonObject{
      {"p50_percent", baseline.tolerances.p50_percent},
      {"p95_percent", baseline.tolerances.p95_percent},
      {"max_percent", baseline.tolerances.max_percent},
      {"alloc_p50_percent", baseline.tolerances.alloc_p50_percent},
      {"alloc_p95_percent", baseline.tolerances.alloc_p95_percent},
      {"alloc_max_percent", baseline.tolerances.alloc_max_percent},
      {"cpu_p50_percent", baseline.tolerances.cpu_p50_percent},
      {"cpu_p95_percent", baseline.tolerances.cpu_p95_percent},
      {"cpu_max_percent", baseline.tolerances.cpu_max_percent},
      {"rss_mean_percent", baseline.tolerances.rss_mean_percent},
  };
  std::ofstream out(path);
  if (!out) {
    return false;
  }
  out << util::SerializeJson(util::JsonValue(std::move(root)));
  return true;
}

BaselineComparison CompareToBaseline(const BaselineRecord& baseline, const Aggregate& aggregate) {
  BaselineComparison result;
  result.scenario_name = aggregate.scenario_name;
  AddMetric(&result, "p50_wall_ms", baseline.metrics.p50_wall_ms, aggregate.metrics.p50_wall_ms,
            baseline.tolerances.p50_percent);
  AddMetric(&result, "p95_wall_ms", baseline.metrics.p95_wall_ms, aggregate.metrics.p95_wall_ms,
            baseline.tolerances.p95_percent);
  AddMetric(&result, "max_wall_ms", baseline.metrics.max_wall_ms, aggregate.metrics.max_wall_ms,
            baseline.tolerances.max_percent);
  AddMetric(&result, "p50_allocations", baseline.metrics.p50_allocations,
            aggregate.metrics.p50_allocations, baseline.tolerances.alloc_p50_percent);
  AddMetric(&result, "p95_allocations", baseline.metrics.p95_allocations,
            aggregate.metrics.p95_allocations, baseline.tolerances.alloc_p95_percent);
  AddMetric(&result, "max_allocations", baseline.metrics.max_allocations,
            aggregate.metrics.max_allocations, baseline.tolerances.alloc_max_percent);
  if (baseline.has_cpu_metrics) {
    const NormalizedCpu cpu = NormalizeCpuAgainstBaselineClock(baseline, aggregate);
    result.clock = cpu.clock;
    AddMetric(&result, "p50_cpu_ms", baseline.metrics.p50_cpu_ms, cpu.metrics.p50_cpu_ms,
              baseline.tolerances.cpu_p50_percent, aggregate.metrics.p50_cpu_ms);
    AddMetric(&result, "p95_cpu_ms", baseline.metrics.p95_cpu_ms, cpu.metrics.p95_cpu_ms,
              baseline.tolerances.cpu_p95_percent, aggregate.metrics.p95_cpu_ms);
    AddMetric(&result, "max_cpu_ms", baseline.metrics.max_cpu_ms, cpu.metrics.max_cpu_ms,
              baseline.tolerances.cpu_max_percent, aggregate.metrics.max_cpu_ms);
  }
  if (baseline.has_rss_metrics) {
    // Resident growth is quantized to the 4 KiB page, so near zero a PERCENTAGE
    // envelope is the wrong instrument entirely: one page of jitter on a one-page
    // baseline is +50%, and no percentage that still catches real growth can absorb
    // it. multi_project_switch proved this — recorded at 4,096, it measured 6,144 on
    // the very next run and failed at +50% against a 25% envelope.
    //
    // The floor converts the gate to a flat absolute allowance in that regime. At
    // 64 KiB the allowance is 80 KiB, which absorbs several pages of allocator
    // jitter while still failing a scenario that newly retains ~80 KiB per
    // iteration (8 MB over a hundred operations). 90 of 93 baselines record exactly
    // zero growth, so nearly the whole suite gates on that flat allowance; the two
    // that genuinely grow (2.75 MB and 1.61 MB per iteration) are 30-40x above it
    // and gate on the percentage as intended.
    constexpr double kRssNoiseFloorBytes = 64.0 * 1024.0;
    const auto add_rss = [&](std::string_view name, double expected, double actual,
                             double tolerance_percent) {
      AddMetric(&result, name, std::max(expected, kRssNoiseFloorBytes), actual, tolerance_percent);
    };
    // ONE resident gate, on the trimmed mean, deliberately.
    //
    // p95 and max measure which iteration tripped an allocator arena expansion,
    // which is why they were never gated. But p50 turned out to be no better, in
    // two ways that only showed up once the readings were trimmed
    // (TD-2026-08-05-136) and the numbers got sharp enough to see:
    //
    //  - It sits on a mode boundary for any scenario that retains on SOME
    //    iterations. diff_stage_hunk_large_patch alternates 0 / ~220 KB, so its p50
    //    is decided by how many iterations happened to land in each mode: 218, 184
    //    and 324 KB across three runs of one unchanged binary, a 1.76x swing.
    //  - Worse, it is BLIND to that whole shape. merge_scroll_large_fixture retains
    //    ~972 KB per iteration on average and its p50 is exactly 0, so the gate
    //    read "this scenario grows by nothing" for a megabyte an iteration. Same
    //    for editor_sort_lines_large (p50 28 KB, mean 250 KB) and
    //    editor_scroll_fresh_content_large (p50 2 KB, mean 195 KB).
    //
    // The trimmed mean sees every iteration, so a scenario that retains on half of
    // them reports half the rate instead of zero, and dropping the single largest
    // sample keeps iteration 0's cold pass from owning it. Measured across the same
    // three runs it is stable where p50 was not: 1.02x on editor_sort_lines_large
    // against 1.14x, 1.12x on editor_surround_multi_caret against 1.68x, 1.001x on
    // merge_scroll_large_fixture. The percentiles stay recorded and reported.
    add_rss("mean_rss_growth_bytes", baseline.metrics.mean_rss_growth_bytes,
            aggregate.metrics.mean_rss_growth_bytes, baseline.tolerances.rss_mean_percent);
  }
  return result;
}

CalibrationSpread MeasureCalibrationSpread(const Aggregate& aggregate) {
  CalibrationSpread spread;
  for (const Iteration& iteration : aggregate.iterations) {
    // The metric field is the live path; the counter scan is the fallback for an
    // aggregate rebuilt from a report, where the probe only ever appears as a
    // named counter.
    std::uint64_t reading = iteration.metrics.cpu_calibration_ns;
    if (reading == 0) {
      for (const auto& [name, value] : iteration.perf_counters) {
        if (name == "harness.cpu_calibration_ns") {
          reading = value;
          break;
        }
      }
    }
    if (reading == 0) {
      continue;
    }
    if (!spread.valid) {
      spread.valid = true;
      spread.min_ns = reading;
      spread.max_ns = reading;
      continue;
    }
    spread.min_ns = std::min(spread.min_ns, reading);
    spread.max_ns = std::max(spread.max_ns, reading);
  }
  if (spread.valid && spread.min_ns != 0) {
    spread.ratio = static_cast<double>(spread.max_ns) / static_cast<double>(spread.min_ns);
  }
  return spread;
}

std::string DescribeCalibrationSpread(const CalibrationSpread& spread) {
  if (!spread.valid) {
    return {};
  }
  std::ostringstream out;
  out << "  [machine clock moved during the run: harness.cpu_calibration_ns "
      << (spread.min_ns / 1000) << "-" << (spread.max_ns / 1000) << "us, " << spread.ratio
      << "x — a duration metric scales with it; see TD-2026-08-05-137]";
  return out.str();
}

}  // namespace microide::tests::perf
