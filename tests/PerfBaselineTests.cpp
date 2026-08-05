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

// Wall and allocation tolerances are independent: a scenario can widen its wall
// envelope for machine jitter while keeping a tight allocation complexity gate.
void TestPerfBaselineDecouplesWallAndAllocationTolerances() {
  perf::BaselineRecord baseline = MakeBaseline();
  baseline.tolerances = perf::Tolerances{
      .p50_percent = 75.0,   // wall: wide (absorbs jitter)
      .p95_percent = 250.0,
      .max_percent = 400.0,
      .alloc_p50_percent = 10.0,  // allocations: tight (deterministic)
      .alloc_p95_percent = 20.0,
      .alloc_max_percent = 50.0,
  };

  // A large wall rise within the wide wall envelope but flat allocations passes.
  perf::Aggregate wall_only;
  wall_only.scenario_name = "test";
  wall_only.metrics = perf::MetricSet{
      .p50_wall_ms = 160.0,  // +60% < 75%
      .p95_wall_ms = 160.0,
      .max_wall_ms = 160.0,
      .p50_allocations = 105.0,  // +5% < 10%
      .p95_allocations = 105.0,
      .max_allocations = 105.0,
  };
  Expect(perf::CompareToBaseline(baseline, wall_only).passed,
         "a wall rise within the wide wall envelope with flat allocations should pass");

  // The same wall rise but a modest allocation rise beyond the tight allocation
  // envelope fails -- the loose wall envelope must not blind the allocation gate.
  perf::Aggregate alloc_regression = wall_only;
  alloc_regression.metrics.p50_allocations = 120.0;  // +20% > 10%
  Expect(!perf::CompareToBaseline(baseline, alloc_regression).passed,
         "an allocation rise beyond the tight allocation envelope must fail even when wall is wide");
}

// A baseline written before the wall/alloc split (no allocation tolerance keys)
// must default its allocation envelopes to the wall envelopes, so pre-decoupling
// baselines gate exactly as they did before.
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

// Decoupled tolerances survive a Save -> Load round trip.
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

// The committed wall baselines are only reproducible on the harness's own lane
// (`--video=dummy --renderer=software`, both applied before SDL_Init). Wrapping
// microide_perf in `xvfb-run` or exporting SDL_VIDEODRIVER charges real
// window-system present cost: measured 2-12x on every frame-pumping scenario,
// ~1.0x on the pure-unit ones, with byte-identical allocation counts. That shape
// is indistinguishable from a broad code regression -- it survived two sessions
// of investigation, because a whole-suite A/B against the baseline commit
// reproduces it (both sides windowed) and the allocation oracle agrees with the
// baseline (allocations do not depend on the lane). The recipe reached people
// through the committed docs and tooling, so that is where it is pinned shut.
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
    // Loud-missing-target guard: if these files stop naming the binary, the scan
    // below passes vacuously and this lint is dead.
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

    // Scan line by line so the prose that explains *why* the wrong lane is wrong
    // (which necessarily names it) does not trip the rule -- only a line that
    // pins a driver *and* runs the binary, or a shell line that pins one at all.
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


