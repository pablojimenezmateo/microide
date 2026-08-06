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

// TD-2026-08-06-139: five allocation gates drifted up and every one reported
// PASS, because a pass/fail bit cannot distinguish "unchanged" from "one
// allocation short of red". `EnvelopeUsedPercent` is what the run reports
// instead, so a near-miss is visible in the run that passed it.
//
// The exact case that went unseen is the headline assertion here: +9.4% against
// a +10% tolerance is 94% of the envelope, and 94 >= the 75% notice threshold.
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

  // The real numbers from the entry: editor_mouse_selection_drag, 5010 -> 5482
  // against the default 10% allocation tolerance.
  const perf::MetricComparison drifted = metric(5010.0, 5482.0, 10.0);
  Expect(drifted.passed, "the drift under test is one the gate PASSED — that is the point");
  const double used = perf::EnvelopeUsedPercent(drifted);
  Expect(used > 90.0 && used < 100.0,
         "a +9.4% move against a +10% envelope must report as ~94% of the envelope");
  Expect(used >= perf::kEnvelopeNoticePercent,
         "the drift that went unnoticed for two days must clear the notice threshold");

  // An unchanged scenario must stay silent, or the notice is noise and gets
  // ignored — which is the same defect one layer up.
  Expect(perf::EnvelopeUsedPercent(metric(5010.0, 5010.0, 10.0)) == 0.0,
         "an unchanged measurement consumes none of its envelope");
  Expect(perf::EnvelopeUsedPercent(metric(5010.0, 5100.0, 10.0)) < perf::kEnvelopeNoticePercent,
         "a small move well inside the envelope must not be reported");

  // An improvement reads negative, not as pressure.
  Expect(perf::EnvelopeUsedPercent(metric(5010.0, 2101.0, 10.0)) < 0.0,
         "code that got faster must consume negative envelope, not positive");

  // A failure reads above 100, so one number orders passes and failures alike.
  const perf::MetricComparison failed = metric(5010.0, 6000.0, 10.0);
  Expect(!failed.passed, "the control must actually fail");
  Expect(perf::EnvelopeUsedPercent(failed) > 100.0,
         "a metric past its tolerance must consume more than the whole envelope");

  // Degenerate inputs must not produce infinities that would sort to the top of
  // the headroom summary forever.
  Expect(perf::EnvelopeUsedPercent(metric(0.0, 10.0, 10.0)) == 0.0,
         "a zero baseline has no ratio to report");
  Expect(perf::EnvelopeUsedPercent(metric(100.0, 110.0, 0.0)) == 0.0,
         "a zero tolerance has no envelope to consume");
}

// The gate's own comparison must agree with the envelope arithmetic: a metric
// reported at <=100% of its envelope has to be a metric the gate passed, and
// vice versa. Two numbers describing one decision that could disagree is how a
// summary starts lying (validation-traps.md).
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
      // Strictly past the envelope must be a failure; strictly inside must pass.
      // Exactly 100.0 is the boundary and is left to the comparison to define.
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

// Once a baseline does record them, they gate like any other metric.
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

// A scenario that grows the resident set by essentially nothing has a near-zero
// baseline, where a percentage envelope collapses and a single page of allocator
// noise reads as an unbounded regression. Below one page there is no signal.
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

