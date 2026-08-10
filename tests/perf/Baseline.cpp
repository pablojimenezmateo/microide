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

// Returns the INDEX of the metric just added, not a reference into the vector:
// a later AddMetric reallocates, and a caller holding a reference across one is
// a dangling-pointer bug waiting for the next metric to be added.
std::size_t AddMetric(BaselineComparison* out,
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
  return out->metrics.size() - 1;
}

// AddMetric for a gate whose allowance is an absolute quantity rather than a
// percentage of the baseline.
//
// Every other metric here is non-negative and far from zero, so `expected *
// tolerance%` is both the allowance and a readable description of it. Net heap
// retention is neither: it is signed, and its most desirable value is exactly
// zero, where a percentage envelope has zero width. The tolerance is still
// REPORTED as a percentage (the equivalent one) so verdict lines, the JSON report
// and the headroom ranking keep one vocabulary -- but at expected == 0 there is
// no such percentage, and reporting 0% there would read as "gated to the byte"
// when the gate is a page wide. That case reports the allowance's own scale.
//
// Known limit, stated rather than hidden: `EnvelopeUsedPercent` divides by
// `expected`, so a gate whose baseline is exactly zero contributes nothing to the
// headroom ranking. That is tolerable HERE and only here -- a scenario recorded
// at zero net retention that starts retaining does not creep toward a 4 KB
// allowance, it blows past it by orders of magnitude and fails outright.
std::size_t AddMetricWithAllowance(BaselineComparison* out,
                                   std::string_view name,
                                   double expected,
                                   double actual,
                                   double allowance) {
  MetricComparison metric{
      .metric = std::string(name),
      .expected = expected,
      .actual = actual,
      .tolerance_percent = std::abs(expected) > 0.0 ? allowance / std::abs(expected) * 100.0 : 100.0,
      .passed = actual <= expected + allowance,
      .raw_actual = actual,
  };
  if (!metric.passed) {
    out->passed = false;
  }
  out->metrics.push_back(std::move(metric));
  return out->metrics.size() - 1;
}

// Stop enforcing a metric that was already added, and say why.
//
// The measurement stays in the comparison — it is still the most useful number
// in the room — but it no longer decides the run. Recomputing `passed` from the
// remaining enforced metrics rather than flipping the bit keeps one definition
// of "this scenario is red".
void Unenforce(BaselineComparison* out, std::size_t index, std::string reason) {
  out->metrics[index].enforced = false;
  out->metrics[index].note = std::move(reason);
  out->passed = true;
  for (const MetricComparison& entry : out->metrics) {
    if (entry.enforced && !entry.passed) {
      out->passed = false;
    }
  }
}

