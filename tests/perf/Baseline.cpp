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
               double tolerance_percent) {
  MetricComparison metric{
      .metric = std::string(name),
      .expected = expected,
      .actual = actual,
      .tolerance_percent = tolerance_percent,
      .passed = WithinTolerance(expected, actual, tolerance_percent),
  };
  if (!metric.passed) {
    out->passed = false;
  }
  out->metrics.push_back(std::move(metric));
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
  record.has_rss_metrics = IsNumber(metrics["p50_rss_growth_bytes"]);
  if (record.has_rss_metrics) {
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
  record.tolerances.rss_p50_percent = tolerances["rss_p50_percent"].AsDouble(25.0);
  record.tolerances.rss_p95_percent = tolerances["rss_p95_percent"].AsDouble(35.0);
  record.tolerances.rss_max_percent = tolerances["rss_max_percent"].AsDouble(60.0);
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
      {"rss_p50_percent", baseline.tolerances.rss_p50_percent},
      {"rss_p95_percent", baseline.tolerances.rss_p95_percent},
      {"rss_max_percent", baseline.tolerances.rss_max_percent},
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
    AddMetric(&result, "p50_cpu_ms", baseline.metrics.p50_cpu_ms, aggregate.metrics.p50_cpu_ms,
              baseline.tolerances.cpu_p50_percent);
    AddMetric(&result, "p95_cpu_ms", baseline.metrics.p95_cpu_ms, aggregate.metrics.p95_cpu_ms,
              baseline.tolerances.cpu_p95_percent);
    AddMetric(&result, "max_cpu_ms", baseline.metrics.max_cpu_ms, aggregate.metrics.max_cpu_ms,
              baseline.tolerances.cpu_max_percent);
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
    // p50 ONLY, deliberately. Resident growth is bursty: the iteration that happens
    // to trip an allocator arena expansion pays for it and the rest pay nothing, so
    // p95 and max measure which iteration got unlucky rather than anything about the
    // code. editor_column_selection_burst records p50 = 10 KiB against p95 = 9 MiB
    // from exactly that -- gating either would be a permanent coin flip. The p95/max
    // numbers are still recorded and reported; they are just not a gate.
    add_rss("p50_rss_growth_bytes", baseline.metrics.p50_rss_growth_bytes,
            aggregate.metrics.p50_rss_growth_bytes, baseline.tolerances.rss_p50_percent);
  }
  return result;
}

CalibrationSpread MeasureCalibrationSpread(const Aggregate& aggregate) {
  CalibrationSpread spread;
  for (const Iteration& iteration : aggregate.iterations) {
    for (const auto& [name, value] : iteration.perf_counters) {
      if (name != "harness.cpu_calibration_ns" || value == 0) {
        continue;
      }
      if (!spread.valid) {
        spread.valid = true;
        spread.min_ns = value;
        spread.max_ns = value;
        continue;
      }
      spread.min_ns = std::min(spread.min_ns, value);
      spread.max_ns = std::max(spread.max_ns, value);
    }
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
