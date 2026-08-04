#pragma once

#include <SDL3/SDL.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "workspace/shell/WorkspaceShell.h"
#include "workspace/WorkspaceVirtualDocument.h"

#include "compare/MergeModel.h"
#include "editor/TextViewport.h"
#include "perf/PerfHarnessIsolation.h"

namespace microide::tests::perf {

// Non-throwing fixture probes. The perf runner is the speed-regression oracle, so
// fixture discovery must fail in controlled, diagnosable ways: a probe error
// (permission denied, symlink loop, stale mount, overlong component) must degrade to
// "fixture absent" so the scenario can print its labelled message and honor
// --require-fixtures, never throw a raw std::filesystem_error mid-run.
inline bool PathExistsNoThrow(const std::filesystem::path& path) {
  std::error_code ec;
  return std::filesystem::exists(path, ec) && !ec;
}

inline bool DirectoryExistsNoThrow(const std::filesystem::path& path) {
  std::error_code ec;
  return std::filesystem::is_directory(path, ec) && !ec;
}

struct MetricSnapshot {
  double wall_ms = 0.0;
  std::uint64_t allocations = 0;
  std::uint64_t frees = 0;
  std::uint64_t bytes_allocated = 0;
  std::uint64_t bytes_freed = 0;
  // Process CPU time (user + system, summed across ALL threads) consumed by the
  // iteration. Wall time alone cannot see priority 3: a change that keeps its
  // latency by moving work onto three background threads reads as neutral on
  // wall and is a large regression here. This is what makes "low CPU usage" a
  // measured budget instead of a stated intention.
  double cpu_ms = 0.0;
  // Resident-set growth across the iteration, in bytes. Deliberately the delta and
  // not absolute RSS: the harness runs every iteration of every scenario in one
  // process, so absolute RSS is dominated by whatever ran earlier and is not
  // attributable to the scenario. Growth is. Clamped at zero when the iteration
  // gives memory back.
  std::uint64_t rss_growth_bytes = 0;
};

struct MetricSet {
  double p50_wall_ms = 0.0;
  double p95_wall_ms = 0.0;
  double max_wall_ms = 0.0;
  double p50_allocations = 0.0;
  double p95_allocations = 0.0;
  double max_allocations = 0.0;
  double p50_cpu_ms = 0.0;
  double p95_cpu_ms = 0.0;
  double max_cpu_ms = 0.0;
  double p50_rss_growth_bytes = 0.0;
  double p95_rss_growth_bytes = 0.0;
  double max_rss_growth_bytes = 0.0;
};

// Process-wide CPU time and resident set, for the harness and for scenarios that
// want to assert on them directly. CPU time comes from getrusage(RUSAGE_SELF),
// which sums every thread; RSS from /proc/self/statm.
double ProcessCpuMilliseconds();
std::uint64_t ProcessResidentBytes();

struct Iteration {
  struct PhaseMetrics {
    std::string name;
    double wall_ms = 0.0;
    std::uint64_t allocations = 0;
    std::uint64_t frees = 0;
    std::uint64_t bytes_allocated = 0;
    std::uint64_t bytes_freed = 0;
  };

  std::size_t index = 0;
  MetricSnapshot metrics;
  std::vector<PhaseMetrics> phase_metrics;
  std::vector<std::pair<std::string, std::uint64_t>> perf_counters;
};

struct Aggregate {
  std::string scenario_name;
  std::vector<Iteration> iterations;
  MetricSet metrics;
  bool smoke = false;
};

struct ReportMetadata {
  std::string runner_class;           // e.g. "perf-runner-v1" or "local-advisory"
  std::string sdl_video_driver;       // e.g. "dummy", "x11", "software"
  std::string sdl_renderer_driver;    // e.g. "software"
  std::vector<std::string> scenarios; // resolved scenario list executed
  std::size_t iterations = 0;
  std::string layout_mode;            // "auto" | "regular" | "compact" | ""
  std::uint64_t seed = 0;
  std::string provenance;             // "reference" | "advisory"
  std::string isolated_app_root;      // path to the per-run sandbox, if any
  std::string cpu_affinity;           // e.g. "0-3,12-15 (8 cpus @ 5157 MHz)", "off"
};


class ScenarioContext {
 public:
  ScenarioContext(workspace::WorkspaceShell& shell, SDL_Window* window, SDL_Renderer* renderer);