std::size_t AddMetric(BaselineComparison* out,
                      std::string_view name,
                      double expected,
                      double actual,
                      double tolerance_percent) {
  return AddMetric(out, name, expected, actual, tolerance_percent, actual);
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

// Decide whether the resident gate is comparable at this run's iteration count,
// and annotate it either way.
//
// `mean_rss_growth_bytes` is a trimmed mean over a series that SETTLES: the first
// iterations pay arena growth and first-touch faults that later ones do not, so
// averaging fewer of them reads high. That is not jitter and it does not average
// out — typing_large_file measured 100-114 KB at `--iterations=6` and 84-95 KB at
// the default 10, five runs a side on one quiet box, one binary. Against an
// 80,555 baseline with a +25% envelope that is a red gate half the time at 6 and
// a comfortable pass at 10, and the failure line said "measured=113869 (+41%)"
// with nothing to suggest the sample size was the reason (TD-2026-08-06-148).
//
// So: a run shorter than the baseline is not compared. Reporting the number and
// declining to gate it beats a red that means nothing — a developer who drops
// --iterations to save time gets an explanation, not a bisect.
//
// The other direction is left enforced. A LONGER run reads lower, so the gate can
// only get more permissive, which is a note rather than a failure — but it is
// still worth saying, because a permissive gate that nobody knows is permissive
// is how a baseline set goes quietly vacuous (validation-traps.md).
void AnnotateResidentGateForIterationCount(BaselineComparison* result,
                                           std::size_t metric_index,
                                           const BaselineRecord& baseline,
                                           const Aggregate& aggregate) {
  const std::size_t measured = aggregate.iterations.size();
  // Either side unknown: a pre-iteration-count baseline, or an aggregate built
  // from summary metrics alone (the unit tests, and report replay). Compare as
  // before rather than inventing a count.
  if (baseline.iterations == 0 || measured == 0 || measured == baseline.iterations) {
    return;
  }
  std::ostringstream note;
  if (measured < baseline.iterations) {
    note << "mean_rss_growth_bytes NOT ENFORCED: this run averaged " << measured
         << " iterations against a baseline recorded over " << baseline.iterations
         << ". The statistic is a trimmed mean over a settling series and reads high at "
            "short counts, so the comparison is not a measurement — rerun with "
            "--iterations=" << baseline.iterations << " to gate it (TD-2026-08-06-148)";
    Unenforce(result, metric_index, note.str());
    return;
  }
  note << "resident gate is loose: this run averaged " << measured
       << " iterations against a baseline recorded over " << baseline.iterations
       << ", and the trimmed mean falls as iterations rise (TD-2026-08-06-148)";
  result->metrics[metric_index].note = note.str();
}

// Gate every phase the baseline recorded, and say out loud when a phase the run
// measured has no baseline to be gated against.
//
// This is the instrument TD-2026-08-06-153 was filed for. A scenario total is
// dominated by setup on almost every shell scenario -- search_first_result's
// tight, deterministic 20,192-allocation gate is 99.3% project-open, so the
// search it is named after could decuple without moving it past its envelope.
// The phase is what the scenario claims to measure.
//
// Two failure modes, both deliberate:
//
//  - A baseline phase MISSING from the run fails. That is not pedantry: a
//    Measure() call deleted or renamed in a refactor silently removes a gate,
//    and a gate that disappears without a word is exactly how this suite went
//    quietly vacuous before (validation-traps.md). Renaming a phase is a
//    rebaseline, like every other deliberate change to what is measured.
//  - A measured phase with NO baseline is reported, unenforced, as one note per
//    scenario. It is not a failure (a new phase must be addable without a red
//    run), but it must not be invisible either -- an ungated phase that nobody
//    knows is ungated is the whole subject of the entry.
void ComparePhaseAllocations(BaselineComparison* out,
                             const BaselineRecord& baseline,
                             const Aggregate& aggregate) {
  const auto find_measured = [&](std::string_view name) -> const PhaseMetricSet* {
    for (const PhaseMetricSet& phase : aggregate.phases) {
      if (phase.name == name) {
        return &phase;
      }
    }
    return nullptr;
  };

  for (const PhaseMetricSet& expected : baseline.phases) {
    const std::string metric_name = "phase[" + expected.name + "].p50_allocations";
    const PhaseMetricSet* measured = find_measured(expected.name);
    if (measured == nullptr) {
      MetricComparison missing{
          .metric = metric_name,
          .expected = expected.p50_allocations,
          .actual = 0.0,
          .tolerance_percent = baseline.tolerances.phase_alloc_p50_percent,
          .passed = false,
          .raw_actual = 0.0,
      };
      missing.note = "phase '" + expected.name +
                     "' is gated by the baseline but was not measured by this run: the gate is "
                     "gone, not passing. Restore the Measure() call or rebaseline the scenario "
                     "(TD-2026-08-06-153)";
      out->passed = false;
      out->metrics.push_back(std::move(missing));
      continue;
    }
    AddMetric(out, metric_name, expected.p50_allocations, measured->p50_allocations,
              baseline.tolerances.phase_alloc_p50_percent);
  }

  std::string ungated;
  std::size_t ungated_count = 0;
  for (const PhaseMetricSet& measured : aggregate.phases) {
    const bool gated = std::any_of(baseline.phases.begin(), baseline.phases.end(),
                                   [&](const PhaseMetricSet& expected) {
                                     return expected.name == measured.name;
                                   });
    if (gated) {
      continue;
    }
    if (++ungated_count <= 3) {
      ungated += (ungated.empty() ? "" : ", ") + measured.name;
    }
  }
  if (ungated_count == 0) {
    return;
  }
  MetricComparison note{
      .metric = "phases_not_in_baseline",
      .expected = 0.0,
      .actual = static_cast<double>(ungated_count),
      .tolerance_percent = 0.0,
      .passed = true,
      .raw_actual = static_cast<double>(ungated_count),
      .enforced = false,
  };
  note.note = std::to_string(ungated_count) + " measured phase(s) NOT GATED (" + ungated +
              (ungated_count > 3 ? ", ..." : "") +
              "): this baseline predates phase gating, so only the scenario total is enforced — "
              "rerun --update-baseline to gate what the scenario actually measures "
              "(TD-2026-08-06-153)";
  out->metrics.push_back(std::move(note));
}

// Name the one shape that means "the harness moved, not the code".
//
// A code regression moves the MEDIAN. When `p50_allocations` matches its
// baseline to the allocation while `p95`/`max` are multiples of theirs, the extra
// work happened in one iteration of ten and is therefore a property of the
// process, not of the scenario: exactly what per-scenario isolation did to 34 of
// 100 scenarios by giving each one its own first-ever painted frame
// (TD-2026-08-10-168, ~8,600 allocations in iteration 1, p50 matching exactly in
// every case). Diagnosing that took a human noticing the pattern while chasing
// something unrelated, and 168 closed with "nothing gates this" left open.
//
// This does not decide the verdict — the tail gate already failed on its own
// merits, and a real tail regression exists. It attaches the reading that
// separates the two, so the next occurrence is a sentence on the verdict line
// instead of an afternoon.
constexpr double kTailDivergenceP50MatchPercent = 1.0;
constexpr double kTailDivergenceTailFactor = 2.0;

void NoteTailOnlyAllocationDivergence(BaselineComparison* out) {
  const auto find = [&](std::string_view name) -> MetricComparison* {
    for (MetricComparison& metric : out->metrics) {
      if (metric.metric == name) {
        return &metric;
      }
    }
    return nullptr;
  };
  const MetricComparison* p50 = find("p50_allocations");
  if (p50 == nullptr || p50->expected <= 0.0) {
    return;
  }
  const double p50_delta =
      std::abs(p50->actual - p50->expected) / p50->expected * 100.0;
  if (p50_delta > kTailDivergenceP50MatchPercent) {
    return;
  }
  for (const std::string_view name : {"p95_allocations", "max_allocations"}) {
    MetricComparison* tail = find(name);
    if (tail == nullptr || tail->passed || tail->expected <= 0.0) {
      continue;
    }
    const double factor = tail->actual / tail->expected;
    if (factor < kTailDivergenceTailFactor) {
      continue;
    }
    std::ostringstream note;
    note << "TAIL-ONLY DIVERGENCE: p50_allocations matches the baseline (" << p50->actual
         << " vs " << p50->expected << ") while this tail is " << factor
         << "x. A code regression moves the median, so the extra work is in one iteration "
            "and is a property of the PROCESS, not the scenario — suspect the harness "
            "regime the baseline was recorded in before suspecting the code "
            "(TD-2026-08-10-168)";
    if (tail->note.empty()) {
      tail->note = note.str();
    } else {
      tail->note += "; " + note.str();
    }
  }
}

// A baseline recorded at a different revision of the scenario is not a baseline
// for this scenario. Report every number, enforce none of them, and go red on
// the mismatch itself so the run says "rerecord" instead of "regression".
void RefuseComparisonAcrossMeasurementRevisions(BaselineComparison* out,
                                                const BaselineRecord& baseline,
                                                const Aggregate& aggregate) {
  if (baseline.measurement_revision == aggregate.measurement_revision) {
    return;
  }
  // Unenforced with no per-metric note, deliberately. Every gate in the scenario
  // is disqualified by the same one fact, and repeating it fourteen times on the
  // verdict line buries the one sentence that matters under its own explanation.
  for (MetricComparison& metric : out->metrics) {
    metric.enforced = false;
  }
  MetricComparison mismatch{
      .metric = "measurement_revision",
      .expected = static_cast<double>(baseline.measurement_revision),
      .actual = static_cast<double>(aggregate.measurement_revision),
      .tolerance_percent = 0.0,
      .passed = false,
      .raw_actual = static_cast<double>(aggregate.measurement_revision),
  };
  mismatch.note =
      "NOT COMPARABLE: this baseline was recorded at measurement_revision " +
      std::to_string(baseline.measurement_revision) + " and the scenario now declares " +
      std::to_string(aggregate.measurement_revision) +
      ", so it measures different work. Every other metric here is a reading, not a gate. "
      "Run --update-baseline for it on the reference runner; do NOT difference the old and "
      "new numbers as if they described the same work (TD-2026-08-07-167)";
  out->passed = false;
  out->metrics.push_back(std::move(mismatch));
}

}  // namespace

