#include "perf/Baseline.h"

#include <cmath>
#include <fstream>

#include "util/JsonValue.h"

namespace microide::tests::perf {
namespace {

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
  return record;
}

bool SaveBaseline(const std::filesystem::path& path, const BaselineRecord& baseline) {
  util::JsonObject root;
  root["scenario"] = baseline.scenario_name;
  root["metrics"] = util::JsonObject{
      {"p50_wall_ms", baseline.metrics.p50_wall_ms},
      {"p95_wall_ms", baseline.metrics.p95_wall_ms},
      {"max_wall_ms", baseline.metrics.max_wall_ms},
      {"p50_allocations", baseline.metrics.p50_allocations},
      {"p95_allocations", baseline.metrics.p95_allocations},
      {"max_allocations", baseline.metrics.max_allocations},
  };
  root["tolerances"] = util::JsonObject{
      {"p50_percent", baseline.tolerances.p50_percent},
      {"p95_percent", baseline.tolerances.p95_percent},
      {"max_percent", baseline.tolerances.max_percent},
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
            aggregate.metrics.p50_allocations, baseline.tolerances.p50_percent);
  AddMetric(&result, "p95_allocations", baseline.metrics.p95_allocations,
            aggregate.metrics.p95_allocations, baseline.tolerances.p95_percent);
  AddMetric(&result, "max_allocations", baseline.metrics.max_allocations,
            aggregate.metrics.max_allocations, baseline.tolerances.max_percent);
  return result;
}

}  // namespace microide::tests::perf