  void PumpFrames(std::size_t count);
  void PumpEvents();
  bool Open(const std::filesystem::path& project_root);
  void OpenTab(const std::filesystem::path& path);
  void Type(std::string_view text);
  void Scroll(int vertical_ticks);
  void KeyDown(SDL_Keycode key, SDL_Keymod modifiers = SDL_KMOD_NONE);
  bool ExecuteCommand(std::string_view command_line);
  void ToggleProjectSearchPatternMode();
  void ConsumeProjectSearchUpdates();
  std::uint64_t Wait(std::chrono::milliseconds duration);
  bool WaitForDiagnostics(const std::filesystem::path& path,
                          std::chrono::milliseconds timeout);
  // Pump frames until `relative_path` appears in the project file index (the
  // initial background build has reached it) or the timeout elapses. Used to
  // take the async indexer out of a scenario's measured window so its metrics
  // are deterministic. Returns whether the path was present.
  bool WaitForFileIndexPath(const std::filesystem::path& relative_path,
                            std::chrono::milliseconds timeout);
  // Drain project-search updates, pumping frames, until the active search
  // reports it is no longer running (finished) or the timeout elapses. Returns
  // whether the search finished.
  bool WaitForProjectSearchFinished(std::chrono::milliseconds timeout);
  bool AssertNoAllocationsDuringDraw(std::string* error = nullptr);
  double Measure(std::string_view phase_name, const std::function<void()>& action);
  std::vector<Iteration::PhaseMetrics> TakePhaseMetrics();
  std::uint64_t RandomU64();
  void OpenFileFinder();
  void ActivateGitSidebar();
  void ShowGitSidebar();
  void RefreshGitSidebar();
  bool WaitForGitSidebarIdle(std::chrono::milliseconds timeout);
  bool WaitForGitSidebarEntries(std::size_t min_entries, std::chrono::milliseconds timeout);
  void JumpCompareHunk(int delta);
  void StageCompareHunk();
  void StageCompareSelectedLines();
  void MoveMergeConflict(int delta);
  void ApplyMergeChoice(compare::MergeChoice choice);
  void SimulateExternalFileChange(const std::filesystem::path& path, std::string_view appended_text);
  void StartSearch(std::string_view query);
  void OpenTerminal(std::string_view command);
  void ResizeWindow(int width, int height);

  void SetSetting(std::string_view id, std::string value);
  void ApplyEditorPreferencesToAllTabs();
  editor::TextViewport& ActiveViewport();
  bool SaveActiveTab();
  void RegisterVirtualDocument(const workspace::VirtualDocumentSpec& spec);
  bool OpenVirtualDocument(std::string_view uri);
  bool ExpandSnippetAtCaret(std::string_view snippet_body);

  /// Open an editor tab with outline + snippet placeholder session primed; drain outline debounce.
  /// Used by `idle_soak_30s` (§13.E.3) after snippet/outline ship.
  void PrimeEditorEssentialsIdleSoakSurface();

  workspace::WorkspaceShell& Shell() { return shell_; }