// The 93 baselines recorded before cpu/rss existed carry neither metric. Loading
// one must mark them ungated rather than comparing the measured value against an
// implicit 0.0, which would fail every scenario the moment the metrics shipped.
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

  // And the comparison must not invent cpu/rss rows for it.
  perf::Aggregate aggregate;
  aggregate.scenario_name = "legacy";
  aggregate.metrics = perf::MetricSet{
      .p50_wall_ms = 10.0, .p95_wall_ms = 10.0, .max_wall_ms = 10.0,
      .p50_allocations = 10.0, .p95_allocations = 10.0, .max_allocations = 10.0,
      .p50_cpu_ms = 999.0, .p95_cpu_ms = 999.0, .max_cpu_ms = 999.0,
      .p50_rss_growth_bytes = 9.0e8, .p95_rss_growth_bytes = 9.0e8,
      .max_rss_growth_bytes = 9.0e8,
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

// Once a baseline does record them, they gate like any other metric.
void TestPerfBaselineGatesCpuAndRssWhenRecorded() {
  perf::BaselineRecord baseline = MakeBaseline();
  baseline.has_cpu_metrics = true;
  baseline.has_rss_metrics = true;
  baseline.metrics.p50_cpu_ms = 100.0;
  baseline.metrics.p95_cpu_ms = 100.0;
  baseline.metrics.max_cpu_ms = 100.0;
  baseline.metrics.p50_rss_growth_bytes = 1'000'000.0;
  baseline.metrics.p95_rss_growth_bytes = 1'000'000.0;
  baseline.metrics.max_rss_growth_bytes = 1'000'000.0;
  baseline.tolerances.cpu_p50_percent = 10.0;
  baseline.tolerances.rss_p50_percent = 25.0;

  // Wall time held constant while CPU doubles: exactly the shape of "the work moved
  // onto background threads". Wall-only gating calls this neutral.
  perf::Aggregate cpu_regression;
  cpu_regression.scenario_name = baseline.scenario_name;
  cpu_regression.metrics = baseline.metrics;
  cpu_regression.metrics.p50_cpu_ms = 200.0;
  Expect(!perf::CompareToBaseline(baseline, cpu_regression).passed,
         "doubling CPU time at constant wall time must fail the baseline");

  perf::Aggregate rss_regression;
  rss_regression.scenario_name = baseline.scenario_name;
  rss_regression.metrics = baseline.metrics;
  rss_regression.metrics.p50_rss_growth_bytes = 4'000'000.0;
  Expect(!perf::CompareToBaseline(baseline, rss_regression).passed,
         "quadrupling resident growth must fail the baseline");

  perf::Aggregate within;
  within.scenario_name = baseline.scenario_name;
  within.metrics = baseline.metrics;
  within.metrics.p50_cpu_ms = 105.0;
  within.metrics.p50_rss_growth_bytes = 1'100'000.0;
  Expect(perf::CompareToBaseline(baseline, within).passed,
         "cpu/rss inside their envelopes should pass");
}

// A scenario that grows the resident set by essentially nothing has a near-zero
// baseline, where a percentage envelope collapses and a single page of allocator
// noise reads as an unbounded regression. Below one page there is no signal.
void TestPerfBaselineRssNoiseFloorAbsorbsSubPageJitter() {
  perf::BaselineRecord baseline = MakeBaseline();
  baseline.has_rss_metrics = true;
  baseline.metrics.p50_rss_growth_bytes = 0.0;
  baseline.metrics.p95_rss_growth_bytes = 0.0;
  baseline.metrics.max_rss_growth_bytes = 0.0;

  perf::Aggregate one_page;
  one_page.scenario_name = baseline.scenario_name;
  one_page.metrics = baseline.metrics;
  one_page.metrics.p50_rss_growth_bytes = 4096.0;
  Expect(perf::CompareToBaseline(baseline, one_page).passed,
         "a single page of growth over a zero baseline must not be a regression");

  perf::Aggregate real_growth;
  real_growth.scenario_name = baseline.scenario_name;
  real_growth.metrics = baseline.metrics;
  real_growth.metrics.p50_rss_growth_bytes = 64.0 * 1024.0 * 1024.0;
  Expect(!perf::CompareToBaseline(baseline, real_growth).passed,
         "64 MiB of growth over a zero baseline is a real regression, not noise");
}

// cpu/rss survive a Save -> Load round trip and come back gated.
void TestPerfBaselineCpuAndRssRoundTrip() {
  TemporaryDirectory temp;
  const std::filesystem::path path = temp.path() / "cpu_rss_roundtrip.json";
  perf::BaselineRecord baseline = MakeBaseline();
  baseline.metrics.p50_cpu_ms = 12.5;
  baseline.metrics.p95_cpu_ms = 18.5;
  baseline.metrics.max_cpu_ms = 24.5;
  baseline.metrics.p50_rss_growth_bytes = 131072.0;
  baseline.metrics.p95_rss_growth_bytes = 262144.0;
  baseline.metrics.max_rss_growth_bytes = 524288.0;
  // Load-bearing on SAVE as well as load: a scenario with gate_cpu_metrics=false
  // must not write cpu metrics at all (see the sibling test below).
  baseline.has_cpu_metrics = true;
  Expect(perf::SaveBaseline(path, baseline), "saving a baseline with cpu/rss should succeed");

  const std::optional<perf::BaselineRecord> loaded = perf::LoadBaseline(path);
  Expect(loaded.has_value(), "a saved cpu/rss baseline should load");
  Expect(loaded->has_cpu_metrics && loaded->has_rss_metrics,
         "a round-tripped baseline should come back gated");
  Expect(loaded->metrics.p50_cpu_ms == 12.5, "cpu p50 should survive the round trip");
  Expect(loaded->metrics.max_rss_growth_bytes == 524288.0,
         "rss max should survive the round trip");
}

// A scenario that opts out of CPU gating (Scenario::gate_cpu_metrics = false,
// used by idle_soak_30s -- see TD-2026-08-05-137) must have the cpu metrics
// OMITTED from its baseline, not written as zeros. LoadBaseline infers
// has_cpu_metrics from whether p50_cpu_ms is a number, so a zero would come back
// gated and hold every future run against a 0 ms budget -- the opt-out would
// invert into the tightest gate in the suite.
void TestPerfBaselineUngatedCpuIsOmittedNotZeroed() {
  TemporaryDirectory temp;
  const std::filesystem::path path = temp.path() / "cpu_ungated.json";
  perf::BaselineRecord baseline = MakeBaseline();
  baseline.metrics.p50_cpu_ms = 12.5;
  baseline.metrics.p95_cpu_ms = 18.5;
  baseline.metrics.max_cpu_ms = 24.5;
  baseline.metrics.p50_rss_growth_bytes = 131072.0;
  baseline.has_cpu_metrics = false;
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

  // The comparison must then report no cpu metric at all, rather than a 0 ms
  // expectation that anything above zero fails.
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

// TD-2026-08-05-137: harness.cpu_calibration_ns was recorded and never read, so
// working out that a cpu_ms failure was the governor rather than the code meant
// scraping --report-json by hand. The spread across a run is what carries that
// signal, and it must stay silent on a machine holding one clock.
void TestPerfCalibrationSpreadFlagsAMovingClock() {
  const auto make_run = [](std::initializer_list<std::uint64_t> calibrations) {
    perf::Aggregate aggregate;
    std::size_t index = 0;
    for (const std::uint64_t nanoseconds : calibrations) {
      perf::Iteration iteration;
      iteration.index = index++;
      // A real iteration carries application counters beside the probe; the
      // scan must pick out its own and ignore those.
      iteration.perf_counters.emplace_back("editor.line_materializations", 4200);
      iteration.perf_counters.emplace_back("harness.cpu_calibration_ns", nanoseconds);
      aggregate.iterations.push_back(std::move(iteration));
    }
    return aggregate;
  };

  // The numbers from the reproduction in the TD: a clean 671 -> 857 us step.
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

  // A machine holding one clock drifts about 1%, which must not be reported --
  // a note on every scenario would be noise, and noise is how a real one gets
  // skipped.
  const perf::CalibrationSpread steady =
      perf::MeasureCalibrationSpread(make_run({670700, 673100, 671900, 674300}));
  Expect(steady.valid, "a steady run still records the probe");
  Expect(steady.ratio < perf::kCalibrationSpreadNoteRatio,
         "ordinary drift must stay under the note threshold");

  // A scenario whose run recorded no probe at all (an older report, or a lane
  // that skipped it) must not synthesise one.
  perf::Aggregate probeless;
  probeless.iterations.push_back(perf::Iteration{});
  const perf::CalibrationSpread absent = perf::MeasureCalibrationSpread(probeless);
  Expect(!absent.valid, "a run without the probe has no spread");
  Expect(perf::DescribeCalibrationSpread(absent).empty(),
         "and must describe nothing rather than a 1x range of zeros");
}

}  // namespace

void RegisterPerfBaselineTests(std::vector<TestCase>& tests) {
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
  AddTest(tests, "PerfBaseline/CpuAndRssRoundTrip", TestPerfBaselineCpuAndRssRoundTrip);
  AddTest(tests, "PerfBaseline/UngatedCpuIsOmittedNotZeroed",
          TestPerfBaselineUngatedCpuIsOmittedNotZeroed);
}

}  // namespace microide::tests
