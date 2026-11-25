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

void TestPerfEnvelopeConsumptionSeesADriftThatPassed() {
  const auto metric = [](double expected, double actual, double tolerance) {
    perf::MetricComparison m;
    m.metric = "p50_allocations";
    m.expected = expected;
    m.actual = actual;
    m.tolerance_percent = tolerance;
    m.raw_actual = actual;
    m.passed = actual <= expected * (1.0 + tolerance / 100.0);
    return m;
  };

  const perf::MetricComparison drifted = metric(5010.0, 5482.0, 10.0);
  Expect(drifted.passed, "the drift under test is one the gate PASSED — that is the point");
  const double used = perf::EnvelopeUsedPercent(drifted);
  Expect(used > 90.0 && used < 100.0,
         "a +9.4% move against a +10% envelope must report as ~94% of the envelope");
  Expect(used >= perf::kEnvelopeNoticePercent,
         "the drift that went unnoticed for two days must clear the notice threshold");

  Expect(perf::EnvelopeUsedPercent(metric(5010.0, 5010.0, 10.0)) == 0.0,
         "an unchanged measurement consumes none of its envelope");
  Expect(perf::EnvelopeUsedPercent(metric(5010.0, 5100.0, 10.0)) < perf::kEnvelopeNoticePercent,
         "a small move well inside the envelope must not be reported");

  Expect(perf::EnvelopeUsedPercent(metric(5010.0, 2101.0, 10.0)) < 0.0,
         "code that got faster must consume negative envelope, not positive");

  const perf::MetricComparison failed = metric(5010.0, 6000.0, 10.0);
  Expect(!failed.passed, "the control must actually fail");
  Expect(perf::EnvelopeUsedPercent(failed) > 100.0,
         "a metric past its tolerance must consume more than the whole envelope");

  Expect(perf::EnvelopeUsedPercent(metric(0.0, 10.0, 10.0)) == 0.0,
         "a zero baseline has no ratio to report");
  Expect(perf::EnvelopeUsedPercent(metric(100.0, 110.0, 0.0)) == 0.0,
         "a zero tolerance has no envelope to consume");
}