 private:
  workspace::WorkspaceShell& shell_;
  SDL_Window* window_ = nullptr;
  SDL_Renderer* renderer_ = nullptr;
  std::mt19937_64 rng_;
  std::vector<Iteration::PhaseMetrics> phase_metrics_;
};

struct Scenario {
  std::string name;
  bool smoke = false;
  bool baseline_gated = true;
  bool run_by_default = true;
  // Discarded iterations run (on the same reused driver) before the measured
  // ones. For scenarios whose first run does one-time cold work the rest reuse
  // — e.g. a project's initial file-index build — this warms that state so every
  // measured iteration is uniform and the p95/max percentiles aren't governed by
  // a single noisy cold sample. Keep 0 for scenarios that are already uniform.
  std::size_t warmup_iterations = 0;
  // Per-metric regression tolerances (percent), written into this scenario's
  // baseline at --update-baseline time.
  //
  // WALL is gated loosely and ALLOCATIONS tightly, on purpose. Three full gated
  // runs of one unchanged binary on this reference runner produced 14, 2 and 0
  // failures against baselines captured from that same binary, with wall
  // overshoots up to +159% on the p50 -- the machine lands in stable modes tens
  // of percent apart and the smallest scenarios (a 2 ms median) are dominated by
  // it. A 10% wall gate here is not a regression detector, it is a coin flip, and
  // a suite that is red on half its runs detects nothing at all because nobody
  // reads it.
  //
  // Allocation counts, since the counter became per-thread (AllocationCounter.cpp),
  // ARE byte-identical run to run. They scale with the work done, so they catch
  // exactly the regressions that matter most -- an added allocation on a hot path,
  // a cache that stopped hitting, an O(n) walk that became O(n^2) -- and they catch
  // them precisely. That is the oracle; keep it tight.
  //
  // What this policy gives up is a constant-factor wall regression under ~2x. That
  // is already the job of the interleaved current-vs-main tools/perf-compare.py
  // run, where shared machine load cancels because both sides pay it.
  double tolerance_p50_percent = 100.0;
  double tolerance_p95_percent = 150.0;
  double tolerance_max_percent = 200.0;
  // A negative value means "inherit the matching wall tolerance". Do not use it:
  // it exists only so the resolution rule stays expressible, and inheriting the
  // wall envelope would throw away the precise gate above.
  double tolerance_alloc_p50_percent = 10.0;
  double tolerance_alloc_p95_percent = 20.0;
  double tolerance_alloc_max_percent = 50.0;
  std::function<void(ScenarioContext&)> run;
};

// Wall envelopes for the handful of scenarios that need an even wider tail than
// the (already wide) default -- a flat baseline whose p95/max have no natural
// headroom at all, so a single context switch in one of ten iterations trips
// them. Allocation envelopes are NOT widened here: see Scenario above for why
// they are the gate that matters.
namespace tolerance {

inline constexpr double kJitterWallP50 = 100.0;
inline constexpr double kJitterWallP95 = 250.0;
inline constexpr double kJitterWallMax = 400.0;

inline constexpr double kExactAllocP50 = 10.0;
inline constexpr double kExactAllocP95 = 20.0;
inline constexpr double kExactAllocMax = 50.0;

}  // namespace tolerance

class PerfHarness {
 public:
  struct RunOptions {
    std::vector<std::string> scenario_names;
    bool smoke_only = false;
    std::size_t iterations = 10;
    std::optional<std::uint64_t> random_seed;
    std::optional<std::string> layout_mode_override;
    bool keep_artifacts = false;
    // SDL renderer driver to measure through. "software" (default) is the
    // portable, baseline-gated reference path. "auto"/"default" lets SDL pick
    // the platform GPU backend; any other value forces that specific SDL driver.
    // A non-software driver is advisory only (numbers are not cross-machine
    // portable and never update baselines) -- it exists so GPU-only paths like
    // batched glyph text can be measured at all.
    std::string renderer_driver = "software";
    // SDL *video* driver to measure through. "dummy" (default) is the
    // baseline-gated reference lane: no window system, so a present is a no-op
    // and a scenario's wall time is the app's own work. Any real video driver
    // (x11, wayland) adds window-system present cost that lands only on
    // frame-pumping scenarios -- measured on perf-runner-v1 at 2-12x the dummy
    // lane for shell scenarios and 1.0x for pure-unit ones, with byte-identical
    // allocation counts. That asymmetry silently invalidates every wall gate, so
    // a non-dummy video driver is advisory only, exactly like a non-software
    // renderer. "auto"/"default" leaves SDL_VIDEODRIVER alone and lets SDL pick.
    std::string video_driver = "dummy";
  };

  static void RegisterScenario(const Scenario& scenario);
  static std::vector<Scenario> RegisteredScenarios();
  static std::optional<Aggregate> RunScenario(const Scenario& scenario, const RunOptions& options);
  static bool VerifyFixtureTree(const std::filesystem::path& root,
                                const std::filesystem::path& expected_hash_file,
                                std::string* error);
  static std::string LastError();
  // SDL driver name the most recent InitializeDriver actually resolved (e.g.
  // "software", "opengl"). Empty before the first driver init. Backs report
  // metadata so the GPU advisory lane records the real backend it measured.
  static std::string ResolvedRendererDriver();
  // SDL video driver the most recent InitializeDriver actually resolved (e.g.
  // "dummy", "x11"). Read back from SDL rather than from the environment so the
  // report records the lane that was measured, not the one that was requested.
  static std::string ResolvedVideoDriver();

 private:
  struct Driver {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    workspace::WorkspaceShell shell;
    bool initialized = false;
    std::filesystem::path isolated_app_root;
    bool keep_artifacts = false;
  };

  static bool InitializeDriver(Driver* driver,
                               std::optional<std::uint64_t> random_seed,
                               bool keep_artifacts = false,
                               std::string_view renderer_driver = "software",
                               std::string_view video_driver = "dummy");
  static void ShutdownDriver(Driver* driver);
};

class ScenarioRegistration {
 public:
  explicit ScenarioRegistration(const Scenario& scenario);
};

}  // namespace microide::tests::perf

#define MICROIDE_PERF_SCENARIO(NAME_LITERAL, SMOKE, FN)                              \
  namespace {                                                                         \
  const ::microide::tests::perf::ScenarioRegistration                                \
      g_perf_scenario_registration_##FN({                                            \
          .name = NAME_LITERAL,                                                      \
          .smoke = SMOKE,                                                            \
          .baseline_gated = true,                                                    \
          .run_by_default = true,                                                    \
          .run = FN,                                                                 \
      });                                                                             \
  }  // namespace
