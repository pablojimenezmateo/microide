#include "perf/Baseline.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

#include "util/JsonValue.h"

namespace microide::tests::perf {
namespace {

bool IsNumber(const util::JsonValue& value) {
  return value.IsInt() || value.IsDouble();
}

double AllowedDelta(double expected, double tolerance_percent) {
  return std::abs(expected) * (tolerance_percent / 100.0);
}

bool WithinTolerance(double expected, double actual, double tolerance_percent) {
  return actual <= expected + AllowedDelta(expected, tolerance_percent);
}

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

struct NormalizedCpu {
  MetricSet metrics;
  ClockNormalization clock;
};

double WallClockCorrection(double clock_factor, double cpu_ms, double wall_ms) {
  if (wall_ms <= 0.0) {
    return 1.0;
  }
  const double cpu_fraction = std::clamp(cpu_ms / wall_ms, 0.0, 1.0);
  return 1.0 + (clock_factor - 1.0) * cpu_fraction;
}

NormalizedCpu NormalizeWallAgainstBaselineClock(const BaselineRecord& baseline,
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
    const double clock_factor =
        ClampNormalizationFactor(baseline_ns / iteration_ns, &out.clock.clamped);
    normalized.push_back(iteration.metrics.wall_ms *
                         WallClockCorrection(clock_factor, iteration.metrics.cpu_ms,
                                             iteration.metrics.wall_ms));
  }
  if (normalized.empty()) {
    return out;
  }
  out.metrics.p50_wall_ms = Percentile(normalized, 0.50);
  out.metrics.p95_wall_ms = Percentile(normalized, 0.95);
  out.metrics.max_wall_ms = *std::max_element(normalized.begin(), normalized.end());
  out.clock.applied = true;
  out.clock.factor = Percentile(measured_ns, 0.50) / baseline_ns;
  return out;
}

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

void RepointSettlingAllocationGateAtRecordedMax(BaselineComparison* result,
                                                std::size_t metric_index,
                                                double recorded_max,
                                                double max_tolerance_percent,
                                                std::size_t measured_iterations,
                                                std::size_t baseline_iterations,
                                                std::string_view metric_name) {
  if (baseline_iterations == 0 || measured_iterations == 0 ||
      measured_iterations >= baseline_iterations) {
    return;
  }
  MetricComparison& metric = result->metrics[metric_index];
  metric.expected = recorded_max;
  metric.tolerance_percent = max_tolerance_percent;
  metric.passed = WithinTolerance(recorded_max, metric.actual, max_tolerance_percent);
  std::ostringstream note;
  note << metric_name << " gated against the baseline's recorded MAX (" << recorded_max
       << "), not its median: this run measured " << measured_iterations
       << " iterations against a baseline recorded over " << baseline_iterations
       << ", and a median over that few samples of a settling series is the cold first "
          "iteration — which is the series' maximum, not its middle";
  metric.note = note.str();
  result->passed = true;
  for (const MetricComparison& entry : result->metrics) {
    if (entry.enforced && !entry.passed) {
      result->passed = false;
    }
  }
}

void AnnotateSettlingGateForIterationCount(BaselineComparison* result,
                                           std::size_t metric_index,
                                           const BaselineRecord& baseline,
                                           const Aggregate& aggregate,
                                           std::string_view metric_name,
                                           std::string_view statistic,
                                           std::string_view debt_id) {
  const std::size_t measured = aggregate.iterations.size();
  if (baseline.iterations == 0 || measured == 0 || measured == baseline.iterations) {
    return;
  }
  std::ostringstream note;
  if (measured < baseline.iterations) {
    note << metric_name << " NOT ENFORCED: this run measured " << measured
         << " iterations against a baseline recorded over " << baseline.iterations
         << ". The statistic is " << statistic
         << " over a settling series and reads high at short counts, so the comparison is "
            "not a measurement — rerun with --iterations="
         << baseline.iterations << " to gate it (" << debt_id << ")";
    Unenforce(result, metric_index, note.str());
    return;
  }
  note << metric_name << " gate is loose: this run measured " << measured
       << " iterations against a baseline recorded over " << baseline.iterations
       << ", and the statistic falls as iterations rise (" << debt_id << ")";
  result->metrics[metric_index].note = note.str();
}

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
    const std::size_t phase_index =
        AddMetric(out, metric_name, expected.p50_allocations, measured->p50_allocations,
                  baseline.tolerances.phase_alloc_p50_percent);
    RepointSettlingAllocationGateAtRecordedMax(
        out, phase_index, expected.max_allocations, baseline.tolerances.alloc_max_percent,
        aggregate.iterations.size(), baseline.iterations, metric_name);
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

void RefuseComparisonAcrossMeasurementRevisions(BaselineComparison* out,
                                                const BaselineRecord& baseline,
                                                const Aggregate& aggregate) {
  if (baseline.measurement_revision == aggregate.measurement_revision) {
    return;
  }
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

}

BaselineRecord MergeDeterministicMetrics(const BaselineRecord& existing,
                                         const BaselineRecord& fresh) {
  BaselineRecord merged = existing;
  merged.scenario_name = fresh.scenario_name;
  merged.tolerances = fresh.tolerances;
  merged.measurement_revision = fresh.measurement_revision;
  merged.build_config = fresh.build_config;
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
  record.tolerances.alloc_p50_percent =
      tolerances["alloc_p50_percent"].AsDouble(record.tolerances.p50_percent);
  record.tolerances.alloc_p95_percent =
      tolerances["alloc_p95_percent"].AsDouble(record.tolerances.p95_percent);
  record.tolerances.alloc_max_percent =
      tolerances["alloc_max_percent"].AsDouble(record.tolerances.max_percent);

  record.has_cpu_metrics = IsNumber(metrics["p50_cpu_ms"]);
  if (record.has_cpu_metrics) {
    record.metrics.p50_cpu_ms = metrics["p50_cpu_ms"].AsDouble();
    record.metrics.p95_cpu_ms = metrics["p95_cpu_ms"].AsDouble();
    record.metrics.max_cpu_ms = metrics["max_cpu_ms"].AsDouble();
  }
  record.has_calibration = IsNumber(metrics["p50_cpu_calibration_ns"]);
  if (record.has_calibration) {
    record.metrics.p50_cpu_calibration_ns = metrics["p50_cpu_calibration_ns"].AsDouble();
  }
  record.has_rss_metrics = IsNumber(metrics["mean_rss_growth_bytes"]);
  if (record.has_rss_metrics) {
    record.metrics.mean_rss_growth_bytes = metrics["mean_rss_growth_bytes"].AsDouble();
    record.metrics.p50_rss_growth_bytes = metrics["p50_rss_growth_bytes"].AsDouble();
    record.metrics.p95_rss_growth_bytes = metrics["p95_rss_growth_bytes"].AsDouble();
    record.metrics.max_rss_growth_bytes = metrics["max_rss_growth_bytes"].AsDouble();
  }
  record.has_net_heap_metrics = IsNumber(metrics["p50_net_heap_bytes"]);
  if (record.has_net_heap_metrics) {
    record.metrics.p50_net_heap_bytes = metrics["p50_net_heap_bytes"].AsDouble();
  }
  if (IsNumber((*parsed)["iterations"])) {
    const double iterations = (*parsed)["iterations"].AsDouble();
    record.iterations = iterations > 0.0 ? static_cast<std::size_t>(iterations) : 0;
  }

  if (IsNumber((*parsed)["measurement_revision"])) {
    const double revision = (*parsed)["measurement_revision"].AsDouble();
    record.measurement_revision = revision >= 1.0 ? static_cast<std::size_t>(revision) : 1;
  }
  record.timing_is_advisory = (*parsed)["timing_is_advisory"].AsBool(false);
  record.build_config = (*parsed)["build_config"].AsString();
  record.wall_spread_percent = (*parsed)["wall_spread_percent"].AsDouble(0.0);

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
  };
  if (baseline.has_rss_metrics) {
    metrics["mean_rss_growth_bytes"] = baseline.metrics.mean_rss_growth_bytes;
    metrics["p50_rss_growth_bytes"] = baseline.metrics.p50_rss_growth_bytes;
    metrics["p95_rss_growth_bytes"] = baseline.metrics.p95_rss_growth_bytes;
    metrics["max_rss_growth_bytes"] = baseline.metrics.max_rss_growth_bytes;
  }
  if (baseline.has_cpu_metrics) {
    metrics["p50_cpu_ms"] = baseline.metrics.p50_cpu_ms;
    metrics["p95_cpu_ms"] = baseline.metrics.p95_cpu_ms;
    metrics["max_cpu_ms"] = baseline.metrics.max_cpu_ms;
  }
  if (baseline.has_calibration && baseline.metrics.p50_cpu_calibration_ns > 0.0) {
    metrics["p50_cpu_calibration_ns"] = baseline.metrics.p50_cpu_calibration_ns;
  }
  if (baseline.has_net_heap_metrics) {
    metrics["p50_net_heap_bytes"] = baseline.metrics.p50_net_heap_bytes;
  }
  root["metrics"] = std::move(metrics);
  if (baseline.iterations > 0) {
    root["iterations"] = static_cast<std::int64_t>(baseline.iterations);
  }
  root["measurement_revision"] = static_cast<std::int64_t>(baseline.measurement_revision);
  if (baseline.timing_is_advisory) {
    root["timing_is_advisory"] = true;
  }
  if (!baseline.build_config.empty()) {
    root["build_config"] = baseline.build_config;
  }
  if (baseline.wall_spread_percent > 0.0) {
    root["wall_spread_percent"] = baseline.wall_spread_percent;
  }
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

void UnenforceAdvisoryTimingHalf(BaselineComparison* out, const BaselineRecord& baseline) {
  if (!baseline.timing_is_advisory) {
    return;
  }
  static constexpr std::string_view kMachineSensitive[] = {
      "p50_wall_ms",  "p95_wall_ms",         "max_wall_ms", "p50_cpu_ms",
      "p95_cpu_ms",   "max_cpu_ms",          "mean_rss_growth_bytes",
  };
  for (std::size_t index = 0; index < out->metrics.size(); ++index) {
    const std::string& metric = out->metrics[index].metric;
    const bool machine_sensitive =
        std::find(std::begin(kMachineSensitive), std::end(kMachineSensitive), metric) !=
        std::end(kMachineSensitive);
    if (!machine_sensitive || !out->metrics[index].enforced) {
      continue;
    }
    Unenforce(out, index,
              metric + " NOT ENFORCED: this baseline's timing half was recorded on an "
                       "advisory runner, so the number describes that machine as much as the "
                       "code. Rerun --update-baseline on the reference runner to arm it "
                       "(TD-2026-08-11-184)");
  }
}

void UnenforceMetricsThatMoveWithBuildConfig(BaselineComparison* out,
                                             const BaselineRecord& baseline,
                                             const Aggregate& aggregate) {
  if (baseline.build_config.empty() || aggregate.build_config.empty() ||
      baseline.build_config == aggregate.build_config) {
    return;
  }
  static constexpr std::string_view kConfigSensitive[] = {
      "p50_wall_ms", "p95_wall_ms", "max_wall_ms",           "p50_cpu_ms",
      "p95_cpu_ms",  "max_cpu_ms",  "mean_rss_growth_bytes", "p50_net_heap_bytes",
  };
  for (std::size_t index = 0; index < out->metrics.size(); ++index) {
    const std::string& metric = out->metrics[index].metric;
    const bool sensitive = std::find(std::begin(kConfigSensitive), std::end(kConfigSensitive),
                                     metric) != std::end(kConfigSensitive);
    if (!sensitive || !out->metrics[index].enforced) {
      continue;
    }
    Unenforce(out, index,
              metric + " NOT ENFORCED: this baseline was recorded from a '" +
                  baseline.build_config + "' build and this run is '" + aggregate.build_config +
                  "'. Retention and duration move with the build configuration, so this is "
                  "not a comparison — rebuild with `cmake --preset microide-perf` "
                  "(TD-2026-08-12-191)");
  }
}

inline constexpr double kWallSpreadSafetyFactor = 3.0;
inline constexpr double kWallSpreadFloorPercent = 25.0;

double EffectiveWallTolerance(double declared, double spread_percent) {
  if (!(spread_percent > 0.0)) {
    return declared;
  }
  const double derived =
      std::max(spread_percent * kWallSpreadSafetyFactor, kWallSpreadFloorPercent);
  return std::min(declared, derived);
}

BaselineComparison CompareToBaseline(const BaselineRecord& baseline, const Aggregate& aggregate) {
  BaselineComparison result;
  result.scenario_name = aggregate.scenario_name;
  const NormalizedCpu wall = NormalizeWallAgainstBaselineClock(baseline, aggregate);
  const double wall_spread = baseline.wall_spread_percent;
  AddMetric(&result, "p50_wall_ms", baseline.metrics.p50_wall_ms, wall.metrics.p50_wall_ms,
            EffectiveWallTolerance(baseline.tolerances.p50_percent, wall_spread),
            aggregate.metrics.p50_wall_ms);
  AddMetric(&result, "p95_wall_ms", baseline.metrics.p95_wall_ms, wall.metrics.p95_wall_ms,
            EffectiveWallTolerance(baseline.tolerances.p95_percent, wall_spread),
            aggregate.metrics.p95_wall_ms);
  AddMetric(&result, "max_wall_ms", baseline.metrics.max_wall_ms, wall.metrics.max_wall_ms,
            EffectiveWallTolerance(baseline.tolerances.max_percent, wall_spread),
            aggregate.metrics.max_wall_ms);
  result.clock = wall.clock;
  const std::size_t alloc_p50_index =
      AddMetric(&result, "p50_allocations", baseline.metrics.p50_allocations,
                aggregate.metrics.p50_allocations, baseline.tolerances.alloc_p50_percent);
  RepointSettlingAllocationGateAtRecordedMax(
      &result, alloc_p50_index, baseline.metrics.max_allocations,
      baseline.tolerances.alloc_max_percent, aggregate.iterations.size(), baseline.iterations,
      "p50_allocations");
  const std::size_t alloc_p95_index =
      AddMetric(&result, "p95_allocations", baseline.metrics.p95_allocations,
                aggregate.metrics.p95_allocations, baseline.tolerances.alloc_p95_percent);
  RepointSettlingAllocationGateAtRecordedMax(
      &result, alloc_p95_index, baseline.metrics.max_allocations,
      baseline.tolerances.alloc_max_percent, aggregate.iterations.size(), baseline.iterations,
      "p95_allocations");
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
    constexpr double kRssNoiseFloorBytes = 96.0 * 1024.0;
    const auto add_rss = [&](std::string_view name, double expected, double actual,
                             double tolerance_percent) {
      return AddMetric(&result, name, std::max(expected, kRssNoiseFloorBytes), actual,
                       tolerance_percent);
    };
    const std::size_t rss_index =
        add_rss("mean_rss_growth_bytes", baseline.metrics.mean_rss_growth_bytes,
                aggregate.metrics.mean_rss_growth_bytes, baseline.tolerances.rss_mean_percent);
    AnnotateSettlingGateForIterationCount(&result, rss_index, baseline, aggregate,
                                          "mean_rss_growth_bytes", "a trimmed mean",
                                          "TD-2026-08-06-148");
  }
  if (baseline.has_net_heap_metrics) {
    constexpr double kNetHeapNoiseFloorBytes = 4096.0;
    const double expected = baseline.metrics.p50_net_heap_bytes;
    const std::size_t net_heap_index = AddMetricWithAllowance(
        &result, "p50_net_heap_bytes", expected, aggregate.metrics.p50_net_heap_bytes,
        std::max(std::abs(expected) * (baseline.tolerances.net_heap_percent / 100.0),
                 kNetHeapNoiseFloorBytes));
    AnnotateSettlingGateForIterationCount(&result, net_heap_index, baseline, aggregate,
                                          "p50_net_heap_bytes", "a median",
                                          "TD-2026-08-10-173");
  }
  ComparePhaseAllocations(&result, baseline, aggregate);
  NoteTailOnlyAllocationDivergence(&result);
  UnenforceAdvisoryTimingHalf(&result, baseline);
  UnenforceMetricsThatMoveWithBuildConfig(&result, baseline, aggregate);
  RefuseComparisonAcrossMeasurementRevisions(&result, baseline, aggregate);
  return result;
}

CalibrationSpread MeasureCalibrationSpread(const Aggregate& aggregate) {
  CalibrationSpread spread;
  for (const Iteration& iteration : aggregate.iterations) {
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

}
