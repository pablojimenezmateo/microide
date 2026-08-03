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

}  // namespace

void RegisterPerfBaselineTests(std::vector<TestCase>& tests) {
  AddTest(tests, "PerfBaseline/RecipesDoNotPinAVideoDriver",
          TestPerfRecipesDoNotPinAVideoDriver);
  AddTest(tests, "PerfBaseline/AllowsImprovements", TestPerfBaselineComparisonAllowsImprovements);
  AddTest(tests, "PerfBaseline/RejectsRegressions", TestPerfBaselineComparisonRejectsRegressions);
  AddTest(tests, "PerfBaseline/DecouplesWallAndAllocationTolerances",
          TestPerfBaselineDecouplesWallAndAllocationTolerances);
  AddTest(tests, "PerfBaseline/LoadDefaultsAllocationToleranceToWall",
          TestPerfBaselineLoadDefaultsAllocationToleranceToWall);
  AddTest(tests, "PerfBaseline/ToleranceRoundTrip", TestPerfBaselineToleranceRoundTrip);
}

}  // namespace microide::tests