void TestPerfEnvelopeConsumptionAgreesWithTheGate() {
  const perf::BaselineRecord baseline = MakeBaseline();
  for (const double measured : {50.0, 99.0, 100.0, 105.0, 109.9, 110.0, 110.1, 150.0, 300.0}) {
    perf::Aggregate aggregate;
    aggregate.scenario_name = "test";
    aggregate.metrics = perf::MetricSet{
        .p50_wall_ms = measured,
        .p95_wall_ms = 100.0,
        .max_wall_ms = 100.0,
        .p50_allocations = measured,
        .p95_allocations = 100.0,
        .max_allocations = 100.0,
    };
    const perf::BaselineComparison comparison = perf::CompareToBaseline(baseline, aggregate);
    for (const perf::MetricComparison& metric : comparison.metrics) {
      const double used = perf::EnvelopeUsedPercent(metric);
      if (used > 100.0 + 1e-9) {
        Expect(!metric.passed,
               "a metric reported past its whole envelope must be one the gate failed");
      } else if (used < 100.0 - 1e-9) {
        Expect(metric.passed,
               "a metric reported inside its envelope must be one the gate passed");
      }
    }
  }
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

void TestPerfBaselineDecouplesWallAndAllocationTolerances() {
  perf::BaselineRecord baseline = MakeBaseline();
  baseline.tolerances = perf::Tolerances{
      .p50_percent = 75.0,
      .p95_percent = 250.0,
      .max_percent = 400.0,
      .alloc_p50_percent = 10.0,
      .alloc_p95_percent = 20.0,
      .alloc_max_percent = 50.0,
  };

  perf::Aggregate wall_only;
  wall_only.scenario_name = "test";
  wall_only.metrics = perf::MetricSet{
      .p50_wall_ms = 160.0,
      .p95_wall_ms = 160.0,
      .max_wall_ms = 160.0,
      .p50_allocations = 105.0,
      .p95_allocations = 105.0,
      .max_allocations = 105.0,
  };
  Expect(perf::CompareToBaseline(baseline, wall_only).passed,
         "a wall rise within the wide wall envelope with flat allocations should pass");

  perf::Aggregate alloc_regression = wall_only;
  alloc_regression.metrics.p50_allocations = 120.0;
  Expect(!perf::CompareToBaseline(baseline, alloc_regression).passed,
         "an allocation rise beyond the tight allocation envelope must fail even when wall is wide");
}

void TestPerfBaselineLoadDefaultsAllocationToleranceToWall() {
  TemporaryDirectory temp;
  const std::filesystem::path path = temp.path() / "legacy_baseline.json";
  WriteFile(path,
            R"({"scenario":"legacy","metrics":{"p50_wall_ms":10,"p95_wall_ms":10,)"
            R"("max_wall_ms":10,"p50_allocations":10,"p95_allocations":10,"max_allocations":10},)"
            R"("tolerances":{"p50_percent":30,"p95_percent":60,"max_percent":90}})");
  const std::optional<perf::BaselineRecord> loaded = perf::LoadBaseline(path);
  Expect(loaded.has_value(), "legacy baseline without allocation tolerances should load");
  Expect(loaded->tolerances.alloc_p50_percent == 30.0,
         "missing alloc p50 tolerance should inherit the wall p50 tolerance");
  Expect(loaded->tolerances.alloc_p95_percent == 60.0,
         "missing alloc p95 tolerance should inherit the wall p95 tolerance");
  Expect(loaded->tolerances.alloc_max_percent == 90.0,
         "missing alloc max tolerance should inherit the wall max tolerance");
}

void TestPerfBaselineToleranceRoundTrip() {
  TemporaryDirectory temp;
  const std::filesystem::path path = temp.path() / "roundtrip_baseline.json";
  perf::BaselineRecord baseline = MakeBaseline();
  baseline.tolerances = perf::Tolerances{
      .p50_percent = 75.0,
      .p95_percent = 250.0,
      .max_percent = 400.0,
      .alloc_p50_percent = 10.0,
      .alloc_p95_percent = 20.0,
      .alloc_max_percent = 50.0,
  };
  Expect(perf::SaveBaseline(path, baseline), "baseline should save");
  const std::optional<perf::BaselineRecord> loaded = perf::LoadBaseline(path);
  Expect(loaded.has_value(), "baseline should reload");
  Expect(loaded->tolerances.p50_percent == 75.0 && loaded->tolerances.alloc_p50_percent == 10.0,
         "distinct wall and allocation p50 tolerances should round-trip");
  Expect(loaded->tolerances.max_percent == 400.0 && loaded->tolerances.alloc_max_percent == 50.0,
         "distinct wall and allocation max tolerances should round-trip");
}

void TestPerfRecipesDoNotPinAVideoDriver() {
  const std::filesystem::path repo_root = TestRoot().parent_path();
  const std::filesystem::path scanned[] = {
      repo_root / "dev-docs" / "performance" / "perf-harness.md",
      repo_root / "tools" / "perf-compare.py",
  };

  std::size_t invocations_seen = 0;
  for (const std::filesystem::path& path : scanned) {
    Expect(std::filesystem::exists(path),
           "perf recipe file should exist: " + path.string());
    const std::string text = ReadFile(path);
    const std::size_t mentions = [&] {
      std::size_t count = 0;
      for (std::size_t at = text.find("microide_perf"); at != std::string::npos;
           at = text.find("microide_perf", at + 1)) {
        ++count;
      }
      return count;
    }();
    Expect(mentions > 0, "perf recipe should still name microide_perf: " + path.string());
    invocations_seen += mentions;

    std::size_t line_number = 0;
    std::size_t line_start = 0;
    while (line_start <= text.size()) {
      const std::size_t line_end = text.find('\n', line_start);
      const std::string line =
          text.substr(line_start, line_end == std::string::npos ? std::string::npos
                                                               : line_end - line_start);
      ++line_number;
      const bool sets_video_driver = line.find("SDL_VIDEODRIVER=") != std::string::npos;
      const bool wraps_in_xvfb = line.find("xvfb-run") != std::string::npos;
      const bool is_prose = line.find('`') != std::string::npos &&
                            line.find("microide_perf") == std::string::npos &&
                            line.find("./build/") == std::string::npos;
      if ((sets_video_driver || wraps_in_xvfb) && !is_prose) {
        Expect(false, path.string() + ":" + std::to_string(line_number) +
                          " pins a video driver for microide_perf; the harness owns its"
                          " lane (--video=dummy) and a windowed run is advisory only: " +
                          line);
      }
      if (line_end == std::string::npos) {
        break;
      }
      line_start = line_end + 1;
    }
  }
  Expect(invocations_seen >= 2, "perf recipes should reference microide_perf in both files");
}


void TestPerfBaselineLeavesCpuAndRssUngatedWhenAbsent() {
  TemporaryDirectory temp;
  const std::filesystem::path path = temp.path() / "pre_cpu_rss_baseline.json";
  WriteFile(path,
            R"({"scenario":"legacy","metrics":{"p50_wall_ms":10,"p95_wall_ms":10,)"
            R"("max_wall_ms":10,"p50_allocations":10,"p95_allocations":10,"max_allocations":10},)"
            R"("tolerances":{"p50_percent":30,"p95_percent":60,"max_percent":90}})");
  const std::optional<perf::BaselineRecord> loaded = perf::LoadBaseline(path);
  Expect(loaded.has_value(), "a baseline without cpu/rss metrics should still load");
  Expect(!loaded->has_cpu_metrics, "absent cpu metrics should be reported as not recorded");
  Expect(!loaded->has_rss_metrics, "absent rss metrics should be reported as not recorded");

  perf::Aggregate aggregate;
  aggregate.scenario_name = "legacy";
  aggregate.metrics = perf::MetricSet{
      .p50_wall_ms = 10.0, .p95_wall_ms = 10.0, .max_wall_ms = 10.0,
      .p50_allocations = 10.0, .p95_allocations = 10.0, .max_allocations = 10.0,
      .p50_cpu_ms = 999.0, .p95_cpu_ms = 999.0, .max_cpu_ms = 999.0,
      .p50_rss_growth_bytes = 9.0e8, .p95_rss_growth_bytes = 9.0e8,
      .max_rss_growth_bytes = 9.0e8, .mean_rss_growth_bytes = 9.0e8,
  };
  const perf::BaselineComparison comparison = perf::CompareToBaseline(*loaded, aggregate);
  Expect(comparison.passed,
         "an ungated baseline must not fail on cpu/rss it never recorded");
  for (const auto& metric : comparison.metrics) {
    Expect(metric.metric.find("cpu_ms") == std::string::npos &&
               metric.metric.find("rss_growth") == std::string::npos,
           "no cpu/rss row should be produced for a baseline that did not record them");
  }
}

void TestPerfBaselineGatesCpuAndRssWhenRecorded() {
  perf::BaselineRecord baseline = MakeBaseline();
  baseline.has_cpu_metrics = true;
  baseline.has_rss_metrics = true;
  baseline.metrics.p50_cpu_ms = 100.0;
  baseline.metrics.p95_cpu_ms = 100.0;
  baseline.metrics.max_cpu_ms = 100.0;
  baseline.metrics.mean_rss_growth_bytes = 1'000'000.0;
  baseline.metrics.p50_rss_growth_bytes = 1'000'000.0;
  baseline.metrics.p95_rss_growth_bytes = 1'000'000.0;
  baseline.metrics.max_rss_growth_bytes = 1'000'000.0;
  baseline.tolerances.cpu_p50_percent = 10.0;
  baseline.tolerances.rss_mean_percent = 25.0;

  perf::Aggregate cpu_regression;
  cpu_regression.scenario_name = baseline.scenario_name;
  cpu_regression.metrics = baseline.metrics;
  cpu_regression.metrics.p50_cpu_ms = 200.0;
  Expect(!perf::CompareToBaseline(baseline, cpu_regression).passed,
         "doubling CPU time at constant wall time must fail the baseline");

  perf::Aggregate rss_regression;
  rss_regression.scenario_name = baseline.scenario_name;
  rss_regression.metrics = baseline.metrics;
  rss_regression.metrics.mean_rss_growth_bytes = 4'000'000.0;
  Expect(!perf::CompareToBaseline(baseline, rss_regression).passed,
         "quadrupling resident growth must fail the baseline");

  perf::Aggregate within;
  within.scenario_name = baseline.scenario_name;
  within.metrics = baseline.metrics;
  within.metrics.p50_cpu_ms = 105.0;
  within.metrics.mean_rss_growth_bytes = 1'100'000.0;
  Expect(perf::CompareToBaseline(baseline, within).passed,
         "cpu/rss inside their envelopes should pass");
}

void TestPerfBaselineRssNoiseFloorAbsorbsSubPageJitter() {
  perf::BaselineRecord baseline = MakeBaseline();
  baseline.has_rss_metrics = true;
  baseline.metrics.mean_rss_growth_bytes = 0.0;
  baseline.metrics.p50_rss_growth_bytes = 0.0;
  baseline.metrics.p95_rss_growth_bytes = 0.0;
  baseline.metrics.max_rss_growth_bytes = 0.0;

  perf::Aggregate one_page;
  one_page.scenario_name = baseline.scenario_name;
  one_page.metrics = baseline.metrics;
  one_page.metrics.mean_rss_growth_bytes = 4096.0;
  Expect(perf::CompareToBaseline(baseline, one_page).passed,
         "a single page of growth over a zero baseline must not be a regression");

  perf::Aggregate real_growth;
  real_growth.scenario_name = baseline.scenario_name;
  real_growth.metrics = baseline.metrics;
  real_growth.metrics.mean_rss_growth_bytes = 64.0 * 1024.0 * 1024.0;
  Expect(!perf::CompareToBaseline(baseline, real_growth).passed,
         "64 MiB of growth over a zero baseline is a real regression, not noise");
}

void TestPerfBaselineDeclinesTheResidentGateOnAShortRun() {
  const auto run_of = [](std::size_t iteration_count, double mean_growth) {
    perf::Aggregate aggregate;
    aggregate.scenario_name = "test";
    aggregate.metrics.mean_rss_growth_bytes = mean_growth;
    for (std::size_t index = 0; index < iteration_count; ++index) {
      perf::Iteration iteration;
      iteration.index = index;
      aggregate.iterations.push_back(std::move(iteration));
    }
    return aggregate;
  };
  const auto resident = [](const perf::BaselineComparison& comparison) {
    for (const perf::MetricComparison& metric : comparison.metrics) {
      if (metric.metric == "mean_rss_growth_bytes") {
        return metric;
      }
    }
    Expect(false, "the comparison must report the resident metric at all");
    return perf::MetricComparison{};
  };

  perf::BaselineRecord baseline = MakeBaseline();
  baseline.has_rss_metrics = true;
  baseline.metrics.mean_rss_growth_bytes = 161'110.0;
  baseline.tolerances.rss_mean_percent = 25.0;
  baseline.iterations = 10;

  const perf::BaselineComparison short_run =
      perf::CompareToBaseline(baseline, run_of(6, 227'738.0));
  const perf::MetricComparison short_metric = resident(short_run);
  Expect(!short_metric.passed, "the raw arithmetic must still say the reading is out of envelope");
  Expect(!short_metric.enforced, "but a six-iteration run must not be gated against a ten's baseline");
  Expect(short_run.passed, "and the scenario must not go red on a comparison that was declined");
  Expect(short_metric.note.find("6 iterations") != std::string::npos &&
             short_metric.note.find("10") != std::string::npos,
         "the note must name both counts, which is the thing the failure line never said");
  Expect(short_metric.note.find("TD-2026-08-06-148") != std::string::npos,
         "and point at the analysis");

  perf::Aggregate short_with_alloc_regression = run_of(6, 227'738.0);
  short_with_alloc_regression.metrics.p50_allocations = 5'000.0;
  Expect(!perf::CompareToBaseline(baseline, short_with_alloc_regression).passed,
         "declining the resident gate must not disarm the allocation gates beside it");

  const perf::BaselineComparison at_count =
      perf::CompareToBaseline(baseline, run_of(10, 227'738.0));
  Expect(resident(at_count).enforced && !at_count.passed,
         "at the baseline's own iteration count the same reading must fail");
  Expect(resident(at_count).note.empty(),
         "and an ordinary comparison must say nothing at all");
  Expect(perf::CompareToBaseline(baseline, run_of(10, 92'000.0)).passed,
         "a reading inside the envelope at the recorded count still passes");

  const perf::BaselineComparison long_run =
      perf::CompareToBaseline(baseline, run_of(25, 227'738.0));
  Expect(resident(long_run).enforced && !long_run.passed,
         "a longer run stays gated: it cannot be fooled high by the settling passes");
  Expect(long_run.metrics.size() == at_count.metrics.size(),
         "and reports the same metric set");
  Expect(resident(long_run).note.find("loose") != std::string::npos,
         "but says the comparison is more permissive than the baseline describes");

  perf::BaselineRecord unversioned = baseline;
  unversioned.iterations = 0;
  const perf::BaselineComparison legacy =
      perf::CompareToBaseline(unversioned, run_of(6, 227'738.0));
  Expect(resident(legacy).enforced && !legacy.passed,
         "a baseline with no recorded iteration count must gate as it always did");

  perf::Aggregate summary_only;
  summary_only.scenario_name = "test";
  summary_only.metrics.mean_rss_growth_bytes = 227'738.0;
  const perf::BaselineComparison replayed = perf::CompareToBaseline(baseline, summary_only);
  Expect(resident(replayed).enforced && !replayed.passed,
         "an aggregate with no per-iteration detail must not read as a short run");
}

void TestPerfBaselineIterationCountRoundTrip() {
  TemporaryDirectory temp;
  perf::BaselineRecord baseline = MakeBaseline();
  baseline.has_rss_metrics = true;
  baseline.metrics.mean_rss_growth_bytes = 80'555.0;
  baseline.iterations = 10;

  const std::filesystem::path path = temp.path() / "iterations.json";
  Expect(perf::SaveBaseline(path, baseline), "a baseline with an iteration count should save");
  const std::optional<perf::BaselineRecord> loaded = perf::LoadBaseline(path);
  Expect(loaded.has_value(), "and load back");
  Expect(loaded->iterations == 10, "with the iteration count intact");

  perf::BaselineRecord unknown = baseline;
  unknown.iterations = 0;
  const std::filesystem::path absent = temp.path() / "no_iterations.json";
  Expect(perf::SaveBaseline(absent, unknown), "a baseline with no iteration count should save");
  const std::string text = ReadFile(absent);
  Expect(text.find("\"iterations\"") == std::string::npos,
         "an unknown iteration count must be omitted, not written as zero");
  const std::optional<perf::BaselineRecord> reloaded = perf::LoadBaseline(absent);
  Expect(reloaded.has_value() && reloaded->iterations == 0,
         "and come back as unknown rather than as a zero-iteration baseline");
}

void TestPerfBaselineCpuAndRssRoundTrip() {
  TemporaryDirectory temp;
  const std::filesystem::path path = temp.path() / "cpu_rss_roundtrip.json";
  perf::BaselineRecord baseline = MakeBaseline();
  baseline.metrics.p50_cpu_ms = 12.5;
  baseline.metrics.p95_cpu_ms = 18.5;
  baseline.metrics.max_cpu_ms = 24.5;
  baseline.metrics.mean_rss_growth_bytes = 98304.0;
  baseline.metrics.p50_rss_growth_bytes = 131072.0;
  baseline.metrics.p95_rss_growth_bytes = 262144.0;
  baseline.metrics.max_rss_growth_bytes = 524288.0;
  baseline.has_cpu_metrics = true;
  baseline.has_rss_metrics = true;
  Expect(perf::SaveBaseline(path, baseline), "saving a baseline with cpu/rss should succeed");

  const std::optional<perf::BaselineRecord> loaded = perf::LoadBaseline(path);
  Expect(loaded.has_value(), "a saved cpu/rss baseline should load");
  Expect(loaded->has_cpu_metrics && loaded->has_rss_metrics,
         "a round-tripped baseline should come back gated");
  Expect(loaded->metrics.p50_cpu_ms == 12.5, "cpu p50 should survive the round trip");
  Expect(loaded->metrics.max_rss_growth_bytes == 524288.0,
         "rss max should survive the round trip");
}

void TestPerfBaselineDeterministicMergeDoesNotMintAResidentGate() {
  TemporaryDirectory temp;
  const std::filesystem::path path = temp.path() / "no_resident_history.json";

  perf::BaselineRecord committed = MakeBaseline();
  committed.has_rss_metrics = false;
  Expect(perf::SaveBaseline(path, committed), "the pre-resident baseline should save");
  Expect(ReadFile(path).find("mean_rss_growth_bytes") == std::string::npos,
         "a record that never measured resident growth must not write the field");

  const std::optional<perf::BaselineRecord> reloaded = perf::LoadBaseline(path);
  Expect(reloaded.has_value(), "the pre-resident baseline should load");
  Expect(!reloaded->has_rss_metrics, "and come back ungated on resident growth");

  perf::BaselineRecord fresh = MakeBaseline();
  fresh.metrics.p50_allocations = 40.0;
  fresh.metrics.mean_rss_growth_bytes = 1500000.0;
  fresh.has_rss_metrics = true;
  const perf::BaselineRecord merged = perf::MergeDeterministicMetrics(*reloaded, fresh);
  Expect(merged.metrics.p50_allocations == 40.0, "the allocation half is re-recorded");
  Expect(!merged.has_rss_metrics,
         "the merge must not adopt a resident reading this mode is not entitled to take");

  Expect(perf::SaveBaseline(path, merged), "the merged baseline should save");
  Expect(ReadFile(path).find("mean_rss_growth_bytes") == std::string::npos,
         "and it must still not write a resident metric it does not have");

  const std::optional<perf::BaselineRecord> after = perf::LoadBaseline(path);
  Expect(after.has_value(), "the merged baseline should load");
  Expect(!after->has_rss_metrics,
         "an allocation-only rebaseline must leave the resident gate exactly as it found it: "
         "unarmed, until someone re-records it on the reference runner");

  perf::Aggregate aggregate;
  aggregate.scenario_name = after->scenario_name;
  aggregate.metrics = after->metrics;
  aggregate.metrics.mean_rss_growth_bytes = 1500000.0;
  const perf::BaselineComparison comparison = perf::CompareToBaseline(*after, aggregate);
  for (const perf::MetricComparison& metric : comparison.metrics) {
    Expect(metric.metric != "mean_rss_growth_bytes",
           "an unrecorded resident half must not appear as a gate");
  }
}

void TestPerfBaselineUngatedCpuIsOmittedNotZeroed() {
  TemporaryDirectory temp;
  const std::filesystem::path path = temp.path() / "cpu_ungated.json";
  perf::BaselineRecord baseline = MakeBaseline();
  baseline.metrics.p50_cpu_ms = 12.5;
  baseline.metrics.p95_cpu_ms = 18.5;
  baseline.metrics.max_cpu_ms = 24.5;
  baseline.metrics.p50_rss_growth_bytes = 131072.0;
  baseline.has_cpu_metrics = false;
  baseline.has_rss_metrics = true;
  Expect(perf::SaveBaseline(path, baseline), "saving an ungated-cpu baseline should succeed");

  const std::string text = ReadFile(path);
  Expect(text.find("p50_cpu_ms") == std::string::npos,
         "an ungated baseline must not write cpu metrics at all");
  Expect(text.find("p50_rss_growth_bytes") != std::string::npos,
         "opting out of cpu gating must not disturb rss gating");

  const std::optional<perf::BaselineRecord> loaded = perf::LoadBaseline(path);
  Expect(loaded.has_value(), "an ungated-cpu baseline should load");
  Expect(!loaded->has_cpu_metrics, "it must come back ungated on cpu, not gated against zero");
  Expect(loaded->has_rss_metrics, "and still gated on rss");

  perf::Aggregate aggregate;
  aggregate.scenario_name = loaded->scenario_name;
  aggregate.metrics.p50_wall_ms = loaded->metrics.p50_wall_ms;
  aggregate.metrics.p95_wall_ms = loaded->metrics.p95_wall_ms;
  aggregate.metrics.max_wall_ms = loaded->metrics.max_wall_ms;
  aggregate.metrics.p50_allocations = loaded->metrics.p50_allocations;
  aggregate.metrics.p95_allocations = loaded->metrics.p95_allocations;
  aggregate.metrics.max_allocations = loaded->metrics.max_allocations;
  aggregate.metrics.p50_rss_growth_bytes = loaded->metrics.p50_rss_growth_bytes;
  aggregate.metrics.p50_cpu_ms = 999.0;
  const perf::BaselineComparison comparison = perf::CompareToBaseline(*loaded, aggregate);
  Expect(comparison.passed, "a wildly higher cpu_ms must not fail an ungated scenario");
  for (const auto& metric : comparison.metrics) {
    Expect(metric.metric.find("cpu") == std::string::npos,
           "an ungated comparison must not report a cpu metric");
  }
}

void TestPerfCalibrationSpreadFlagsAMovingClock() {
  const auto make_run = [](std::initializer_list<std::uint64_t> calibrations) {
    perf::Aggregate aggregate;
    std::size_t index = 0;
    for (const std::uint64_t nanoseconds : calibrations) {
      perf::Iteration iteration;
      iteration.index = index++;
      iteration.perf_counters.emplace_back("editor.line_materializations", 4200);
      iteration.perf_counters.emplace_back("harness.cpu_calibration_ns", nanoseconds);
      aggregate.iterations.push_back(std::move(iteration));
    }
    return aggregate;
  };

  const perf::CalibrationSpread stepped = perf::MeasureCalibrationSpread(
      make_run({670700, 674300, 673600, 716300, 676800, 860800, 857200, 856600}));
  Expect(stepped.valid, "a run that recorded the probe should produce a spread");
  Expect(stepped.min_ns == 670700 && stepped.max_ns == 860800,
         "the spread should bound the probe, not any counter beside it");
  Expect(stepped.ratio > perf::kCalibrationSpreadNoteRatio,
         "a 1.28x clock step must clear the note threshold");
  const std::string note = perf::DescribeCalibrationSpread(stepped);
  Expect(note.find("670-860us") != std::string::npos,
         "the note should print the microsecond range it observed");
  Expect(note.find("TD-2026-08-05-137") != std::string::npos,
         "the note should point at the analysis that explains it");

  const perf::CalibrationSpread steady =
      perf::MeasureCalibrationSpread(make_run({670700, 673100, 671900, 674300}));
  Expect(steady.valid, "a steady run still records the probe");
  Expect(steady.ratio < perf::kCalibrationSpreadNoteRatio,
         "ordinary drift must stay under the note threshold");

  perf::Aggregate probeless;
  probeless.iterations.push_back(perf::Iteration{});
  const perf::CalibrationSpread absent = perf::MeasureCalibrationSpread(probeless);
  Expect(!absent.valid, "a run without the probe has no spread");
  Expect(perf::DescribeCalibrationSpread(absent).empty(),
         "and must describe nothing rather than a 1x range of zeros");
}

perf::Aggregate MakeCpuRun(std::vector<std::pair<double, std::uint64_t>> cpu_and_calibration) {
  perf::Aggregate aggregate;
  aggregate.scenario_name = "test";
  std::vector<double> cpu_ms;
  std::vector<double> calibration_ns;
  std::size_t index = 0;
  for (const auto& [cpu, calibration] : cpu_and_calibration) {
    perf::Iteration iteration;
    iteration.index = index++;
    iteration.metrics.cpu_ms = cpu;
    iteration.metrics.cpu_calibration_ns = calibration;
    cpu_ms.push_back(cpu);
    if (calibration != 0) {
      calibration_ns.push_back(static_cast<double>(calibration));
    }
    aggregate.iterations.push_back(std::move(iteration));
  }
  aggregate.metrics.p50_cpu_ms = perf::Percentile(cpu_ms, 0.50);
  aggregate.metrics.p95_cpu_ms = perf::Percentile(cpu_ms, 0.95);
  aggregate.metrics.max_cpu_ms = *std::max_element(cpu_ms.begin(), cpu_ms.end());
  aggregate.metrics.p50_cpu_calibration_ns = perf::Percentile(calibration_ns, 0.50);
  aggregate.metrics.p50_wall_ms = 100.0;
  aggregate.metrics.p95_wall_ms = 100.0;
  aggregate.metrics.max_wall_ms = 100.0;
  aggregate.metrics.p50_allocations = 100.0;
  aggregate.metrics.p95_allocations = 100.0;
  aggregate.metrics.max_allocations = 100.0;
  return aggregate;
}

perf::BaselineRecord MakeCpuBaseline(double cpu_ms, std::uint64_t calibration_ns) {
  perf::BaselineRecord baseline = MakeBaseline();
  baseline.has_cpu_metrics = true;
  baseline.metrics.p50_cpu_ms = cpu_ms;
  baseline.metrics.p95_cpu_ms = cpu_ms;
  baseline.metrics.max_cpu_ms = cpu_ms;
  baseline.tolerances.cpu_p50_percent = 10.0;
  baseline.tolerances.cpu_p95_percent = 10.0;
  baseline.tolerances.cpu_max_percent = 10.0;
  baseline.has_calibration = calibration_ns != 0;
  baseline.metrics.p50_cpu_calibration_ns = static_cast<double>(calibration_ns);
  return baseline;
}

void TestPerfBaselineNormalisesCpuAgainstTheBaselineClock() {
  const perf::BaselineRecord baseline = MakeCpuBaseline(10.0, 670000);

  const perf::Aggregate slower_machine = MakeCpuRun({{14.0, 938000},
                                                     {14.0, 938000},
                                                     {14.0, 938000},
                                                     {14.0, 938000}});
  const perf::BaselineComparison normalised = perf::CompareToBaseline(baseline, slower_machine);
  Expect(normalised.passed,
         "a +40% cpu rise that the calibration probe says was the machine must not fail");
  Expect(normalised.clock.applied, "the comparison should report that it normalised");
  Expect(std::abs(normalised.clock.factor - 1.4) < 0.01,
         "the reported factor should be the measured clock ratio");
  for (const auto& metric : normalised.metrics) {
    if (metric.metric == "p50_cpu_ms") {
      Expect(std::abs(metric.raw_actual - 14.0) < 1e-9,
             "the raw measurement should still be reported alongside");
      Expect(std::abs(metric.actual - 10.0) < 1e-6,
             "the normalised measurement should come back to the baseline's machine state");
    }
  }

  const perf::Aggregate real_regression = MakeCpuRun({{14.0, 670000},
                                                      {14.0, 670000},
                                                      {14.0, 670000},
                                                      {14.0, 670000}});
  Expect(!perf::CompareToBaseline(baseline, real_regression).passed,
         "a +40% cpu rise at an unchanged clock must still fail");

  perf::BaselineRecord no_clock = baseline;
  no_clock.has_calibration = false;
  no_clock.metrics.p50_cpu_calibration_ns = 0.0;
  const perf::BaselineComparison unnormalised =
      perf::CompareToBaseline(no_clock, slower_machine);
  Expect(!unnormalised.passed,
         "a baseline with no recorded clock must compare raw rather than invent a factor");
  Expect(!unnormalised.clock.applied, "and must report that it did not normalise");
}

void TestPerfBaselineNormalisesWallByItsWorkFraction() {
  const auto make_run = [](double wall_ms, double cpu_ms, std::uint64_t calibration_ns) {
    perf::Aggregate aggregate;
    aggregate.scenario_name = "test";
    std::vector<double> walls;
    for (int i = 0; i < 4; ++i) {
      perf::Iteration iteration;
      iteration.index = static_cast<std::size_t>(i);
      iteration.metrics.wall_ms = wall_ms;
      iteration.metrics.cpu_ms = cpu_ms;
      iteration.metrics.cpu_calibration_ns = calibration_ns;
      walls.push_back(wall_ms);
      aggregate.iterations.push_back(std::move(iteration));
    }
    aggregate.metrics.p50_wall_ms = perf::Percentile(walls, 0.50);
    aggregate.metrics.p95_wall_ms = perf::Percentile(walls, 0.95);
    aggregate.metrics.max_wall_ms = wall_ms;
    aggregate.metrics.p50_cpu_calibration_ns = static_cast<double>(calibration_ns);
    aggregate.metrics.p50_allocations = 100.0;
    aggregate.metrics.p95_allocations = 100.0;
    aggregate.metrics.max_allocations = 100.0;
    return aggregate;
  };

  perf::BaselineRecord baseline = MakeBaseline();
  baseline.metrics.p50_wall_ms = 10.0;
  baseline.metrics.p95_wall_ms = 10.0;
  baseline.metrics.max_wall_ms = 10.0;
  baseline.tolerances.p50_percent = 10.0;
  baseline.tolerances.p95_percent = 10.0;
  baseline.tolerances.max_percent = 10.0;
  baseline.has_calibration = true;
  baseline.metrics.p50_cpu_calibration_ns = 670000.0;

  const perf::BaselineComparison work =
      perf::CompareToBaseline(baseline, make_run(14.0, 14.0, 938000));
  Expect(work.passed,
         "a wall rise the calibration probe attributes to the machine must not fail a "
         "cpu-bound scenario");

  Expect(!perf::CompareToBaseline(baseline, make_run(14.0, 14.0, 670000)).passed,
         "a +40% wall rise at an unchanged clock must still fail");

  Expect(!perf::CompareToBaseline(baseline, make_run(14.0, 0.007, 938000)).passed,
         "a sleep-dominated scenario must not have its wall scaled by the machine clock");
}

void TestPerfBaselineAdvisoryTimingHalfGatesAllocationsOnly() {
  perf::BaselineRecord baseline = MakeBaseline();
  baseline.timing_is_advisory = true;
  baseline.has_cpu_metrics = true;
  baseline.metrics.p50_cpu_ms = 10.0;
  baseline.metrics.p95_cpu_ms = 10.0;
  baseline.metrics.max_cpu_ms = 10.0;

  perf::Aggregate wall_blowout;
  wall_blowout.scenario_name = "test";
  wall_blowout.metrics = baseline.metrics;
  wall_blowout.metrics.p50_wall_ms = baseline.metrics.p50_wall_ms * 50.0;
  wall_blowout.metrics.p95_wall_ms = baseline.metrics.p95_wall_ms * 50.0;
  wall_blowout.metrics.max_wall_ms = baseline.metrics.max_wall_ms * 50.0;
  wall_blowout.metrics.p50_cpu_ms = 500.0;
  const perf::BaselineComparison timing = perf::CompareToBaseline(baseline, wall_blowout);
  Expect(timing.passed,
         "an advisory-timing baseline must not fail on wall or cpu -- those numbers describe "
         "the machine that recorded them");
  bool wall_noted = false;
  for (const auto& metric : timing.metrics) {
    if (metric.metric == "p50_wall_ms") {
      Expect(!metric.enforced, "the wall metric must be reported as unenforced");
      Expect(metric.note.find("advisory runner") != std::string::npos,
             "and must say WHY it is unenforced, on the verdict line");
      wall_noted = true;
    }
  }
  Expect(wall_noted, "the comparison must still carry the wall metric, measured and printed");

  perf::Aggregate alloc_regression;
  alloc_regression.scenario_name = "test";
  alloc_regression.metrics = baseline.metrics;
  alloc_regression.metrics.p50_allocations = baseline.metrics.p50_allocations * 4.0;
  Expect(!perf::CompareToBaseline(baseline, alloc_regression).passed,
         "an advisory-timing baseline must still fail on an allocation regression");

  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "microide-advisory-timing-baseline.json";
  Expect(perf::SaveBaseline(path, baseline), "the advisory baseline should save");
  const std::optional<perf::BaselineRecord> loaded = perf::LoadBaseline(path);
  Expect(loaded.has_value() && loaded->timing_is_advisory,
         "timing_is_advisory must survive a save/load round trip");
  std::filesystem::remove(path);

  perf::BaselineRecord reference = baseline;
  reference.timing_is_advisory = false;
  Expect(!perf::CompareToBaseline(reference, wall_blowout).passed,
         "a reference-lane baseline must still enforce its timing half");
}

void TestPerfBaselineDeclinesComparisonAcrossBuildConfigurations() {
  perf::BaselineRecord baseline = MakeBaseline();
  baseline.build_config = "RelWithDebInfo+lto";
  baseline.has_net_heap_metrics = true;
  baseline.metrics.p50_net_heap_bytes = 8294.0;
  baseline.tolerances.net_heap_percent = 10.0;

  const auto run_with = [&](const std::string& config, double net_heap) {
    perf::Aggregate aggregate;
    aggregate.scenario_name = "test";
    aggregate.metrics = baseline.metrics;
    aggregate.metrics.p50_net_heap_bytes = net_heap;
    aggregate.build_config = config;
    return perf::CompareToBaseline(baseline, aggregate);
  };

  Expect(!run_with("RelWithDebInfo+lto", 75872.0).passed,
         "a retention regression measured in the baseline's own build must still fail");

  const perf::BaselineComparison crossed = run_with("Release", 75872.0);
  Expect(crossed.passed,
         "a retention reading from a different build configuration must not be gated");
  bool noted = false;
  for (const auto& metric : crossed.metrics) {
    if (metric.metric == "p50_net_heap_bytes") {
      Expect(!metric.enforced, "the net-heap metric must be reported as unenforced");
      Expect(metric.note.find("Release") != std::string::npos &&
                 metric.note.find("RelWithDebInfo+lto") != std::string::npos,
             "and must name BOTH configurations, so the reader can see which is which");
      noted = true;
    }
  }
  Expect(noted, "the crossed comparison must still carry the metric, measured and printed");

  perf::Aggregate alloc_regression;
  alloc_regression.scenario_name = "test";
  alloc_regression.metrics = baseline.metrics;
  alloc_regression.metrics.p50_allocations = baseline.metrics.p50_allocations * 4.0;
  alloc_regression.build_config = "Release";
  Expect(!perf::CompareToBaseline(baseline, alloc_regression).passed,
         "an allocation regression must fail even across build configurations");

  perf::BaselineRecord unlabelled = baseline;
  unlabelled.build_config.clear();
  perf::Aggregate labelled;
  labelled.scenario_name = "test";
  labelled.metrics = baseline.metrics;
  labelled.metrics.p50_net_heap_bytes = 75872.0;
  labelled.build_config = "Release";
  Expect(!perf::CompareToBaseline(unlabelled, labelled).passed,
         "an unlabelled baseline must compare raw rather than assume a mismatch");
}

void TestPerfBaselineWallEnvelopeComesFromMeasuredSpread() {
  using perf::EffectiveWallTolerance;

  Expect(EffectiveWallTolerance(100.0, 0.0) == 100.0,
         "a baseline with no recorded spread must keep the declared envelope");

  Expect(EffectiveWallTolerance(100.0, 2.0) == 25.0,
         "a steady scenario tightens to the floor rather than to 3x a tiny spread");

  Expect(EffectiveWallTolerance(100.0, 20.0) == 60.0,
         "a jittery scenario derives its envelope from its own spread");

  Expect(EffectiveWallTolerance(100.0, 200.0) == 100.0,
         "the derived envelope is capped at the declared tolerance, never above it");
  Expect(EffectiveWallTolerance(20.0, 2.0) == 20.0,
         "a scenario declaring TIGHTER than the floor keeps its own value");

  perf::BaselineRecord baseline = MakeBaseline();
  baseline.metrics.p50_wall_ms = 10.0;
  baseline.metrics.p95_wall_ms = 10.0;
  baseline.metrics.max_wall_ms = 10.0;
  baseline.tolerances.p50_percent = 100.0;
  baseline.tolerances.p95_percent = 150.0;
  baseline.tolerances.max_percent = 200.0;
  baseline.wall_spread_percent = 2.0;

  perf::Aggregate run;
  run.scenario_name = "test";
  run.metrics = baseline.metrics;
  run.metrics.p50_wall_ms = 14.0;
  run.metrics.p95_wall_ms = 14.0;
  run.metrics.max_wall_ms = 14.0;
  Expect(!perf::CompareToBaseline(baseline, run).passed,
         "a +40% wall rise must fail a scenario whose own measured spread is 2%");

  perf::BaselineRecord unrecorded = baseline;
  unrecorded.wall_spread_percent = 0.0;
  Expect(perf::CompareToBaseline(unrecorded, run).passed,
         "and must still pass against the 100% default, which is what the entry says "
         "cannot catch a regression under 2x");
}

void TestPerfBaselineNormalisesEachIterationAgainstItsOwnClock() {
  const perf::BaselineRecord baseline = MakeCpuBaseline(14.0, 671000);
  const perf::Aggregate stepped = MakeCpuRun({{14.0, 671000},
                                              {14.1, 674000},
                                              {13.9, 673000},
                                              {14.2, 676000},
                                              {17.9, 857000},
                                              {17.8, 860000},
                                              {17.9, 856000},
                                              {18.0, 858000}});
  const perf::BaselineComparison comparison = perf::CompareToBaseline(baseline, stepped);
  Expect(comparison.passed,
         "a mid-run clock step with unchanged code must not fail the cpu gate");
  for (const auto& metric : comparison.metrics) {
    if (metric.metric == "max_cpu_ms") {
      Expect(metric.actual < 15.0,
             "even the max, which lands in the slow half, should normalise back near 14 ms");
    }
  }

  const perf::Aggregate stepped_with_regression = MakeCpuRun({{14.0, 671000},
                                                              {14.1, 674000},
                                                              {13.9, 673000},
                                                              {14.2, 676000},
                                                              {26.0, 857000},
                                                              {26.0, 860000},
                                                              {26.0, 856000},
                                                              {26.0, 858000}});
  Expect(!perf::CompareToBaseline(baseline, stepped_with_regression).passed,
         "a real regression inside the slow half must survive normalisation");
}

void TestPerfBaselineClampsAnAbsurdClockFactor() {
  const perf::BaselineRecord baseline = MakeCpuBaseline(10.0, 670000);
  const perf::Aggregate absurd = MakeCpuRun({{100.0, 67000000}, {100.0, 67000000}});
  const perf::BaselineComparison comparison = perf::CompareToBaseline(baseline, absurd);
  Expect(comparison.clock.clamped, "a 100x clock reading should report as clamped");
  Expect(!comparison.passed,
         "and must not pass: clamping means the gate stays enforced at 3x, not disabled");
}

void TestPerfBaselineClockNormalisationTouchesCpuOnly() {
  perf::BaselineRecord baseline = MakeCpuBaseline(10.0, 670000);
  baseline.has_rss_metrics = true;
  baseline.metrics.mean_rss_growth_bytes = 1'000'000.0;

  perf::Aggregate run = MakeCpuRun({{14.0, 938000}, {14.0, 938000}});
  run.metrics.p50_allocations = 140.0;
  run.metrics.mean_rss_growth_bytes = 1'400'000.0;
  const perf::BaselineComparison comparison = perf::CompareToBaseline(baseline, run);
  Expect(!comparison.passed, "a clock move must not excuse an allocation or rss rise");
  for (const auto& metric : comparison.metrics) {
    if (metric.metric == "p50_allocations" || metric.metric == "mean_rss_growth_bytes") {
      Expect(metric.actual == metric.raw_actual,
             "non-duration metrics must be compared exactly as measured: " + metric.metric);
      Expect(!metric.passed, metric.metric + " should have failed at +40%");
    }
  }
}

void TestPerfBaselineRssGateSeesPeriodicRetention() {
  perf::BaselineRecord baseline = MakeBaseline();
  baseline.has_rss_metrics = true;
  baseline.tolerances.rss_mean_percent = 25.0;
  baseline.metrics.mean_rss_growth_bytes = 500'000.0;
  baseline.metrics.p50_rss_growth_bytes = 0.0;

  perf::Aggregate doubled;
  doubled.scenario_name = baseline.scenario_name;
  doubled.metrics = baseline.metrics;
  doubled.metrics.mean_rss_growth_bytes = 1'000'000.0;
  doubled.metrics.p50_rss_growth_bytes = 0.0;
  Expect(!perf::CompareToBaseline(baseline, doubled).passed,
         "doubling a periodic retention rate must fail even though its p50 stays zero");

  bool saw_mean = false;
  for (const auto& metric : perf::CompareToBaseline(baseline, doubled).metrics) {
    saw_mean = saw_mean || metric.metric == "mean_rss_growth_bytes";
    Expect(metric.metric != "p50_rss_growth_bytes",
           "the percentile must not gate: it is recorded for diagnosis only");
  }
  Expect(saw_mean, "the trimmed mean should be the resident row in the comparison");
}

void TestPerfBaselineLeavesRssUngatedWhenOnlyPercentilesRecorded() {
  TemporaryDirectory temp;
  const std::filesystem::path path = temp.path() / "percentile_only_rss.json";
  WriteFile(path,
            R"({"scenario":"legacy","metrics":{"p50_wall_ms":10,"p95_wall_ms":10,)"
            R"("max_wall_ms":10,"p50_allocations":10,"p95_allocations":10,"max_allocations":10,)"
            R"("p50_rss_growth_bytes":1000,"p95_rss_growth_bytes":2000,)"
            R"("max_rss_growth_bytes":3000},"tolerances":{}})");
  const std::optional<perf::BaselineRecord> loaded = perf::LoadBaseline(path);
  Expect(loaded.has_value(), "a percentile-only baseline should load");
  Expect(!loaded->has_rss_metrics,
         "and must come back ungated on resident growth rather than gated against zero");

  perf::Aggregate aggregate;
  aggregate.scenario_name = "legacy";
  aggregate.metrics = loaded->metrics;
  aggregate.metrics.mean_rss_growth_bytes = 9.0e8;
  Expect(perf::CompareToBaseline(*loaded, aggregate).passed,
         "a huge measured mean must not fail a baseline that never recorded one");
}

void TestPerfBaselineCalibrationRoundTrip() {
  TemporaryDirectory temp;
  const std::filesystem::path path = temp.path() / "calibration_roundtrip.json";
  perf::BaselineRecord baseline = MakeCpuBaseline(10.0, 673456);
  Expect(perf::SaveBaseline(path, baseline), "a baseline with a recorded clock should save");
  const std::optional<perf::BaselineRecord> loaded = perf::LoadBaseline(path);
  Expect(loaded.has_value() && loaded->has_calibration,
         "it should come back carrying its clock");
  Expect(loaded->metrics.p50_cpu_calibration_ns == 673456.0,
         "the recorded clock should survive the round trip");

  const std::filesystem::path absent = temp.path() / "calibration_absent.json";
  perf::BaselineRecord probeless = MakeCpuBaseline(10.0, 0);
  Expect(perf::SaveBaseline(absent, probeless), "a baseline with no probe should still save");
  Expect(ReadFile(absent).find("p50_cpu_calibration_ns") == std::string::npos,
         "a missing clock must be omitted, not written as zero");
  const std::optional<perf::BaselineRecord> reloaded = perf::LoadBaseline(absent);
  Expect(reloaded.has_value() && !reloaded->has_calibration,
         "and must come back unnormalised rather than gated against a zero clock");
}

void TestPerfPhaseGateCatchesWhatTheTotalCannot() {
  perf::BaselineRecord baseline = MakeBaseline();
  baseline.metrics.p50_allocations = 20192.0;
  baseline.tolerances.alloc_p50_percent = 10.0;
  baseline.tolerances.phase_alloc_p50_percent = 10.0;
  baseline.phases.push_back(perf::PhaseMetricSet{
      .name = "search_first_result.search_to_first_result",
      .p50_allocations = 145.0,
      .max_allocations = 145.0,
      .p50_wall_ms = 2.5,
      .iterations = 10,
  });

  perf::Aggregate aggregate;
  aggregate.scenario_name = "test";
  aggregate.metrics = perf::MetricSet{
      .p50_wall_ms = 100.0,
      .p95_wall_ms = 100.0,
      .max_wall_ms = 100.0,
      .p50_allocations = 20192.0 + 145.0,
      .p95_allocations = 100.0,
      .max_allocations = 100.0,
  };
  aggregate.phases.push_back(perf::PhaseMetricSet{
      .name = "search_first_result.search_to_first_result",
      .p50_allocations = 290.0,
      .max_allocations = 290.0,
      .p50_wall_ms = 5.0,
      .iterations = 10,
  });

  const perf::BaselineComparison comparison = perf::CompareToBaseline(baseline, aggregate);
  const auto find = [&](std::string_view name) -> const perf::MetricComparison* {
    for (const perf::MetricComparison& metric : comparison.metrics) {
      if (metric.metric == name) {
        return &metric;
      }
    }
    return nullptr;
  };

  const perf::MetricComparison* total = find("p50_allocations");
  Expect(total != nullptr && total->passed,
         "the defect under test is that the SCENARIO TOTAL passes a doubled search — "
         "if this ever fails, the premise of TD-2026-08-06-153 has changed");

  const perf::MetricComparison* phase =
      find("phase[search_first_result.search_to_first_result].p50_allocations");
  Expect(phase != nullptr, "the phase the baseline records must be gated");
  Expect(!phase->passed && phase->enforced,
         "a doubled measured phase must fail an enforced gate, not merely be reported");
  Expect(!comparison.passed, "a failed phase gate must turn the scenario red");
}

void TestPerfPhaseGateFailsWhenAPhaseStopsBeingMeasured() {
  perf::BaselineRecord baseline = MakeBaseline();
  baseline.phases.push_back(perf::PhaseMetricSet{
      .name = "scenario.the_phase", .p50_allocations = 145.0, .iterations = 10});

  perf::Aggregate aggregate;
  aggregate.scenario_name = "test";
  aggregate.metrics = baseline.metrics;
  aggregate.phases.push_back(perf::PhaseMetricSet{
      .name = "scenario.the_phase_renamed", .p50_allocations = 145.0, .iterations = 10});

  const perf::BaselineComparison comparison = perf::CompareToBaseline(baseline, aggregate);
  Expect(!comparison.passed, "a baseline phase that stopped being measured must fail the run");
  bool found_missing = false;
  bool found_ungated_note = false;
  for (const perf::MetricComparison& metric : comparison.metrics) {
    if (metric.metric == "phase[scenario.the_phase].p50_allocations") {
      found_missing = true;
      Expect(!metric.passed && metric.enforced && !metric.note.empty(),
             "the missing phase must fail with an explanation, not pass on a zero measurement");
    }
    if (metric.metric == "phases_not_in_baseline") {
      found_ungated_note = true;
      Expect(!metric.enforced,
             "a phase with no baseline must be reported, not enforced against nothing");
      Expect(metric.note.find("scenario.the_phase_renamed") != std::string::npos,
             "the note must name the ungated phase so it can be acted on");
    }
  }
  Expect(found_missing, "the comparison must carry a metric for the missing phase");
  Expect(found_ungated_note, "a measured phase with no baseline must be reported as ungated");
}

void TestPerfPhaseGateIsSilentUntilTheBaselineRecordsPhases() {
  const perf::BaselineRecord baseline = MakeBaseline();
  perf::Aggregate aggregate;
  aggregate.scenario_name = "test";
  aggregate.metrics = baseline.metrics;
  aggregate.phases.push_back(perf::PhaseMetricSet{
      .name = "scenario.phase", .p50_allocations = 900000.0, .iterations = 10});

  const perf::BaselineComparison comparison = perf::CompareToBaseline(baseline, aggregate);
  Expect(comparison.passed,
         "a pre-phase-gating baseline must keep passing on its totals alone");
  bool noted = false;
  for (const perf::MetricComparison& metric : comparison.metrics) {
    Expect(metric.metric.rfind("phase[", 0) != 0,
           "no phase may be gated when the baseline records none");
    if (metric.metric == "phases_not_in_baseline") {
      noted = true;
    }
  }
  Expect(noted, "the ungated phase must still be announced");
}

void TestPerfPhaseBaselineRoundTrip() {
  TemporaryDirectory temp;
  const std::filesystem::path path = temp.path() / "phase_baseline.json";
  perf::BaselineRecord baseline = MakeBaseline();
  baseline.tolerances.phase_alloc_p50_percent = 15.0;
  baseline.phases.push_back(perf::PhaseMetricSet{.name = "a.first",
                                                 .p50_allocations = 145.0,
                                                 .max_allocations = 151.0,
                                                 .p50_wall_ms = 2.5,
                                                 .iterations = 10});
  baseline.phases.push_back(perf::PhaseMetricSet{.name = "a.second",
                                                 .p50_allocations = 0.0,
                                                 .max_allocations = 0.0,
                                                 .p50_wall_ms = 0.125,
                                                 .iterations = 10});
  Expect(perf::SaveBaseline(path, baseline), "baseline should save");
  const std::optional<perf::BaselineRecord> loaded = perf::LoadBaseline(path);
  Expect(loaded.has_value(), "baseline should reload");
  Expect(loaded->phases.size() == 2, "both phases must round-trip");
  Expect(loaded->phases[0].name == "a.first" && loaded->phases[1].name == "a.second",
         "phase order must round-trip");
  Expect(loaded->phases[0].p50_allocations == 145.0 && loaded->phases[0].max_allocations == 151.0,
         "phase allocation numbers must round-trip");
  Expect(loaded->phases[0].iterations == 10, "the phase's iteration count must round-trip");
  Expect(loaded->tolerances.phase_alloc_p50_percent == 15.0,
         "a hand-set phase tolerance must survive a save/load cycle");
  Expect(loaded->phases[1].p50_allocations == 0.0, "a zero-allocation phase must round-trip");
}

void TestPerfPhaseAggregationSumsRepeatsWithinAnIteration() {
  std::vector<perf::Iteration> iterations;
  for (std::size_t i = 0; i < 4; ++i) {
    perf::Iteration iteration;
    iteration.index = i;
    iteration.phase_metrics.push_back(
        perf::Iteration::PhaseMetrics{.name = "s.frame", .wall_ms = 1.0, .allocations = 10});
    iteration.phase_metrics.push_back(
        perf::Iteration::PhaseMetrics{.name = "s.frame", .wall_ms = 1.0, .allocations = 30});
    iteration.phase_metrics.push_back(
        perf::Iteration::PhaseMetrics{.name = "s.settle", .wall_ms = 0.5, .allocations = 7});
    iterations.push_back(std::move(iteration));
  }
  const std::vector<perf::PhaseMetricSet> phases = perf::AggregatePhaseMetrics(iterations);
  Expect(phases.size() == 2, "one entry per distinct phase name");
  Expect(phases[0].name == "s.frame" && phases[1].name == "s.settle",
         "phases must keep first-appearance order");
  Expect(phases[0].p50_allocations == 40.0,
         "repeated Measure() calls in one iteration must sum, not overwrite");
  Expect(phases[0].p50_wall_ms == 2.0, "repeated phase wall time must sum too");
  Expect(phases[0].iterations == 4 && phases[1].iterations == 4,
         "every iteration recorded both phases");

  perf::Iteration sparse;
  sparse.index = 4;
  sparse.phase_metrics.push_back(
      perf::Iteration::PhaseMetrics{.name = "s.rare", .wall_ms = 9.0, .allocations = 99});
  iterations.push_back(std::move(sparse));
  const std::vector<perf::PhaseMetricSet> with_sparse = perf::AggregatePhaseMetrics(iterations);
  Expect(with_sparse.size() == 3, "the late phase must appear");
  Expect(with_sparse[2].name == "s.rare" && with_sparse[2].iterations == 1,
         "a phase seen once must report one iteration, not five");
  Expect(with_sparse[2].p50_allocations == 99.0,
         "a sparse phase percentiles over the iterations that recorded it");
}

void TestPerfDeterministicMergeKeepsTheTimingHalf() {
  perf::BaselineRecord committed = MakeBaseline();
  committed.metrics.p50_wall_ms = 12.0;
  committed.metrics.p95_wall_ms = 15.0;
  committed.metrics.max_wall_ms = 20.0;
  committed.metrics.p50_cpu_ms = 11.0;
  committed.metrics.p95_cpu_ms = 14.0;
  committed.metrics.max_cpu_ms = 19.0;
  committed.metrics.mean_rss_growth_bytes = 4096.0;
  committed.metrics.p50_cpu_calibration_ns = 500000.0;
  committed.metrics.p50_allocations = 76455.0;
  committed.metrics.p95_allocations = 79069.0;
  committed.metrics.max_allocations = 81089.0;
  committed.metrics.p50_net_heap_bytes = 10781.0;
  committed.has_cpu_metrics = true;
  committed.has_rss_metrics = true;
  committed.has_net_heap_metrics = true;
  committed.has_calibration = true;
  committed.iterations = 10;
  committed.phases.push_back(
      perf::PhaseMetricSet{.name = "insert", .p50_allocations = 67366.0, .max_allocations = 67368.0,
                           .p50_wall_ms = 6.28, .iterations = 10});

  perf::BaselineRecord fresh = MakeBaseline();
  fresh.metrics.p50_wall_ms = 90.0;
  fresh.metrics.p95_wall_ms = 140.0;
  fresh.metrics.max_wall_ms = 200.0;
  fresh.metrics.p50_cpu_ms = 88.0;
  fresh.metrics.mean_rss_growth_bytes = 900000.0;
  fresh.metrics.p50_cpu_calibration_ns = 1400000.0;
  fresh.metrics.p50_allocations = 933.0;
  fresh.metrics.p95_allocations = 940.0;
  fresh.metrics.max_allocations = 951.0;
  fresh.metrics.p50_net_heap_bytes = 2048.0;
  fresh.has_net_heap_metrics = true;
  fresh.iterations = 10;
  fresh.phases.push_back(
      perf::PhaseMetricSet{.name = "insert", .p50_allocations = 810.0, .max_allocations = 815.0,
                           .p50_wall_ms = 41.7, .iterations = 10});
  fresh.phases.push_back(
      perf::PhaseMetricSet{.name = "settle", .p50_allocations = 12.0, .max_allocations = 12.0,
                           .p50_wall_ms = 3.0, .iterations = 10});

  const perf::BaselineRecord merged = perf::MergeDeterministicMetrics(committed, fresh);

  Expect(merged.metrics.p50_allocations == 933.0, "the allocation p50 is taken from the run");
  Expect(merged.metrics.p95_allocations == 940.0, "the allocation p95 is taken from the run");
  Expect(merged.metrics.max_allocations == 951.0, "the allocation max is taken from the run");
  Expect(merged.metrics.p50_net_heap_bytes == 2048.0, "net heap retention is deterministic too");

  Expect(merged.metrics.p50_wall_ms == 12.0, "wall p50 stays as committed");
  Expect(merged.metrics.p95_wall_ms == 15.0, "wall p95 stays as committed");
  Expect(merged.metrics.max_wall_ms == 20.0, "wall max stays as committed");
  Expect(merged.metrics.p50_cpu_ms == 11.0, "cpu stays as committed");
  Expect(merged.metrics.p95_cpu_ms == 14.0, "cpu p95 stays as committed");
  Expect(merged.metrics.max_cpu_ms == 19.0, "cpu max stays as committed");
  Expect(merged.metrics.mean_rss_growth_bytes == 4096.0, "the resident mean stays as committed");
  Expect(merged.metrics.p50_cpu_calibration_ns == 500000.0,
         "the clock the baseline was captured at must NOT be replaced by the loaded box's — "
         "a merged record whose calibration says 'measured slow' would normalise every later "
         "cpu gate loose, which is the drift this mode exists to avoid");
  Expect(merged.iterations == 10, "the recorded iteration count belongs to the timing half");
  Expect(merged.has_cpu_metrics && merged.has_rss_metrics && merged.has_calibration,
         "presence flags for the preserved metrics are preserved with them");

  Expect(merged.phases.size() == 2, "phases follow the run, so a new phase is gated");
  Expect(merged.phases[0].name == "insert" && merged.phases[0].p50_allocations == 810.0,
         "a phase's allocation count is taken from the run");
  Expect(merged.phases[0].p50_wall_ms == 6.28,
         "a phase that existed keeps its committed wall reading");
  Expect(merged.phases[1].name == "settle" && merged.phases[1].p50_wall_ms == 3.0,
         "a phase with no committed counterpart is taken whole — there is nothing to preserve");
}

void TestPerfDeterministicMergeDropsPhasesNoLongerMeasured() {
  perf::BaselineRecord committed = MakeBaseline();
  committed.phases.push_back(perf::PhaseMetricSet{.name = "gone", .p50_allocations = 5.0});
  committed.phases.push_back(perf::PhaseMetricSet{.name = "kept", .p50_allocations = 5.0});

  perf::BaselineRecord fresh = MakeBaseline();
  fresh.phases.push_back(perf::PhaseMetricSet{.name = "kept", .p50_allocations = 3.0});

  const perf::BaselineRecord merged = perf::MergeDeterministicMetrics(committed, fresh);
  Expect(merged.phases.size() == 1 && merged.phases[0].name == "kept",
         "a phase the scenario no longer measures is dropped, exactly as a full rebaseline does");
}

void TestPerfBaselineRefusesToGateAcrossAMeasurementRevision() {
  perf::BaselineRecord baseline = MakeBaseline();
  baseline.measurement_revision = 1;

  perf::Aggregate aggregate;
  aggregate.scenario_name = "test";
  aggregate.measurement_revision = 2;
  aggregate.metrics = perf::MetricSet{
      .p50_wall_ms = 500.0,
      .p95_wall_ms = 500.0,
      .max_wall_ms = 500.0,
      .p50_allocations = 500.0,
      .p95_allocations = 500.0,
      .max_allocations = 500.0,
  };

  const perf::BaselineComparison comparison = perf::CompareToBaseline(baseline, aggregate);
  Expect(!comparison.passed, "a revision mismatch must go red, not quietly green");

  bool saw_mismatch = false;
  for (const perf::MetricComparison& metric : comparison.metrics) {
    if (metric.metric == "measurement_revision") {
      saw_mismatch = true;
      Expect(metric.enforced && !metric.passed,
             "the mismatch itself is the enforced failure");
      Expect(metric.note.find("NOT COMPARABLE") != std::string::npos,
             "and says so in words, not only as a number");
      continue;
    }
    Expect(!metric.enforced,
           std::string("every other metric must be reported and NOT enforced: ") + metric.metric);
  }
  Expect(saw_mismatch, "the comparison must carry a measurement_revision entry");

  perf::BaselineRecord matched = baseline;
  matched.measurement_revision = 2;
  const perf::BaselineComparison same = perf::CompareToBaseline(matched, aggregate);
  Expect(!same.passed, "at a matching revision the 5x regression is a real failure again");
  for (const perf::MetricComparison& metric : same.metrics) {
    Expect(metric.metric != "measurement_revision",
           "and no mismatch entry is added when the revisions agree");
  }
}

void TestPerfBaselineMeasurementRevisionRoundTrip() {
  TemporaryDirectory temp;
  perf::BaselineRecord baseline = MakeBaseline();
  baseline.measurement_revision = 4;

  const std::filesystem::path path = temp.path() / "revision.json";
  Expect(perf::SaveBaseline(path, baseline), "a baseline with a revision should save");
  const std::optional<perf::BaselineRecord> loaded = perf::LoadBaseline(path);
  Expect(loaded.has_value() && loaded->measurement_revision == 4, "and load back intact");

  perf::BaselineRecord defaulted = MakeBaseline();
  const std::filesystem::path default_path = temp.path() / "default_revision.json";
  Expect(perf::SaveBaseline(default_path, defaulted), "the default revision should save");
  Expect(ReadFile(default_path).find("\"measurement_revision\"") != std::string::npos,
         "the default revision is written, not omitted");

  const std::filesystem::path legacy = temp.path() / "legacy.json";
  WriteFile(legacy,
            R"({"scenario":"test","metrics":{"p50_wall_ms":1,"p95_wall_ms":1,"max_wall_ms":1,)"
            R"("p50_allocations":1,"p95_allocations":1,"max_allocations":1},"tolerances":{}})");
  const std::optional<perf::BaselineRecord> old = perf::LoadBaseline(legacy);
  Expect(old.has_value() && old->measurement_revision == 1,
         "a pre-field baseline is a revision-1 baseline");
}

void TestPerfDeterministicMergeTakesTheDeclaredRevision() {
  perf::BaselineRecord committed = MakeBaseline();
  committed.measurement_revision = 1;
  committed.metrics.p50_wall_ms = 100.0;
  perf::BaselineRecord fresh = MakeBaseline();
  fresh.measurement_revision = 3;
  fresh.metrics.p50_wall_ms = 999.0;
  fresh.metrics.p50_allocations = 42.0;

  const perf::BaselineRecord merged = perf::MergeDeterministicMetrics(committed, fresh);
  Expect(merged.measurement_revision == 3, "the merged record carries the declared revision");
  Expect(merged.metrics.p50_wall_ms == 100.0,
         "while the timing half this run was not entitled to take is preserved");
  Expect(merged.metrics.p50_allocations == 42.0, "and the deterministic half is taken");
}

void TestPerfBaselineNamesTailOnlyAllocationDivergence() {
  perf::BaselineRecord baseline = MakeBaseline();
  baseline.metrics.p50_allocations = 101.0;
  baseline.metrics.p95_allocations = 120.0;
  baseline.metrics.max_allocations = 681.0;

  perf::Aggregate aggregate;
  aggregate.scenario_name = "test";
  aggregate.metrics = perf::MetricSet{
      .p50_wall_ms = 100.0,
      .p95_wall_ms = 100.0,
      .max_wall_ms = 100.0,
      .p50_allocations = 101.0,
      .p95_allocations = 9000.0,
      .max_allocations = 9364.0,
  };

  const perf::BaselineComparison comparison = perf::CompareToBaseline(baseline, aggregate);
  Expect(!comparison.passed, "a 13x tail is still a failure — this only explains it");
  std::size_t named = 0;
  for (const perf::MetricComparison& metric : comparison.metrics) {
    if (metric.metric == "p95_allocations" || metric.metric == "max_allocations") {
      Expect(metric.note.find("TAIL-ONLY DIVERGENCE") != std::string::npos,
             std::string("the tail metric must name the shape: ") + metric.metric);
      ++named;
    }
    if (metric.metric == "p50_allocations") {
      Expect(metric.note.empty(), "the matching median has nothing to say");
    }
  }
  Expect(named == 2, "both tail metrics carry the diagnosis");

  perf::Aggregate median_moved = aggregate;
  median_moved.metrics.p50_allocations = 5000.0;
  for (const perf::MetricComparison& metric :
       perf::CompareToBaseline(baseline, median_moved).metrics) {
    Expect(metric.note.find("TAIL-ONLY DIVERGENCE") == std::string::npos,
           "a moved median is a code regression, and must not be explained away");
  }

  perf::Aggregate small_tail = aggregate;
  small_tail.metrics.p95_allocations = 145.0;
  small_tail.metrics.max_allocations = 1100.0;
  for (const perf::MetricComparison& metric :
       perf::CompareToBaseline(baseline, small_tail).metrics) {
    if (metric.metric == "p95_allocations") {
      Expect(metric.note.find("TAIL-ONLY DIVERGENCE") == std::string::npos,
             "a 1.2x tail is ordinary drift, not the isolation signature");
    }
  }
}

void TestPerfBaselineDeclinesTheNetHeapGateOnAShortRun() {
  const auto run_of = [](std::size_t iteration_count, double net_heap) {
    perf::Aggregate aggregate;
    aggregate.scenario_name = "test";
    aggregate.metrics.p50_net_heap_bytes = net_heap;
    for (std::size_t index = 0; index < iteration_count; ++index) {
      perf::Iteration iteration;
      iteration.index = index;
      aggregate.iterations.push_back(std::move(iteration));
    }
    return aggregate;
  };
  const auto net_heap = [](const perf::BaselineComparison& comparison) {
    for (const perf::MetricComparison& metric : comparison.metrics) {
      if (metric.metric == "p50_net_heap_bytes") {
        return metric;
      }
    }
    Expect(false, "the comparison must report the net-heap metric at all");
    return perf::MetricComparison{};
  };

  perf::BaselineRecord baseline = MakeBaseline();
  baseline.has_net_heap_metrics = true;
  baseline.metrics.p50_net_heap_bytes = 59'735.0;
  baseline.tolerances.net_heap_percent = 10.0;
  baseline.iterations = 10;

  const perf::BaselineComparison short_run =
      perf::CompareToBaseline(baseline, run_of(5, 297'236.0));
  const perf::MetricComparison short_metric = net_heap(short_run);
  Expect(!short_metric.passed, "the raw arithmetic must still say the reading is out of envelope");
  Expect(!short_metric.enforced,
         "but a five-iteration run must not be gated against a ten's baseline");
  Expect(short_run.passed, "and the scenario must not go red on a comparison that was declined");
  Expect(short_metric.note.find("5 iterations") != std::string::npos &&
             short_metric.note.find("10") != std::string::npos,
         "the note must name both counts");
  Expect(short_metric.note.find("TD-2026-08-10-173") != std::string::npos,
         "and point at the analysis");

  perf::Aggregate short_with_alloc_regression = run_of(5, 297'236.0);
  short_with_alloc_regression.metrics.p50_allocations = 5'000.0;
  Expect(!perf::CompareToBaseline(baseline, short_with_alloc_regression).passed,
         "declining the net-heap gate must not disarm the allocation gates beside it");

  const perf::BaselineComparison at_count =
      perf::CompareToBaseline(baseline, run_of(10, 297'236.0));
  Expect(net_heap(at_count).enforced && !at_count.passed,
         "at the recorded iteration count the retention gate is enforced again");

  const perf::BaselineComparison long_run =
      perf::CompareToBaseline(baseline, run_of(25, 49'334.0));
  Expect(net_heap(long_run).enforced, "a longer run stays gated");
  Expect(net_heap(long_run).note.find("loose") != std::string::npos,
         "and says the gate it passed is looser than the baseline describes");
}

}

void RegisterPerfBaselineTests(std::vector<TestCase>& tests) {
  AddTest(tests, "PerfBaseline/DeclinesTheNetHeapGateOnAShortRun",
          TestPerfBaselineDeclinesTheNetHeapGateOnAShortRun);
  AddTest(tests, "PerfBaseline/RefusesToGateAcrossAMeasurementRevision",
          TestPerfBaselineRefusesToGateAcrossAMeasurementRevision);
  AddTest(tests, "PerfBaseline/MeasurementRevisionRoundTrip",
          TestPerfBaselineMeasurementRevisionRoundTrip);
  AddTest(tests, "PerfBaseline/DeterministicMergeTakesTheDeclaredRevision",
          TestPerfDeterministicMergeTakesTheDeclaredRevision);
  AddTest(tests, "PerfBaseline/NamesTailOnlyAllocationDivergence",
          TestPerfBaselineNamesTailOnlyAllocationDivergence);
  AddTest(tests, "PerfBaseline/DeterministicMergeKeepsTheTimingHalf",
          TestPerfDeterministicMergeKeepsTheTimingHalf);
  AddTest(tests, "PerfBaseline/DeterministicMergeDropsPhasesNoLongerMeasured",
          TestPerfDeterministicMergeDropsPhasesNoLongerMeasured);
  AddTest(tests, "PerfBaseline/PhaseGateCatchesWhatTheTotalCannot",
          TestPerfPhaseGateCatchesWhatTheTotalCannot);
  AddTest(tests, "PerfBaseline/PhaseGateFailsWhenAPhaseStopsBeingMeasured",
          TestPerfPhaseGateFailsWhenAPhaseStopsBeingMeasured);
  AddTest(tests, "PerfBaseline/PhaseGateIsSilentUntilTheBaselineRecordsPhases",
          TestPerfPhaseGateIsSilentUntilTheBaselineRecordsPhases);
  AddTest(tests, "PerfBaseline/PhaseBaselineRoundTrip", TestPerfPhaseBaselineRoundTrip);
  AddTest(tests, "PerfBaseline/PhaseAggregationSumsRepeatsWithinAnIteration",
          TestPerfPhaseAggregationSumsRepeatsWithinAnIteration);
  AddTest(tests, "PerfBaseline/EnvelopeConsumptionSeesADriftThatPassed",
          TestPerfEnvelopeConsumptionSeesADriftThatPassed);
  AddTest(tests, "PerfBaseline/EnvelopeConsumptionAgreesWithTheGate",
          TestPerfEnvelopeConsumptionAgreesWithTheGate);
  AddTest(tests, "PerfBaseline/NormalisesCpuAgainstTheBaselineClock",
          TestPerfBaselineNormalisesCpuAgainstTheBaselineClock);
  AddTest(tests, "PerfBaseline/NormalisesWallByItsWorkFraction",
          TestPerfBaselineNormalisesWallByItsWorkFraction);
  AddTest(tests, "PerfBaseline/AdvisoryTimingHalfGatesAllocationsOnly",
          TestPerfBaselineAdvisoryTimingHalfGatesAllocationsOnly);
  AddTest(tests, "PerfBaseline/DeclinesComparisonAcrossBuildConfigurations",
          TestPerfBaselineDeclinesComparisonAcrossBuildConfigurations);
  AddTest(tests, "PerfBaseline/WallEnvelopeComesFromMeasuredSpread",
          TestPerfBaselineWallEnvelopeComesFromMeasuredSpread);
  AddTest(tests, "PerfBaseline/NormalisesEachIterationAgainstItsOwnClock",
          TestPerfBaselineNormalisesEachIterationAgainstItsOwnClock);
  AddTest(tests, "PerfBaseline/ClampsAnAbsurdClockFactor",
          TestPerfBaselineClampsAnAbsurdClockFactor);
  AddTest(tests, "PerfBaseline/ClockNormalisationTouchesCpuOnly",
          TestPerfBaselineClockNormalisationTouchesCpuOnly);
  AddTest(tests, "PerfBaseline/CalibrationRoundTrip", TestPerfBaselineCalibrationRoundTrip);
  AddTest(tests, "PerfBaseline/RssGateSeesPeriodicRetention",
          TestPerfBaselineRssGateSeesPeriodicRetention);
  AddTest(tests, "PerfBaseline/LeavesRssUngatedWhenOnlyPercentilesRecorded",
          TestPerfBaselineLeavesRssUngatedWhenOnlyPercentilesRecorded);
  AddTest(tests, "PerfBaseline/CalibrationSpreadFlagsAMovingClock",
          TestPerfCalibrationSpreadFlagsAMovingClock);
  AddTest(tests, "PerfBaseline/RecipesDoNotPinAVideoDriver",
          TestPerfRecipesDoNotPinAVideoDriver);
  AddTest(tests, "PerfBaseline/AllowsImprovements", TestPerfBaselineComparisonAllowsImprovements);
  AddTest(tests, "PerfBaseline/RejectsRegressions", TestPerfBaselineComparisonRejectsRegressions);
  AddTest(tests, "PerfBaseline/DecouplesWallAndAllocationTolerances",
          TestPerfBaselineDecouplesWallAndAllocationTolerances);
  AddTest(tests, "PerfBaseline/LoadDefaultsAllocationToleranceToWall",
          TestPerfBaselineLoadDefaultsAllocationToleranceToWall);
  AddTest(tests, "PerfBaseline/ToleranceRoundTrip", TestPerfBaselineToleranceRoundTrip);
  AddTest(tests, "PerfBaseline/LeavesCpuAndRssUngatedWhenAbsent",
          TestPerfBaselineLeavesCpuAndRssUngatedWhenAbsent);
  AddTest(tests, "PerfBaseline/GatesCpuAndRssWhenRecorded",
          TestPerfBaselineGatesCpuAndRssWhenRecorded);
  AddTest(tests, "PerfBaseline/RssNoiseFloorAbsorbsSubPageJitter",
          TestPerfBaselineRssNoiseFloorAbsorbsSubPageJitter);
  AddTest(tests, "PerfBaseline/DeclinesTheResidentGateOnAShortRun",
          TestPerfBaselineDeclinesTheResidentGateOnAShortRun);
  AddTest(tests, "PerfBaseline/IterationCountRoundTrip",
          TestPerfBaselineIterationCountRoundTrip);
  AddTest(tests, "PerfBaseline/CpuAndRssRoundTrip", TestPerfBaselineCpuAndRssRoundTrip);
  AddTest(tests, "PerfBaseline/UngatedCpuIsOmittedNotZeroed",
          TestPerfBaselineUngatedCpuIsOmittedNotZeroed);
  AddTest(tests, "PerfBaseline/DeterministicMergeDoesNotMintAResidentGate",
          TestPerfBaselineDeterministicMergeDoesNotMintAResidentGate);
}

}