BaselineRecord MergeDeterministicMetrics(const BaselineRecord& existing,
                                         const BaselineRecord& fresh) {
  // Start from `existing`, not from `fresh`: the default has to be "keep what is
  // committed", so a metric added to MetricSet later is preserved by omission
  // rather than silently overwritten with a reading this run was not entitled to
  // take.
  BaselineRecord merged = existing;
  merged.scenario_name = fresh.scenario_name;
  // Tolerances come from the scenario definition on either path — a deliberate
  // widening lives in the Scenario struct precisely so a rebaseline cannot reset
  // it (see Scenario::tolerance_rss_percent).
  merged.tolerances = fresh.tolerances;
  // Same reasoning as tolerances: the revision is declared in the Scenario, so a
  // rebaseline of any kind stamps what the code says today. A deterministic
  // rebaseline carries the old wall numbers forward, which is fine — a scenario
  // whose revision moved needs a full rerecord anyway, and stamping the new
  // revision is what lets the run stop shouting about it.
  merged.measurement_revision = fresh.measurement_revision;
  merged.metrics.p50_allocations = fresh.metrics.p50_allocations;
  merged.metrics.p95_allocations = fresh.metrics.p95_allocations;
  merged.metrics.max_allocations = fresh.metrics.max_allocations;
  merged.metrics.p50_net_heap_bytes = fresh.metrics.p50_net_heap_bytes;
  merged.has_net_heap_metrics = fresh.has_net_heap_metrics;

  std::vector<PhaseMetricSet> phases;
  phases.reserve(fresh.phases.size());
  for (const PhaseMetricSet& measured : fresh.phases) {
    const auto prior = std::find_if(
        existing.phases.begin(), existing.phases.end(),
        [&](const PhaseMetricSet& entry) { return entry.name == measured.name; });
    if (prior == existing.phases.end()) {
      phases.push_back(measured);
      continue;
    }
    PhaseMetricSet phase = measured;
    phase.p50_wall_ms = prior->p50_wall_ms;
    phases.push_back(std::move(phase));
  }
  merged.phases = std::move(phases);
  return merged;
}

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
  // Net heap retention. Same rule again: absent on every baseline written before
  // the metric existed, and those stay ungated on it rather than being compared
  // against an implicit zero.
  record.has_net_heap_metrics = IsNumber(metrics["p50_net_heap_bytes"]);
  if (record.has_net_heap_metrics) {
    record.metrics.p50_net_heap_bytes = metrics["p50_net_heap_bytes"].AsDouble();
  }
  // How many iterations this baseline averaged. Absent on baselines written
  // before the field existed; those keep gating the resident mean unconditionally,
  // exactly as they did, and start declining short runs when they are next
  // re-recorded.
  if (IsNumber((*parsed)["iterations"])) {
    const double iterations = (*parsed)["iterations"].AsDouble();
    record.iterations = iterations > 0.0 ? static_cast<std::size_t>(iterations) : 0;
  }

  // Absent means 1: every scenario declares 1 until somebody changes what it
  // measures, so a pre-field baseline is a revision-1 baseline and compares
  // exactly as it did (TD-2026-08-07-167).
  if (IsNumber((*parsed)["measurement_revision"])) {
    const double revision = (*parsed)["measurement_revision"].AsDouble();
    record.measurement_revision = revision >= 1.0 ? static_cast<std::size_t>(revision) : 1;
  }

  record.tolerances.cpu_p50_percent =
      tolerances["cpu_p50_percent"].AsDouble(record.tolerances.p50_percent);
  record.tolerances.cpu_p95_percent =
      tolerances["cpu_p95_percent"].AsDouble(record.tolerances.p95_percent);
  record.tolerances.cpu_max_percent =
      tolerances["cpu_max_percent"].AsDouble(record.tolerances.max_percent);
  record.tolerances.rss_mean_percent = tolerances["rss_mean_percent"].AsDouble(25.0);
  record.tolerances.net_heap_percent = tolerances["net_heap_percent"].AsDouble(10.0);
  record.tolerances.phase_alloc_p50_percent =
      tolerances["phase_alloc_p50_percent"].AsDouble(record.tolerances.alloc_p50_percent);

  // Per-phase allocation baselines. Absent on every baseline written before
  // phase gating existed; those keep gating on the scenario total alone, and
  // CompareToBaseline says on the verdict line that the phases went ungated
  // rather than letting the omission be silent.
  const util::JsonValue& phases = (*parsed)["phases"];
  if (phases.IsArray()) {
    for (const util::JsonValue& phase : phases.AsArray()) {
      if (!phase.IsObject() || !phase["name"].IsString()) {
        continue;
      }
      PhaseMetricSet entry;
      entry.name = phase["name"].AsString();
      entry.p50_allocations = phase["p50_allocations"].AsDouble();
      entry.max_allocations = phase["max_allocations"].AsDouble();
      entry.p50_wall_ms = phase["p50_wall_ms"].AsDouble();
      const double iterations = phase["iterations"].AsDouble();
      entry.iterations = iterations > 0.0 ? static_cast<std::size_t>(iterations) : 0;
      record.phases.push_back(std::move(entry));
    }
  }
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
  // Written whenever the run recorded it, INCLUDING when it is exactly zero: zero
  // net retention is a real and desirable reading, and the loader keys on
  // presence rather than on the value.
  if (baseline.has_net_heap_metrics) {
    metrics["p50_net_heap_bytes"] = baseline.metrics.p50_net_heap_bytes;
  }
  root["metrics"] = std::move(metrics);
  // Omitted when unknown, for the same reason the metrics above are: a zero here
  // would come back as "recorded over 0 iterations", and every run would be
  // longer than that.
  if (baseline.iterations > 0) {
    root["iterations"] = static_cast<std::int64_t>(baseline.iterations);
  }
  // Always written, including the default 1: the whole value of the field is
  // that a reader can tell "revision 1" from "this file predates the idea", and
  // omitting the common case would throw that away for every baseline that has
  // not yet needed a bump.
  root["measurement_revision"] = static_cast<std::int64_t>(baseline.measurement_revision);
  // Written whenever the run measured phases at all. A scenario with no
  // Measure() call writes no "phases" key, which is the same state as a
  // pre-phase-gating baseline and compares the same way.
  if (!baseline.phases.empty()) {
    util::JsonArray phases;
    phases.reserve(baseline.phases.size());
    for (const PhaseMetricSet& phase : baseline.phases) {
      phases.push_back(util::JsonObject{
          {"name", phase.name},
          {"p50_allocations", phase.p50_allocations},
          {"max_allocations", phase.max_allocations},
          {"p50_wall_ms", phase.p50_wall_ms},
          {"iterations", static_cast<std::int64_t>(phase.iterations)},
      });
    }
    root["phases"] = std::move(phases);
  }
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
      {"net_heap_percent", baseline.tolerances.net_heap_percent},
      {"phase_alloc_p50_percent", baseline.tolerances.phase_alloc_p50_percent},
  };
  std::ofstream out(path);
  if (!out) {
    return false;
  }
  out << util::SerializeJson(util::JsonValue(std::move(root)));
  return true;
}

