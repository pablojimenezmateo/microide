#pragma once

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
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

class ScenarioContext;

// The ONE fixture guard for scenario bodies (TD-2026-08-10-170). Returns true
// when the fixture is there; otherwise marks the scenario skipped — which the
// harness turns into a `SKIP` line and, for a baseline-gated scenario, a run
// failure — and returns false so the caller returns immediately.
//
// Every scenario used to spell this itself as `std::cerr << ... ; return;`,
// which told the harness nothing: the empty iteration was measured and compared,
// and a scenario that did nothing PASSED its gate.
bool RequireFixture(ScenarioContext& context,
                    const std::filesystem::path& fixture,
                    std::string_view scenario_label);

// Linear-interpolated percentile over an unsorted sample. Shared with the
// baseline comparison, which re-percentiles the clock-normalised CPU series
// (Baseline.cpp) and must do it exactly the way the harness percentiled the raw
// one -- two subtly different percentile rules would put the normalised and raw
// numbers on different samples and make the reported factor unreadable.
inline double Percentile(std::vector<double> values, double p) {
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  const double idx = p * static_cast<double>(values.size() - 1);
  const std::size_t lo = static_cast<std::size_t>(std::floor(idx));
  const std::size_t hi = static_cast<std::size_t>(std::ceil(idx));
  if (lo == hi) {
    return values[lo];
  }
  const double weight = idx - static_cast<double>(lo);
  return values[lo] * (1.0 - weight) + values[hi] * weight;
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
  // Nanoseconds the harness's fixed-work calibration probe took just OUTSIDE this
  // iteration's window -- a reading of the machine's effective clock while the
  // iteration ran, not of the iteration. Zero when no probe ran.
  //
  // Carried per iteration and not just per run because the failure that motivated
  // it was a clock that stepped MID-run: idle_soak_30s measured 14 ms of CPU on
  // its first five iterations and 30 ms on its last five, one clean step, with
  // every application counter byte-identical across it. Normalising against a
  // single per-run number would smear that; normalising each iteration against
  // its own reading cancels it exactly (TD-2026-08-05-137).
  std::uint64_t cpu_calibration_ns = 0;
  // Resident-set growth across the iteration, in bytes. Deliberately the delta and
  // not absolute RSS: the harness runs every iteration of every scenario in one
  // process, so absolute RSS is dominated by whatever ran earlier and is not
  // attributable to the scenario. Growth is. Clamped at zero when the iteration
  // gives memory back.
  std::uint64_t rss_growth_bytes = 0;
  // `bytes_allocated - bytes_freed` across the measured window: the bytes this
  // iteration allocated through the counting `operator new` and did NOT hand back
  // before the window closed. Signed, because a scenario whose setup allocates
  // and whose measured window releases is legitimately negative
  // (debug_session_stop_to_variables reads -141,047 every iteration).
  //
  // This is the DETERMINISTIC retention instrument, and it is the reason the
  // resident gate stopped being a coin flip (TD-2026-08-06-150). Measured across
  // three process states whose heaps could not have been more different -- a
  // solo run, a 26-scenario prefix, a 52-scenario prefix -- 52 of 52 scenarios
  // reported the SAME value, worst spread 9 bytes. Over the same three states
  // `rss_growth_bytes` for one of them (diff_stage_selected_lines) read 21 KB,
  // 79 KB and 239 KB per iteration for a byte-identical 28,470 bytes of actual
  // retention: RSS is a 7x-amplifying, prefix-dependent view of this number.
  //
  // It is not a superset of RSS and does not replace it. `operator new` is the
  // only allocator it sees, so malloc from Lua, PCRE2, SDL and libc, and anything
  // mmap'd, are invisible here and visible there. Both are gated, for different
  // things.
  std::int64_t net_heap_bytes = 0;
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
  // Mean resident growth per iteration with the single largest sample dropped.
  // This is the GATED resident statistic; p50/p95/max stay recorded for
  // diagnosis. See CompareToBaseline for why a percentile is the wrong
  // instrument for this metric.
  double mean_rss_growth_bytes = 0.0;
  // Median per-iteration `net_heap_bytes`. The median rather than the mean
  // because iteration 0 pays the scenario's one-time cold retention (8 MB on the
  // diff fixtures against 28 KB steady state) and would own an average; the
  // steady-state value is the one that reproduces to the byte.
  double p50_net_heap_bytes = 0.0;
  // Median of the per-iteration calibration probe. Recorded INTO the
  // baseline, which is what makes a duration gate machine-state-independent: the
  // baseline knows what clock it was captured at, so a later run can be compared
  // against a scaled expectation instead of against a number that silently meant
  // "on a 5.1 GHz core" (TD-2026-08-05-137). Zero on a run with no probe, which
  // disables normalisation rather than scaling by a nonsense factor.
  double p50_cpu_calibration_ns = 0.0;
};