// TD-2026-08-06-148: `mean_rss_growth_bytes` is a trimmed mean over a SETTLING
// series, so its value depends on how many iterations the run averaged --
// typing_large_file read 84-95 KB at the default 10 iterations and 100-114 KB at
// 6, one binary, one quiet box. Against its committed baseline that is green at
// 10 and red about half the time at 6, and the failure said "measured=113869
// (+41%)" with nothing about the sample size. A short run is not a measurement
// against a long baseline, so the gate declines it and says so.
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

  // The real numbers from the entry.
  perf::BaselineRecord baseline = MakeBaseline();
  baseline.has_rss_metrics = true;
  baseline.metrics.mean_rss_growth_bytes = 80'555.0;
  baseline.tolerances.rss_mean_percent = 25.0;
  baseline.iterations = 10;

  // The reading that turned the gate red at six iterations. It is genuinely past
  // the envelope -- the point is that the comparison is meaningless, not that the
  // arithmetic is wrong.
  const perf::BaselineComparison short_run =
      perf::CompareToBaseline(baseline, run_of(6, 113'869.0));
  const perf::MetricComparison short_metric = resident(short_run);
  Expect(!short_metric.passed, "the raw arithmetic must still say the reading is out of envelope");
  Expect(!short_metric.enforced, "but a six-iteration run must not be gated against a ten's baseline");
  Expect(short_run.passed, "and the scenario must not go red on a comparison that was declined");
  Expect(short_metric.note.find("6 iterations") != std::string::npos &&
             short_metric.note.find("10") != std::string::npos,
         "the note must name both counts, which is the thing the failure line never said");
  Expect(short_metric.note.find("TD-2026-08-06-148") != std::string::npos,
         "and point at the analysis");

  // Everything else in the same short run stays gated. Allocation counts do not
  // care how many iterations were averaged, and exempting them would turn a
  // --iterations=6 run into a run with no gate at all.
  perf::Aggregate short_with_alloc_regression = run_of(6, 113'869.0);
  short_with_alloc_regression.metrics.p50_allocations = 5'000.0;
  Expect(!perf::CompareToBaseline(baseline, short_with_alloc_regression).passed,
         "declining the resident gate must not disarm the allocation gates beside it");

  // At the recorded count the gate is live in both directions.
  const perf::BaselineComparison at_count =
      perf::CompareToBaseline(baseline, run_of(10, 113'869.0));
  Expect(resident(at_count).enforced && !at_count.passed,
         "at the baseline's own iteration count the same reading must fail");
  Expect(resident(at_count).note.empty(),
         "and an ordinary comparison must say nothing at all");
  Expect(perf::CompareToBaseline(baseline, run_of(10, 92'000.0)).passed,
         "a reading inside the envelope at the recorded count still passes");

  // A LONGER run reads lower, so the gate can only get looser. Still enforced --
  // it can only produce false greens, never false reds -- but said out loud,
  // because a gate nobody knows is loose is how a baseline set goes vacuous.
  const perf::BaselineComparison long_run =
      perf::CompareToBaseline(baseline, run_of(25, 113'869.0));
  Expect(resident(long_run).enforced && !long_run.passed,
         "a longer run stays gated: it cannot be fooled high by the settling passes");
  Expect(long_run.metrics.size() == at_count.metrics.size(),
         "and reports the same metric set");
  Expect(resident(long_run).note.find("loose") != std::string::npos,
         "but says the comparison is more permissive than the baseline describes");

  // A baseline written before the field existed carries no count, and must gate
  // exactly as it did -- otherwise adding the field would silently unenforce the
  // whole committed set.
  perf::BaselineRecord unversioned = baseline;
  unversioned.iterations = 0;
  const perf::BaselineComparison legacy =
      perf::CompareToBaseline(unversioned, run_of(6, 113'869.0));
  Expect(resident(legacy).enforced && !legacy.passed,
         "a baseline with no recorded iteration count must gate as it always did");

  // Neither must an aggregate that carries only summary metrics (report replay,
  // and most of the tests in this file) be treated as a zero-iteration run.
  perf::Aggregate summary_only;
  summary_only.scenario_name = "test";
  summary_only.metrics.mean_rss_growth_bytes = 113'869.0;
  const perf::BaselineComparison replayed = perf::CompareToBaseline(baseline, summary_only);
  Expect(resident(replayed).enforced && !replayed.passed,
         "an aggregate with no per-iteration detail must not read as a short run");
}

// The iteration count survives a Save -> Load round trip, and is omitted rather
// than zeroed when unknown. A zero that came back as "recorded over 0 iterations"
// would make every run longer than its baseline and annotate the whole suite.
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

// cpu/rss survive a Save -> Load round trip and come back gated.
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

// Build an aggregate whose iterations each carry a cpu_ms and the calibration
// reading taken beside it, and whose MetricSet is percentiled from them exactly as
// the harness does.
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
  // Wall and allocations flat and equal to the baseline: this fixture is about the
  // CPU gate only, so nothing else may decide the verdict.
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
  // Tight, so the test is about normalisation and not about the suite's wide
  // default CPU envelope absorbing everything.
  baseline.tolerances.cpu_p50_percent = 10.0;
  baseline.tolerances.cpu_p95_percent = 10.0;
  baseline.tolerances.cpu_max_percent = 10.0;
  baseline.has_calibration = calibration_ns != 0;
  baseline.metrics.p50_cpu_calibration_ns = static_cast<double>(calibration_ns);
  return baseline;
}

// TD-2026-08-05-137 item 2: cpu_ms is a duration, so a baseline captured on a core
// at one clock is not a budget for the same code on a core at another. The
// baseline now records the clock it was captured at, and the comparison re-expresses
// the run in it.
void TestPerfBaselineNormalisesCpuAgainstTheBaselineClock() {
  // Captured at 670 us of calibration, costing 10 ms of CPU.
  const perf::BaselineRecord baseline = MakeCpuBaseline(10.0, 670000);

  // Same code, machine 1.4x slower: every duration is 1.4x, calibration included.
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

  // The same +40% on a machine holding the baseline's clock is a real regression,
  // and normalisation must not hide it. This is the negative control: without it
  // the test above passes just as well against a gate that stopped gating.
  const perf::Aggregate real_regression = MakeCpuRun({{14.0, 670000},
                                                      {14.0, 670000},
                                                      {14.0, 670000},
                                                      {14.0, 670000}});
  Expect(!perf::CompareToBaseline(baseline, real_regression).passed,
         "a +40% cpu rise at an unchanged clock must still fail");

  // And a baseline that never recorded a clock compares raw, exactly as before.
  perf::BaselineRecord no_clock = baseline;
  no_clock.has_calibration = false;
  no_clock.metrics.p50_cpu_calibration_ns = 0.0;
  const perf::BaselineComparison unnormalised =
      perf::CompareToBaseline(no_clock, slower_machine);
  Expect(!unnormalised.passed,
         "a baseline with no recorded clock must compare raw rather than invent a factor");
  Expect(!unnormalised.clock.applied, "and must report that it did not normalise");
}

// The failure that opened the TD was a clock that stepped MID-run: five iterations
// at one clock and five at another, one clean step, application counters identical
// across it. A single per-run factor would smear that across both halves; each
// iteration is normalised against its own reading instead.
void TestPerfBaselineNormalisesEachIterationAgainstItsOwnClock() {
  const perf::BaselineRecord baseline = MakeCpuBaseline(14.0, 671000);
  // The reproduction's own shape: unchanged code, clock steps 671 -> 857 us at the
  // halfway point, cpu_ms steps with it.
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

  // A regression that appears only in the slow half is still visible after
  // normalisation: it is the ratio that carries the signal, not the raw value.
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

// A factor far outside the range a governor can produce is much more likely a
// broken probe or a baseline from another machine class. Scaling a gate by it
// silently would make the gate vacuous, which is the failure mode this whole area
// exists to prevent.
void TestPerfBaselineClampsAnAbsurdClockFactor() {
  const perf::BaselineRecord baseline = MakeCpuBaseline(10.0, 670000);
  const perf::Aggregate absurd = MakeCpuRun({{100.0, 67000000}, {100.0, 67000000}});
  const perf::BaselineComparison comparison = perf::CompareToBaseline(baseline, absurd);
  Expect(comparison.clock.clamped, "a 100x clock reading should report as clamped");
  Expect(!comparison.passed,
         "and must not pass: clamping means the gate stays enforced at 3x, not disabled");
}

// Normalisation is a CPU-only correction. Allocation counts do not depend on the
// clock at all -- their agreeing across a clock step is the EVIDENCE that a cpu
// failure is the machine -- and resident growth does not either.
void TestPerfBaselineClockNormalisationTouchesCpuOnly() {
  perf::BaselineRecord baseline = MakeCpuBaseline(10.0, 670000);
  baseline.has_rss_metrics = true;
  baseline.metrics.mean_rss_growth_bytes = 1'000'000.0;

  perf::Aggregate run = MakeCpuRun({{14.0, 938000}, {14.0, 938000}});
  run.metrics.p50_allocations = 140.0;              // +40%, like the durations
  run.metrics.mean_rss_growth_bytes = 1'400'000.0;  // +40%, like the durations
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

// TD-2026-08-05-136: the resident gate is the trimmed mean, and the two reasons it
// is not a percentile are worth pinning, because both were live defects.
void TestPerfBaselineRssGateSeesPeriodicRetention() {
  perf::BaselineRecord baseline = MakeBaseline();
  baseline.has_rss_metrics = true;
  baseline.tolerances.rss_mean_percent = 25.0;
  // A scenario that retains ~1 MB on every other iteration: its p50 is exactly
  // zero, which is what merge_scroll_large_fixture's gate said while it grew by
  // ~972 KB per iteration. The mean says 500 KB, so the gate has something to hold.
  baseline.metrics.mean_rss_growth_bytes = 500'000.0;
  baseline.metrics.p50_rss_growth_bytes = 0.0;

  perf::Aggregate doubled;
  doubled.scenario_name = baseline.scenario_name;
  doubled.metrics = baseline.metrics;
  doubled.metrics.mean_rss_growth_bytes = 1'000'000.0;
  doubled.metrics.p50_rss_growth_bytes = 0.0;  // still zero: the percentile is blind
  Expect(!perf::CompareToBaseline(baseline, doubled).passed,
         "doubling a periodic retention rate must fail even though its p50 stays zero");

  // And the metric that gates is named in the comparison, so a failing run says
  // which number it means.
  bool saw_mean = false;
  for (const auto& metric : perf::CompareToBaseline(baseline, doubled).metrics) {
    saw_mean = saw_mean || metric.metric == "mean_rss_growth_bytes";
    Expect(metric.metric != "p50_rss_growth_bytes",
           "the percentile must not gate: it is recorded for diagnosis only");
  }
  Expect(saw_mean, "the trimmed mean should be the resident row in the comparison");
}

// A baseline that predates the trimmed mean records only percentiles. It must come
// back ungated on resident growth rather than gated against a 0.0 mean, which would
// fail every scenario that grows at all.
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

// The recorded clock survives a Save -> Load round trip, and is omitted rather than
// zeroed when a run had no probe -- a zero would come back as "recorded" and divide
// by it.
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

}  // namespace

void RegisterPerfBaselineTests(std::vector<TestCase>& tests) {
  AddTest(tests, "PerfBaseline/EnvelopeConsumptionSeesADriftThatPassed",
          TestPerfEnvelopeConsumptionSeesADriftThatPassed);
  AddTest(tests, "PerfBaseline/EnvelopeConsumptionAgreesWithTheGate",
          TestPerfEnvelopeConsumptionAgreesWithTheGate);
  AddTest(tests, "PerfBaseline/NormalisesCpuAgainstTheBaselineClock",
          TestPerfBaselineNormalisesCpuAgainstTheBaselineClock);
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
}

}  // namespace microide::tests