double MetricDeltaPercent(const MetricComparison& metric) {
  if (metric.expected == 0.0) {
    return 0.0;
  }
  return (metric.actual / metric.expected - 1.0) * 100.0;
}

double EnvelopeUsedPercent(const MetricComparison& metric) {
  if (metric.expected == 0.0 || metric.tolerance_percent <= 0.0) {
    return 0.0;
  }
  return MetricDeltaPercent(metric) / metric.tolerance_percent * 100.0;
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
      return AddMetric(&result, name, std::max(expected, kRssNoiseFloorBytes), actual,
                       tolerance_percent);
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
    const std::size_t rss_index =
        add_rss("mean_rss_growth_bytes", baseline.metrics.mean_rss_growth_bytes,
                aggregate.metrics.mean_rss_growth_bytes, baseline.tolerances.rss_mean_percent);
    AnnotateResidentGateForIterationCount(&result, rss_index, baseline, aggregate);
  }
  if (baseline.has_net_heap_metrics) {
    // The deterministic half of the memory gate (TD-2026-08-06-150). Unlike the
    // resident mean this reproduces to the byte across process states, so it gets
    // an allocation-class envelope and it is the gate that a retention regression
    // is expected to trip.
    //
    // The floor exists for the same reason the resident one does, at a different
    // scale. A scenario that nets exactly zero -- the desirable reading -- would
    // otherwise get a zero-width gate and fail on the first byte, and "retains
    // nothing" is precisely the state worth being able to record. One page of
    // allowance is ~450x the worst cross-process spread ever measured for this
    // metric (9 bytes) and still fails a scenario that newly holds 4 KB per
    // iteration.
    constexpr double kNetHeapNoiseFloorBytes = 4096.0;
    const double expected = baseline.metrics.p50_net_heap_bytes;
    AddMetricWithAllowance(
        &result, "p50_net_heap_bytes", expected, aggregate.metrics.p50_net_heap_bytes,
        std::max(std::abs(expected) * (baseline.tolerances.net_heap_percent / 100.0),
                 kNetHeapNoiseFloorBytes));
  }
  ComparePhaseAllocations(&result, baseline, aggregate);
  NoteTailOnlyAllocationDivergence(&result);
  RefuseComparisonAcrossMeasurementRevisions(&result, baseline, aggregate);
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