// Process-wide CPU time and resident set, for the harness and for scenarios that
// want to assert on them directly. CPU time comes from getrusage(RUSAGE_SELF),
// which sums every thread; RSS from /proc/self/statm.
double ProcessCpuMilliseconds();
std::uint64_t ProcessResidentBytes();
// Release allocator-held free pages back to the OS. Call immediately before a
// resident reading that is meant to measure retained memory; see the definition
// for why an untrimmed reading is a coin flip (TD-2026-08-05-136).
void SettleResidentSet();

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

// One measured phase, aggregated across a scenario's measured iterations.
//
// A scenario total answers "what did this whole iteration cost", which for most
// scenarios is dominated by setup: search_first_result's 20,192 allocations are
// 20,047 of project-open-and-index-build and 145 of the search the scenario is
// named after, so its tight total gate would not notice the search doubling
// (TD-2026-08-06-153). The phase is the thing the scenario claims to measure, so
// the phase is what has to be gated.
//
// ALLOCATIONS are the gated statistic, for the same reason they are at scenario
// level: they are deterministic run to run and they scale with the work done.
// Wall is recorded for diagnosis only -- a 2 ms phase carries the whole of this
// runner's scheduler jitter.
struct PhaseMetricSet {
  std::string name;
  // Per-iteration allocation count for this phase: the SUM over every Measure()
  // call that used this name in that iteration, then percentiled across
  // iterations. Summing matters because a scenario is free to call Measure with
  // one name several times per iteration, and gating one arbitrary call of a
  // repeated phase would be a gate on which call happened to be last.
  double p50_allocations = 0.0;
  double max_allocations = 0.0;
  double p50_wall_ms = 0.0;
  // Measured iterations that recorded this phase at all. A phase inside a
  // conditional shows up here as a count below the run's iteration count, which
  // is worth seeing before trusting its percentile.
  std::size_t iterations = 0;
};

struct Aggregate {
  std::string scenario_name;
  std::vector<Iteration> iterations;
  MetricSet metrics;
  // Aggregated per-phase metrics, in first-appearance order.
  std::vector<PhaseMetricSet> phases;
  bool smoke = false;
  // Non-empty when the scenario declared it could not run (see
  // ScenarioContext::SkipScenario). Its metrics describe nothing and must not be
  // compared to a baseline, written as one, or reported as a pass.
  std::string skip_reason;
};

