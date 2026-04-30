#include "TestSupport.h"

#include "perf/Baseline.h"

namespace microide::tests {
namespace {

perf::BaselineRecord MakeBaseline() {
  perf::BaselineRecord baseline;
  baseline.scenario_name = "test";
  baseline.metrics = perf::MetricSet{
      .p50_wall_ms = 100.0,
      .p95_wall_ms = 100.0,
      .max_wall_ms = 100.0,
      .p50_allocations = 100.0,
      .p95_allocations = 100.0,
      .max_allocations = 100.0,
  };
  baseline.tolerances = perf::Tolerances{
      .p50_percent = 10.0,
      .p95_percent = 20.0,
      .max_percent = 50.0,
  };
  return baseline;
}

void TestPerfBaselineComparisonAllowsImprovements() {
  const perf::BaselineRecord baseline = MakeBaseline();
  perf::Aggregate aggregate;
  aggregate.scenario_name = "test";
  aggregate.metrics = perf::MetricSet{
      .p50_wall_ms = 85.0,
      .p95_wall_ms = 75.0,
      .max_wall_ms = 60.0,
      .p50_allocations = 70.0,
      .p95_allocations = 60.0,
      .max_allocations = 55.0,
  };

  const perf::BaselineComparison comparison = perf::CompareToBaseline(baseline, aggregate);
  Expect(comparison.passed, "improved perf metrics should pass baseline comparison");
}

void TestPerfBaselineComparisonRejectsRegressions() {
  const perf::BaselineRecord baseline = MakeBaseline();
  perf::Aggregate aggregate;
  aggregate.scenario_name = "test";
  aggregate.metrics = perf::MetricSet{
      .p50_wall_ms = 111.0,
      .p95_wall_ms = 121.0,
      .max_wall_ms = 151.0,
      .p50_allocations = 111.0,
      .p95_allocations = 121.0,
      .max_allocations = 151.0,
  };

  const perf::BaselineComparison comparison = perf::CompareToBaseline(baseline, aggregate);
  Expect(!comparison.passed, "worse perf metrics beyond tolerance should fail baseline comparison");
}

}  // namespace

void RegisterPerfBaselineTests(std::vector<TestCase>& tests) {
  AddTest(tests, "PerfBaseline/AllowsImprovements", TestPerfBaselineComparisonAllowsImprovements);
  AddTest(tests, "PerfBaseline/RejectsRegressions", TestPerfBaselineComparisonRejectsRegressions);
}

}  // namespace microide::tests