// Aggregate the per-iteration phase records into one entry per phase name.
// Header-inline because both binaries need it: microide_perf produces it from a
// run, and microide_tests (which does not link the harness TU) builds Aggregates
// the same way to test the gate.
inline std::vector<PhaseMetricSet> AggregatePhaseMetrics(const std::vector<Iteration>& iterations) {
  // First-appearance order, kept deliberately: a scenario's phases read as a
  // timeline, and sorting them alphabetically would scramble that in every
  // report and every baseline diff.
  std::vector<PhaseMetricSet> phases;
  std::vector<std::vector<double>> allocation_samples;
  std::vector<std::vector<double>> wall_samples;
  // Keyed on the iteration's own strings, which outlive this call.
  std::unordered_map<std::string_view, std::size_t> index_by_name;

  for (const Iteration& iteration : iterations) {
    // Sum repeats WITHIN an iteration before percentiling ACROSS iterations: a
    // scenario may call Measure() with one name several times in a single pass,
    // and each of those calls is part of what that iteration cost.
    std::vector<double> iteration_allocations(phases.size(), 0.0);
    std::vector<double> iteration_wall(phases.size(), 0.0);
    std::vector<bool> present(phases.size(), false);
    for (const Iteration::PhaseMetrics& phase : iteration.phase_metrics) {
      auto [it, inserted] = index_by_name.emplace(std::string_view(phase.name), phases.size());
      if (inserted) {
        phases.push_back(PhaseMetricSet{.name = phase.name});
        allocation_samples.emplace_back();
        wall_samples.emplace_back();
        iteration_allocations.push_back(0.0);
        iteration_wall.push_back(0.0);
        present.push_back(false);
      }
      const std::size_t index = it->second;
      iteration_allocations[index] += static_cast<double>(phase.allocations);
      iteration_wall[index] += phase.wall_ms;
      present[index] = true;
    }
    for (std::size_t index = 0; index < phases.size(); ++index) {
      if (!present[index]) {
        continue;
      }
      allocation_samples[index].push_back(iteration_allocations[index]);
      wall_samples[index].push_back(iteration_wall[index]);
    }
  }

  for (std::size_t index = 0; index < phases.size(); ++index) {
    phases[index].p50_allocations = Percentile(allocation_samples[index], 0.50);
    phases[index].max_allocations =
        allocation_samples[index].empty()
            ? 0.0
            : *std::max_element(allocation_samples[index].begin(),
                                allocation_samples[index].end());
    phases[index].p50_wall_ms = Percentile(wall_samples[index], 0.50);
    phases[index].iterations = allocation_samples[index].size();
  }
  return phases;
}

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
  // When this run happened, ISO-8601 UTC. A --report-json is only a drift record
  // if it says when it was taken; without it a directory of reports is dated by
  // mtime, which any copy or rsync destroys (TD-2026-08-06-141).
  std::string timestamp_utc;
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
  // Harness-owned counters, merged into the iteration's `perf_counters` under a
  // `harness.` prefix. The production counters describe what the *application*
  // did; on a soak scenario almost none of the measured CPU is application work,
  // it is the harness's own idle-poll loop and its 1920x1080 software present.
  // Without these the two are indistinguishable, and an iteration that costs 2x
  // for identical application counters has no visible explanation at all.
  std::vector<std::pair<std::string, std::uint64_t>> TakeHarnessCounters();
  // Nanoseconds a fixed, deterministic slab of integer work took, measured just
  // OUTSIDE this iteration's window. cpu_ms is a duration, so it moves when the machine's effective clock moves —
  // and on a laptop forty minutes into a gate run the boost budget is spent and
  // identical instructions cost measurably more wall and CPU. Without this
  // reading, the report cannot tell "the code got slower" from "the machine got
  // slower"; with it, the gate normalises the difference away.
  void RecordCpuCalibration(std::uint64_t nanoseconds);
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
  // Truncate every file SimulateExternalFileChange appended to back to the size
  // it had before this iteration touched it. Called by the harness after each
  // iteration (measured and warmup alike), outside the measured window, and on
  // the exception path too — a fixture left grown makes every later run of every
  // scenario reading that tree measure something else (TD-2026-08-06-155).
  void RestoreExternalFileChanges();
  void StartSearch(std::string_view query);
  void OpenTerminal(std::string_view command);
  // Push bytes straight into the active terminal's emulator, exactly as the
  // production pty reader thread does. The harness spawns no real shell, so this
  // is the ONLY way a terminal scenario gets content — and unlike waiting on a
  // child process it is byte-for-byte reproducible.
  void FeedTerminalOutput(std::string_view bytes);
  // Close every terminal tab. The driver is shared across a scenario's iterations,
  // so a scenario that opens terminals must close them or each iteration inherits
  // every earlier one.
  void CloseAllTerminals();
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

  // Declare that this scenario cannot run — a fixture it needs is absent, so
  // whatever it measured is not what its baseline describes.
  //
  // Scenarios used to handle this themselves: print to stderr and `return`. The
  // harness never learned, so the iteration was recorded, compared to a real
  // baseline, and reported **PASS** — because a gate only fails on a regression
  // and "did nothing" is not a regression. `editor_moby_dick_workout` passed
  // measuring **7 allocations against a baseline of 138,599**, which is the
  // shape of every vacuous green in `validation-traps.md` (TD-2026-08-10-170).
  void SkipScenario(std::string reason);
  bool skipped() const { return !skip_reason_.empty(); }
  const std::string& skip_reason() const { return skip_reason_; }

 private:
  workspace::WorkspaceShell& shell_;
  std::string skip_reason_;
  SDL_Window* window_ = nullptr;
  SDL_Renderer* renderer_ = nullptr;
  std::mt19937_64 rng_;
  std::vector<Iteration::PhaseMetrics> phase_metrics_;
  // (path, size before this iteration's first append) for every fixture file
  // SimulateExternalFileChange touched. See RestoreExternalFileChanges.
  std::vector<std::pair<std::filesystem::path, std::uintmax_t>> external_file_restores_;

  // Fixed slots rather than a name lookup: the idle-wait loop bumps these up to
  // ~5,600 times per iteration, and a counter that costs a string compare per
  // bump would become a measurable share of the very budget it is there to
  // explain.
  enum class HarnessCounter : std::size_t {
    kFramesPumped,
    kIdleWaitPolls,
    kIdleWaitIdleSleeps,
    kIdleWaitCaretSleeps,
    kIdleWaitShortPolls,
    kIdleWaitHandledWakes,
    kCpuCalibrationNs,
    kCount,
  };
  void BumpHarnessCounter(HarnessCounter counter, std::uint64_t amount = 1) {
    harness_counters_[static_cast<std::size_t>(counter)] += amount;
  }
  std::array<std::uint64_t, static_cast<std::size_t>(HarnessCounter::kCount)> harness_counters_{};
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
  // Resident-growth envelope, written into this scenario's baseline at
  // --update-baseline time.
  //
  // These exist because a widened envelope that lives only in the committed JSON
  // does not survive a rebaseline. editor_long_line_select_all_edit carried a
  // hand-edited 60% for TD-2026-08-05-136 while the writer below had no rss
  // fields at all, so the next `--update-baseline` would have silently reset it to
  // the struct default and re-armed a gate its own TD calls a coin flip -- with
  // nothing in the diff to read except three numbers changing in a generated file.
  // A tolerance that is not expressed in code is a comment.
  double tolerance_rss_percent = 25.0;
  // Net-heap-retention envelope, written into this scenario's baseline at
  // --update-baseline time for the same reason the resident one is: a tolerance
  // that lives only in the committed JSON does not survive a rebaseline.
  //
  // Tight by default because the metric is deterministic (TD-2026-08-06-150).
  double tolerance_net_heap_percent = 10.0;
  // Whether this scenario's iteration CPU time is a valid regression signal.
  //
  // Set false only where it provably is not, and say what replaces it. cpu_ms is
  // a duration, so it scales with the machine's effective clock -- and a scenario
  // that spends most of its iteration asleep lets the governor walk the core down
  // to the 605 MHz floor, 8.5x below its 5157 MHz ceiling. What is left to
  // measure then is a handful of frames rendered at whatever clock the core
  // happened to be at on the way back up. `harness.cpu_calibration_ns` makes that
  // visible: on idle_soak_30s it stepped 671 -> 857 us mid-run and the iteration
  // cpu_ms stepped 14 -> 30 ms with it, application counters byte-identical
  // across the step (TD-2026-08-05-137). No percentage envelope is both stable
  // and meaningful against that. Same reasoning as the p50-only RSS gate in
  // Baseline.cpp: a metric that measures which iteration got unlucky is not a
  // gate. The numbers are still measured and reported, just not enforced.
  bool gate_cpu_metrics = true;
  // Whether this scenario's net heap retention is a valid regression signal.
  //
  // It is for almost everything -- 52 of 52 scenarios re-measured under three
  // different suite prefixes reproduced to the byte, and 98 of 99 held across two
  // full-suite runs on very differently loaded machines (TD-2026-08-06-150). Set
  // false only where the metric provably measures timing instead of code, and say
  // what does.
  //
  // The one counterexample so far is `multi_project_switch`, and it is
  // instructive: `bytes_allocated - bytes_freed` is measured on the shell thread
  // across ONE iteration's window, so a scenario whose window frees structures
  // the PREVIOUS iteration allocated reports a difference that depends on when
  // the teardown lands, not on what the code does. Its series spans −21,284 to
  // +29,061 bytes with `p50_allocations` moving by 14 across the same runs. The
  // allocation count is the oracle there.
  bool gate_net_heap_metrics = true;
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
  // Committed manifest for a fixture tree: `<root>.sha256`. Derived rather than
  // passed so a call site cannot pair a tree with the wrong manifest.
  static std::filesystem::path FixtureManifestPath(const std::filesystem::path& root);
  // Integrity check for a fixture tree against its committed manifest. Hashes in
  // process (util::Sha256); the perf runner needs no interpreter on PATH.
  static bool VerifyFixtureTree(const std::filesystem::path& root, std::string* error);
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
