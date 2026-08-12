#include "perf/PerfHarness.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <numeric>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(__unix__)
#include <sys/resource.h>
#include <unistd.h>
#endif

#if defined(__GLIBC__)
#include <malloc.h>
#endif

#include "perf/AllocationCounter.h"
#include "perf/PerfHarnessIsolation.h"
#include "TerminalSessionTestAccess.h"
#include "WorkspaceShellEventHelpers.h"
#include "render/RendererInfo.h"
#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"
#include "util/Sha256.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"

namespace microide::tests::perf {
namespace {

std::vector<Scenario>& ScenarioRegistry() {
  static std::vector<Scenario> registry;
  return registry;
}

std::string& HarnessError() {
  static std::string error;
  return error;
}

// SDL driver name the last InitializeDriver actually got, for report metadata.
std::string& ResolvedRendererDriverStorage() {
  static std::string driver;
  return driver;
}

// SDL video driver the last InitializeDriver actually got, for report metadata.
std::string& ResolvedVideoDriverStorage() {
  static std::string driver;
  return driver;
}

double MaxOr0(const std::vector<double>& values) {
  return values.empty() ? 0.0 : *std::max_element(values.begin(), values.end());
}

// Mean with the single largest sample dropped.
//
// The largest sample is almost always iteration 0's cold pass -- arena growth,
// first-touch page faults, the caches every later iteration reuses -- which is
// several times every other sample and would own a plain mean. Dropping exactly
// one keeps the statistic a mean (it still sees every other iteration, including
// a scenario that retains on only some of them) rather than turning it into
// another percentile.
double TrimmedMeanOr0(std::vector<double> values) {
  if (values.empty()) {
    return 0.0;
  }
  if (values.size() < 3) {
    return std::accumulate(values.begin(), values.end(), 0.0) /
           static_cast<double>(values.size());
  }
  const auto largest = std::max_element(values.begin(), values.end());
  values.erase(largest);
  return std::accumulate(values.begin(), values.end(), 0.0) /
         static_cast<double>(values.size());
}

MetricSet AggregateMetrics(const std::vector<Iteration>& iterations) {
  std::vector<double> wall_ms;
  std::vector<double> allocations;
  std::vector<double> cpu_ms;
  std::vector<double> rss_growth_bytes;
  std::vector<double> net_heap_bytes;
  std::vector<double> calibration_ns;
  wall_ms.reserve(iterations.size());
  allocations.reserve(iterations.size());
  cpu_ms.reserve(iterations.size());
  rss_growth_bytes.reserve(iterations.size());
  net_heap_bytes.reserve(iterations.size());
  calibration_ns.reserve(iterations.size());
  for (const Iteration& iteration : iterations) {
    wall_ms.push_back(iteration.metrics.wall_ms);
    allocations.push_back(static_cast<double>(iteration.metrics.allocations));
    cpu_ms.push_back(iteration.metrics.cpu_ms);
    rss_growth_bytes.push_back(static_cast<double>(iteration.metrics.rss_growth_bytes));
    net_heap_bytes.push_back(static_cast<double>(iteration.metrics.net_heap_bytes));
    if (iteration.metrics.cpu_calibration_ns != 0) {
      calibration_ns.push_back(static_cast<double>(iteration.metrics.cpu_calibration_ns));
    }
  }
  MetricSet out;
  out.p50_wall_ms = Percentile(wall_ms, 0.50);
  out.p95_wall_ms = Percentile(wall_ms, 0.95);
  out.max_wall_ms = MaxOr0(wall_ms);
  out.p50_allocations = Percentile(allocations, 0.50);
  out.p95_allocations = Percentile(allocations, 0.95);
  out.max_allocations = MaxOr0(allocations);
  out.p50_cpu_ms = Percentile(cpu_ms, 0.50);
  out.p95_cpu_ms = Percentile(cpu_ms, 0.95);
  out.max_cpu_ms = MaxOr0(cpu_ms);
  out.p50_rss_growth_bytes = Percentile(rss_growth_bytes, 0.50);
  out.p95_rss_growth_bytes = Percentile(rss_growth_bytes, 0.95);
  out.max_rss_growth_bytes = MaxOr0(rss_growth_bytes);
  out.mean_rss_growth_bytes = TrimmedMeanOr0(rss_growth_bytes);
  out.p50_net_heap_bytes = Percentile(net_heap_bytes, 0.50);
  out.p50_cpu_calibration_ns = Percentile(calibration_ns, 0.50);
  return out;
}

bool ShouldRunScenario(const Scenario& scenario, const PerfHarness::RunOptions& options) {
  if (options.smoke_only && !scenario.smoke) {
    return false;
  }
  if (options.scenario_names.empty()) {
    return scenario.run_by_default;
  }
  return std::find(options.scenario_names.begin(), options.scenario_names.end(), scenario.name) !=
         options.scenario_names.end();
}

std::uint64_t ResolveSeed(std::optional<std::uint64_t> explicit_seed) {
  if (explicit_seed.has_value()) {
    return *explicit_seed;
  }
  if (const char* env = SDL_getenv("MICROIDE_PERF_SEED")) {
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(env, &end, 10);
    if (end != env) {
      return static_cast<std::uint64_t>(parsed);
    }
  }
  return 1337ULL;
}

// Time a fixed, deterministic slab of integer work. Everything the harness records
// as a cost -- wall_ms, cpu_ms -- is a duration, so all of it scales with the
// machine's effective clock. This gives every iteration a reading of what that
// clock actually was, so a report can separate a slower binary from a slower
// machine and the gate can normalise the difference away (TD-2026-08-05-137).
//
// Deliberately branch-free, allocation-free, memory-free and dependent (each step
// needs the previous result), and sunk into a volatile so it cannot be elided.
// Every one of those properties is load-bearing, because this number scales a
// gate: a probe that touches memory is a probe the code under test can move.
//
// Measured on perf-runner-v1 (Ryzen AI 9 HX 370, 8 fast threads at 5157 MHz),
// while closing item 3 of the TD -- the probe "understates the effect", so try a
// shape closer to the work:
//
//  - A MEMORY-MIXED probe (this chain + an L2 pointer chase + a 256 KiB streaming
//    copy) reads DIRTIER, not truer. Its memory half swung 3.3x across one
//    scenario's iterations (632 -> 192 us) while the chain held to 1%, because the
//    probe's buffers are evicted by whatever the scenario just did. That makes the
//    reading a function of the code under test: a change that grows the working set
//    inflates the probe, which scales the expectation up, which loosens the very
//    gate that change should have tripped. It also streams ~12 MB immediately
//    before the measured window, evicting the app's own working set. Rejected on
//    both counts.
//  - A BURST-SHAPED probe (the same work in short slabs, each after a 1 ms sleep,
//    to time a core that has just been idle) read within 1% of this one on every
//    iteration of every scenario tried. A 1 ms gap is nowhere near long enough for
//    the core to leave the state it is in.
//
// What the machine actually does here, measured: a thread that works CONTINUOUSLY
// for about a second reaches a state where this probe reads ~467 us instead of
// ~673 us -- 1.44x -- and holds it for as long as it keeps working. 300 ms of idle
// drops it back, and no amount of spinning afterwards recovers it inside a run with
// idle in it; a busy keeper thread pinned to another physical core does not hold it
// either, so it is per-thread residency and not a package state the harness can
// pin. Scenarios that pump frames and wait therefore live in the slow state, and
// the few that grind continuously (syntax_highlight_cpp_lines) cross into the fast
// one PART WAY THROUGH A RUN: it measured 252 us for nine iterations then 179 us
// for five, with cpu_ms tracking it 15.6 -> 11.3 ms (1.38x against the probe's
// 1.41x). That is the case per-iteration normalisation exists for.
std::uint64_t MeasureCpuCalibrationNanoseconds() {
  static volatile std::uint64_t sink = 0;
  const auto start = std::chrono::steady_clock::now();
  std::uint64_t accumulator = 1;
  for (std::uint64_t i = 0; i < 400000; ++i) {
    accumulator = accumulator * 6364136223846793005ULL + 1442695040888963407ULL;
    accumulator ^= accumulator >> 33;
  }
  const auto end = std::chrono::steady_clock::now();
  sink = accumulator;
  // Read the sink back so the store is observably live: written-and-never-read
  // is a warning, and -Werror is on in the clang lane.
  return static_cast<std::uint64_t>(
             std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()) |
         (sink & 0U);
}

}  // namespace

double ProcessCpuMilliseconds() {
#if defined(__unix__)
  // RUSAGE_SELF sums every thread in the process, which is the point: a scenario
  // that offloads work to the background executor still pays for it here even
  // though its wall time does not move.
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0) {
    return 0.0;
  }
  const auto to_ms = [](const timeval& tv) {
    return static_cast<double>(tv.tv_sec) * 1000.0 + static_cast<double>(tv.tv_usec) / 1000.0;
  };
  return to_ms(usage.ru_utime) + to_ms(usage.ru_stime);
#else
  return 0.0;
#endif
}

// Hand every page the allocator is merely *holding* back to the OS, so a resident
// reading either side of an iteration measures what the process RETAINS rather
// than what its arenas happen to be caching.
//
// Without this, `rss_growth_bytes` on editor_long_line_select_all_edit was bimodal
// — 4.19 MB or 6.29 MB per iteration, stably, with byte-identical allocation
// counts on both sides (TD-2026-08-05-136). Both values are multiples of the
// fixture's ~2 MiB line: the variance is whether a megabyte-scale block was still
// sitting on a free list at the sample point, which is a fact about glibc's arena
// bookkeeping and not about the code under test. A gate whose value is decided
// there is a coin flip.
//
// Runs OUTSIDE the measured window at both boundaries, so it costs the scenario
// neither wall nor CPU; it does mean the next iteration re-faults the pages it
// gets back, which is a real cost the measurement now carries uniformly instead of
// on whichever iteration got unlucky.
void SettleResidentSet() {
#if defined(__GLIBC__)
  malloc_trim(0);
#endif
}

std::uint64_t ProcessResidentBytes() {
#if defined(__unix__)
  // statm's second field is resident pages. ru_maxrss would be cheaper but it is
  // a high-water mark that never comes down, so it cannot express the growth of
  // one iteration inside a long-lived process.
  std::ifstream statm("/proc/self/statm");
  if (!statm) {
    return 0;
  }
  std::uint64_t total_pages = 0;
  std::uint64_t resident_pages = 0;
  if (!(statm >> total_pages >> resident_pages)) {
    return 0;
  }
  const long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0) {
    return 0;
  }
  return resident_pages * static_cast<std::uint64_t>(page_size);
#else
  return 0;
#endif
}

ScenarioContext::ScenarioContext(workspace::WorkspaceShell& shell,
                                 SDL_Window* window,
                                 SDL_Renderer* renderer)
    : shell_(shell), window_(window), renderer_(renderer), rng_(ResolveSeed(std::nullopt)) {}

std::vector<std::pair<std::string, std::uint64_t>> ScenarioContext::TakeHarnessCounters() {
  static constexpr std::array<std::string_view,
                              static_cast<std::size_t>(HarnessCounter::kCount)>
      kNames = {
          "harness.frames_pumped",         "harness.idle_wait_polls",
          "harness.idle_wait_idle_sleeps", "harness.idle_wait_caret_sleeps",
          "harness.idle_wait_short_polls", "harness.idle_wait_handled_wakes",
          "harness.cpu_calibration_ns",
      };
  std::vector<std::pair<std::string, std::uint64_t>> out;
  for (std::size_t i = 0; i < harness_counters_.size(); ++i) {
    if (harness_counters_[i] != 0) {
      out.emplace_back(std::string(kNames[i]), harness_counters_[i]);
    }
  }
  harness_counters_.fill(0);
  return out;
}

bool RequireFixture(ScenarioContext& context,
                    const std::filesystem::path& fixture,
                    std::string_view scenario_label) {
  if (PathExistsNoThrow(fixture)) {
    return true;
  }
  std::string reason(scenario_label);
  reason += ": missing fixture ";
  reason += fixture.string();
  // Deliberately names the ctest setup rather than one generator: there are
  // three (editor-essentials, file-finder, kernel) and naming the wrong one sent
  // a reader to a script that does not produce the tree they are missing.
  reason += " (generate every perf fixture: ctest --test-dir build -R microide_perf_fixtures)";
  context.SkipScenario(std::move(reason));
  return false;
}

void ScenarioContext::SkipScenario(std::string reason) {
  if (skip_reason_.empty()) {
    skip_reason_ = std::move(reason);
  }
}

void ScenarioContext::RecordCpuCalibration(std::uint64_t nanoseconds) {
  BumpHarnessCounter(HarnessCounter::kCpuCalibrationNs, nanoseconds);
}

void ScenarioContext::PumpFrames(std::size_t count) {
  BumpHarnessCounter(HarnessCounter::kFramesPumped, count);
  for (std::size_t i = 0; i < count; ++i) {
    {
      // The application's whole frame. Named here because the app's own top-level
      // scopes (PrepareFrameOnce, WorkspaceRootView::Render) are siblings, so
      // without this there is nothing in a trace summary to add them up against.
      util::PerformanceTrace::Scope scope("PerfHarness::PumpFrames::ShellRender");
      shell_.Render(renderer_, 1920, 1080);
    }
    {
      // The single largest cost in most frame-pumping scenarios, and it belongs to
      // the harness, not the editor: presenting a 1920x1080 software surface is
      // ~8 MB of pixel work per frame. On editor_typing_minified_line it is ~85%
      // of the measured wall, which is why that scenario's wall number barely
      // moves for large changes to the edit path and its allocation count is the
      // oracle. Untraced, that time read as "unexplained", which invites reading a
      // 5% app-side win as noise (TD-2026-08-05-133).
      util::PerformanceTrace::Scope scope("PerfHarness::PumpFrames::Present");
      SDL_RenderPresent(renderer_);
    }
  }
}

void ScenarioContext::PumpEvents() {
  SDL_Event event{};
  while (SDL_PollEvent(&event)) {
    (void)shell_.HandleEvent(event);
  }
}

bool ScenarioContext::Open(const std::filesystem::path& project_root) {
  std::error_code error;
  const std::filesystem::path resolved =
      std::filesystem::absolute(project_root, error).lexically_normal();
  if (error || resolved.empty()) {
    return workspace::WorkspaceShell::TestAccess::OpenProjectTab(shell_, project_root, false, false);
  }
  return workspace::WorkspaceShell::TestAccess::OpenProjectTab(shell_, resolved, false, false);
}

void ScenarioContext::OpenTab(const std::filesystem::path& path) {
  std::error_code error;
  const std::filesystem::path resolved = std::filesystem::absolute(path, error).lexically_normal();
  if (error || resolved.empty()) {
    workspace::WorkspaceShell::TestAccess::OpenFile(shell_, path);
    return;
  }
  workspace::WorkspaceShell::TestAccess::OpenFile(shell_, resolved);
}

void ScenarioContext::CloseActiveTab() {
  if (workspace::WorkspaceShell::TestAccess::FocusedGroupOpenTabCount(shell_) == 0) {
    return;
  }
  workspace::WorkspaceShell::TestAccess::CloseTab(
      shell_, workspace::WorkspaceShell::TestAccess::ActiveTabIndex(shell_));
}

void ScenarioContext::Type(std::string_view text) {
  SDL_Event event{};
  event.type = SDL_EVENT_TEXT_INPUT;
  std::string storage(text);
  event.text.text = storage.c_str();
  (void)shell_.HandleEvent(event);
}

void ScenarioContext::Scroll(int vertical_ticks) {
  (void)SendMouseWheel(shell_, 960.0f, 540.0f, vertical_ticks, 0);
}

void ScenarioContext::KeyDown(SDL_Keycode key, SDL_Keymod modifiers) {
  (void)SendKeyDown(shell_, key, modifiers);
}

bool ScenarioContext::ExecuteCommand(std::string_view command_line) {
  return workspace::WorkspaceShell::TestAccess::ExecuteCommandLine(shell_, std::string(command_line));
}

void ScenarioContext::ToggleProjectSearchPatternMode() {
  workspace::WorkspaceShell::TestAccess::ToggleProjectSearchPatternMode(shell_);
}

void ScenarioContext::ConsumeProjectSearchUpdates() {
  workspace::WorkspaceShell::TestAccess::ConsumeProjectSearchUpdates(shell_);
}

std::uint64_t ScenarioContext::Wait(std::chrono::milliseconds duration) {
  const auto end = std::chrono::steady_clock::now() + duration;
  std::uint64_t wake_count = 0;
  constexpr auto kIdlePollInterval = std::chrono::milliseconds(4);
  while (std::chrono::steady_clock::now() < end) {
    const auto now = std::chrono::steady_clock::now();
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(end - now);
    const auto idle_state = shell_.CurrentIdleWaitState();
    BumpHarnessCounter(HarnessCounter::kIdleWaitPolls);
    if (idle_state.hint == workspace::WorkspaceShell::IdleHint::Idle) {
      BumpHarnessCounter(HarnessCounter::kIdleWaitIdleSleeps);
      std::this_thread::sleep_for(std::min(remaining, std::chrono::milliseconds(20)));
      continue;
    }
    if (idle_state.hint == workspace::WorkspaceShell::IdleHint::CaretOnly &&
        idle_state.caret_remaining_ms > 1) {
      BumpHarnessCounter(HarnessCounter::kIdleWaitCaretSleeps);
      const auto caret_delay = std::chrono::milliseconds(idle_state.caret_remaining_ms);
      std::this_thread::sleep_for(std::min(remaining, caret_delay));
      continue;
    }

    const auto wake = shell_.HandleScheduledWake();
    if (wake.handled) {
      BumpHarnessCounter(HarnessCounter::kIdleWaitHandledWakes);
      ++wake_count;
      PumpFrames(1);
      continue;
    }
    BumpHarnessCounter(HarnessCounter::kIdleWaitShortPolls);
    std::this_thread::sleep_for(kIdlePollInterval);
  }
  return wake_count;
}

bool ScenarioContext::WaitForDiagnostics(const std::filesystem::path& path,
                                         std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() <= deadline) {
    if (workspace::WorkspaceShell::TestAccess::DiagnosticsForPath(shell_, path) != nullptr) {
      return true;
    }
    const auto wake = shell_.HandleScheduledWake();
    if (wake.handled) {
      PumpFrames(1);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return workspace::WorkspaceShell::TestAccess::DiagnosticsForPath(shell_, path) != nullptr;
}

void ScenarioContext::PublishDiagnostics(std::string_view owner,
                                         const std::filesystem::path& path,
                                         std::vector<editor::Diagnostic> diagnostics) {
  workspace::WorkspaceShell::TestAccess::PublishDiagnostics(shell_, owner, path,
                                                            std::move(diagnostics));
}

bool ScenarioContext::HasDiagnostics(const std::filesystem::path& path) const {
  const auto* published = workspace::WorkspaceShell::TestAccess::DiagnosticsForPath(shell_, path);
  return published != nullptr && !published->empty();
}

bool ScenarioContext::WaitForFileIndexPath(const std::filesystem::path& relative_path,
                                           std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() <= deadline) {
    if (workspace::WorkspaceShell::TestAccess::FileIndexContainsPath(shell_, relative_path)) {
      return true;
    }
    const auto wake = shell_.HandleScheduledWake();
    if (wake.handled) {
      PumpFrames(1);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return workspace::WorkspaceShell::TestAccess::FileIndexContainsPath(shell_, relative_path);
}

bool ScenarioContext::WaitForProjectSearchFinished(std::chrono::milliseconds timeout) {
  // Wait for the background worker to genuinely finish via the non-consuming
  // WorkerFinished signal, THEN drain exactly once. Two determinism reasons for
  // this shape: (1) consuming updates mid-search applies a timing-dependent number
  // of partial-result batches, each allocating — draining once after completion
  // applies the single deterministic final state instead; (2) we do not pump the
  // shell while spinning — the search runs on its own thread and flips the flag
  // independently, so pumping would only add unrelated, timing-dependent
  // main-thread work to the measured window.
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() <= deadline) {
    if (workspace::WorkspaceShell::TestAccess::ProjectSearchWorkerFinished(shell_)) {
      workspace::WorkspaceShell::TestAccess::ConsumeProjectSearchUpdates(shell_);
      return !workspace::WorkspaceShell::TestAccess::ProjectSearchRunning(shell_);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  workspace::WorkspaceShell::TestAccess::ConsumeProjectSearchUpdates(shell_);
  return !workspace::WorkspaceShell::TestAccess::ProjectSearchRunning(shell_);
}

bool ScenarioContext::AssertNoAllocationsDuringDraw(std::string* error) {
  const AllocationSnapshot before = Allocations::Snapshot();
  shell_.Render(renderer_, 1920, 1080);
  const AllocationDelta delta = Allocations::DeltaSince(before);
  const bool ok = delta.allocations == 0 && delta.bytes_allocated == 0;
  if (!ok && error != nullptr) {
    *error = "draw path allocated memory";
  }
  return ok;
}

double ScenarioContext::Measure(std::string_view phase_name, const std::function<void()>& action) {
  // Scope the allocation-site trace to this phase when the run asked for it. The
  // name test happens once per phase, not per allocation, and is a no-op when
  // MICROIDE_PERF_ALLOC_TRACE_PHASE is unset.
  const std::string_view trace_filter = Allocations::PhaseTraceFilter();
  const bool trace_this_phase =
      !trace_filter.empty() && phase_name.find(trace_filter) != std::string_view::npos;
  if (trace_this_phase) {
    Allocations::SetPhaseTraceActive(true);
  }
  const AllocationSnapshot before = Allocations::Snapshot();
  const auto start = std::chrono::steady_clock::now();
  action();
  const auto end = std::chrono::steady_clock::now();
  const AllocationDelta delta = Allocations::DeltaSince(before);
  if (trace_this_phase) {
    Allocations::SetPhaseTraceActive(false);
  }
  const double elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count();
  phase_metrics_.push_back(Iteration::PhaseMetrics{
      .name = std::string(phase_name),
      .wall_ms = elapsed_ms,
      .allocations = static_cast<std::uint64_t>(std::max<std::int64_t>(0, delta.allocations)),
      .frees = static_cast<std::uint64_t>(std::max<std::int64_t>(0, delta.frees)),
      .bytes_allocated =
          static_cast<std::uint64_t>(std::max<std::int64_t>(0, delta.bytes_allocated)),
      .bytes_freed = static_cast<std::uint64_t>(std::max<std::int64_t>(0, delta.bytes_freed)),
  });
  return elapsed_ms;
}

std::vector<Iteration::PhaseMetrics> ScenarioContext::TakePhaseMetrics() {
  return std::move(phase_metrics_);
}

std::uint64_t ScenarioContext::RandomU64() {
  return rng_();
}

void ScenarioContext::OpenFileFinder() {
  ExecuteCommand("files");
}

void ScenarioContext::ActivateGitSidebar() {
  ExecuteCommand("sidebar-show git");
}

void ScenarioContext::ShowGitSidebar() {
  workspace::WorkspaceShell::TestAccess::ShowGitSidebar(shell_);
}

void ScenarioContext::RefreshGitSidebar() {
  workspace::WorkspaceShell::TestAccess::RefreshGitSidebar(shell_);
}

bool ScenarioContext::WaitForGitSidebarIdle(std::chrono::milliseconds timeout) {
  // Prefer WorkspaceShell::TestAccess::PerfRunGitSidebarRefreshSync for a scenario
  // that needs a settled sidebar: showing the git sidebar dispatches an async
  // status whose completion is subject to supersession by the throttled automatic
  // refresh, so polling for the flag to clear can outlast any timeout you pick.
  // This helper is for waiting on a refresh you know was dispatched once.
  //
  // Deliberately NOT delegating to WaitForGitSidebarEntries(0, ...): that call
  // asks for "at least zero entries", which is true on the first check, so this
  // returned after one pump and reported whatever the refreshing flag happened to
  // say at that instant. It waited for nothing. A scenario reaching for a
  // wait-for-idle helper wants the async refresh to have LANDED, and getting a
  // one-shot poll instead is how git_sidebar_activate's allocation count wandered
  // 498-626 across runs with byte-identical git counters.
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    PumpEvents();
    workspace::WorkspaceShell::TestAccess::ConsumeGitSidebarRefresh(shell_);
    if (!workspace::WorkspaceShell::TestAccess::GitSidebarRefreshing(shell_)) {
      return true;
    }
    // A frame every poll, not only on a handled wake: the background status result
    // is published through the main-thread mailbox that a frame drains, so a loop
    // that only pumps events can spin until its deadline with the answer already
    // sitting in the queue.
    (void)shell_.HandleScheduledWake();
    PumpFrames(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  PumpEvents();
  workspace::WorkspaceShell::TestAccess::ConsumeGitSidebarRefresh(shell_);
  return !workspace::WorkspaceShell::TestAccess::GitSidebarRefreshing(shell_);
}

bool ScenarioContext::WaitForGitSidebarEntries(std::size_t min_entries,
                                               std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    PumpEvents();
    workspace::WorkspaceShell::TestAccess::ConsumeGitSidebarRefresh(shell_);
    if (workspace::WorkspaceShell::TestAccess::GitSidebarEntries(shell_).size() >= min_entries) {
      return true;
    }
    const auto wake = shell_.HandleScheduledWake();
    if (wake.handled) {
      PumpFrames(1);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  PumpEvents();
  workspace::WorkspaceShell::TestAccess::ConsumeGitSidebarRefresh(shell_);
  return workspace::WorkspaceShell::TestAccess::GitSidebarEntries(shell_).size() >= min_entries;
}

void ScenarioContext::JumpCompareHunk(int delta) {
  const SDL_Keycode key = delta < 0 ? SDLK_LEFTBRACKET : SDLK_RIGHTBRACKET;
  KeyDown(key, SDL_KMOD_NONE);
  PumpFrames(1);
}

void ScenarioContext::StageCompareHunk() {
  workspace::WorkspaceShell::TestAccess::StageCompareHunk(shell_);
  PumpFrames(4);
}

void ScenarioContext::StageCompareSelectedLines() {
  workspace::WorkspaceShell::TestAccess::StageCompareSelectedLines(shell_);
  PumpFrames(4);
}

void ScenarioContext::MoveMergeConflict(int delta) {
  const SDL_Keycode key = delta < 0 ? SDLK_LEFTBRACKET : SDLK_RIGHTBRACKET;
  KeyDown(key, SDL_KMOD_ALT);
  PumpFrames(1);
}

void ScenarioContext::ApplyMergeChoice(compare::MergeChoice choice) {
  workspace::WorkspaceShell::TestAccess::ApplyMergeChoice(shell_, choice);
  PumpFrames(1);
}

void ScenarioContext::SimulateExternalFileChange(const std::filesystem::path& path,
                                                 std::string_view appended_text) {
  std::error_code error;
  std::filesystem::path resolved = path;
  if (!path.is_absolute()) {
    resolved = workspace::WorkspaceShell::TestAccess::ProjectRoot(shell_) / path;
  }
  resolved = std::filesystem::absolute(resolved, error).lexically_normal();
  // Record what to truncate back to, ONCE per path, before the first append.
  //
  // This append lands in a fixture tree that lives in the repository and is
  // shared by every scenario reading it, and nothing ever undid it: the two
  // external-change scenarios had appended 1,361 and 1,337 times on this
  // checkout, growing `git_large_diff_project/src/large.cpp`'s worktree diff
  // from the 3 lines its generator writes to 2,725. Every diff and merge
  // scenario on those fixtures was measuring a diff whose size is a function of
  // how many times the suite had ever been run here, and their committed
  // baselines had been ratcheting up with it (TD-2026-08-06-155).
  const std::uintmax_t original_size = std::filesystem::file_size(resolved, error);
  if (!error) {
    bool already_recorded = false;
    for (const auto& [recorded_path, recorded_size] : external_file_restores_) {
      if (recorded_path == resolved) {
        already_recorded = true;
        break;
      }
    }
    if (!already_recorded) {
      external_file_restores_.emplace_back(resolved, original_size);
    }
  }
  std::ofstream output(resolved, std::ios::binary | std::ios::app);
  if (!output) {
    throw std::runtime_error("SimulateExternalFileChange: failed to open " + resolved.string());
  }
  output << appended_text;
  if (!output.good()) {
    throw std::runtime_error("SimulateExternalFileChange: failed to write " + resolved.string());
  }
  (void)workspace::WorkspaceShell::TestAccess::ReloadProjectIfFilesChanged(shell_, true);
  PumpFrames(2);
}

void ScenarioContext::RestoreExternalFileChanges() {
  for (const auto& [path, size] : external_file_restores_) {
    std::error_code error;
    std::filesystem::resize_file(path, size, error);
    if (error) {
      // Loud, not silent: a fixture left grown is a measurement that drifts on
      // every later run, which is exactly the failure this exists to end.
      std::cerr << "[perf] WARNING: could not restore fixture file " << path << " to " << size
                << " bytes: " << error.message() << '\n';
    }
  }
  external_file_restores_.clear();
}

void ScenarioContext::StartSearch(std::string_view query) {
  ExecuteCommand("project-search " + std::string(query));
}

void ScenarioContext::OpenTerminal(std::string_view command) {
  std::string command_line = "term";
  if (!command.empty()) {
    command_line.append(" ");
    command_line.append(command);
  }
  (void)ExecuteCommand(command_line);
}

void ScenarioContext::FeedTerminalOutput(std::string_view bytes) {
  // The harness runs with placeholder terminals (no shell is spawned), so a
  // scenario cannot get output by launching a command and sleeping — that
  // measured an EMPTY terminal for as long as the sleep lasted. Feeding the
  // emulator directly is what the production reader thread does with the bytes it
  // reads off the pty, and it is byte-for-byte reproducible.
  if (workspace::WorkspaceShell::TestAccess::TerminalTabCount(shell_) == 0) {
    throw std::runtime_error("FeedTerminalOutput: no terminal is open");
  }
  TerminalSessionTestAccess::AppendOutput(
      workspace::WorkspaceShell::TestAccess::ActiveTerminalSession(shell_), bytes);
  PumpEvents();
}

void ScenarioContext::CloseAllTerminals() {
  // The driver — and therefore the shell — is reused across every iteration of a
  // scenario, so a terminal an iteration opens is still attached on the next one.
  // Twenty accumulated shells draining output inside the next iteration's window
  // is a per-iteration allocation ramp that reads exactly like a leak in whatever
  // the scenario is actually measuring.
  while (workspace::WorkspaceShell::TestAccess::TerminalTabCount(shell_) > 0) {
    workspace::WorkspaceShell::TestAccess::CloseTerminalTab(shell_, 0);
  }
  PumpFrames(1);
}

void ScenarioContext::ResizeWindow(int width, int height) {
  if (window_ != nullptr) {
    SDL_SetWindowSize(window_, width, height);
  }
  (void)SendWindowResized(shell_, width, height);
}

void ScenarioContext::SetSetting(std::string_view id, std::string value) {
  (void)workspace::WorkspaceShell::TestAccess::SetSettingValue(shell_, id, std::move(value));
}

editor::TextViewport& ScenarioContext::ActiveViewport() {
  return workspace::WorkspaceShell::TestAccess::ActiveEditor(shell_);
}

bool ScenarioContext::SaveActiveTab() {
  const std::size_t index =
      workspace::WorkspaceShell::TestAccess::ActiveTabIndex(shell_);
  return workspace::WorkspaceShell::TestAccess::SaveTab(shell_, index);
}

void ScenarioContext::RegisterVirtualDocument(const workspace::VirtualDocumentSpec& spec) {
  workspace::WorkspaceShell::TestAccess::RegisterVirtualDocument(shell_, spec);
}

bool ScenarioContext::OpenVirtualDocument(std::string_view uri) {
  return workspace::WorkspaceShell::TestAccess::OpenVirtualDocument(shell_, uri);
}

bool ScenarioContext::ExpandSnippetAtCaret(std::string_view snippet_body) {
  return workspace::WorkspaceShell::TestAccess::PerfExpandSnippetAtCaret(shell_, snippet_body);
}

void ScenarioContext::PrimeEditorEssentialsIdleSoakSurface() {
  (void)Open("tests/perf/fixtures/large_project");
  OpenTab("tests/perf/fixtures/large_project/pkg0/file_1.txt");
  PumpFrames(2);
  (void)ExecuteCommand("sidebar-show outline");
  Type("_");
  PumpFrames(8);
  workspace::WorkspaceShell::TestAccess::PerfHarnessPrimeSnippetPlaceholderSession(shell_);
  PumpFrames(4);
  // Outline debounce is 150 ms; absorb completion-before-soak without counting toward the 27 s gate.
  (void)Wait(std::chrono::milliseconds(400));
}

void PerfHarness::RegisterScenario(const Scenario& scenario) {
  ScenarioRegistry().push_back(scenario);
}

std::vector<Scenario> PerfHarness::RegisteredScenarios() {
  return ScenarioRegistry();
}

std::optional<Aggregate> PerfHarness::RunScenario(const Scenario& scenario,
                                                  const RunOptions& options) {
  if (!ShouldRunScenario(scenario, options)) {
    return std::nullopt;
  }

  // BEFORE `Driver`, and that ordering is the whole point (TD-2026-08-07-165).
  // `Driver` holds a `WorkspaceShell` by value, so declaring it RUNS the shell's
  // constructor — which resolves the user-level state root and loads `recents`
  // from it. Establishing the isolated app root inside InitializeDriver was
  // therefore one statement too late: every scenario's shell read the developer's
  // real ~/.local/state/microide, and `repo_open_rss_idle` measured 369
  // allocations on a machine that had never run microide and 627 on one where the
  // recents list had grown. A gate whose value depends on the operator's home
  // directory is not a gate.
  std::string isolate_error;
  const std::filesystem::path isolated_app_root =
      EstablishIsolatedAppRoot(options.keep_artifacts, &isolate_error);
  if (isolated_app_root.empty()) {
    HarnessError() = isolate_error.empty() ? "failed to establish isolated app-root"
                                           : isolate_error;
    return std::nullopt;
  }

  Driver driver;
  driver.keep_artifacts = options.keep_artifacts;
  driver.isolated_app_root = isolated_app_root;
  if (!InitializeDriver(&driver, options.random_seed, options.keep_artifacts,
                        options.renderer_driver, options.video_driver)) {
    return std::nullopt;
  }

  // Process warm-up, before any measured iteration and independent of the
  // scenario (TD-2026-08-10-168). The FIRST frames a process ever paints do
  // several thousand allocations no later frame repeats — lazy first-paint state
  // that is global to the process, not to the scenario. While the whole suite
  // shared one process that cost landed on whichever scenario ran first and
  // every other scenario inherited it warm; per-scenario isolation
  // (TD-2026-08-06-152) gave each scenario its own process, so each one pays it
  // again, in its own iteration 1, where it governs p95 and max:
  //
  //     cold_startup_no_project   p50 101, max 9,364   (committed max: 681)
  //     typing_small_file         p50 261, max 12,382  (committed max: 3,703)
  //
  // A constant ~8.6k spike in exactly one iteration, across a third of the
  // suite. This is deliberately NOT `warmup_iterations` — that runs the whole
  // scenario, which would also warm what a scenario means to measure cold (a
  // project open, a cold finder index). Pumping frames on the bare driver warms
  // only the process, which is exactly what the shared-process regime handed
  // every scenario but the first.
  {
    ScenarioContext warm_process(driver.shell, driver.window, driver.renderer);
    warm_process.PumpFrames(3);
  }

  Aggregate aggregate;
  aggregate.scenario_name = scenario.name;
  aggregate.smoke = scenario.smoke;
  aggregate.measurement_revision = scenario.measurement_revision;
  aggregate.iterations.reserve(options.iterations);

  // Warmup: run (and discard) the scenario a few times on the same reused driver
  // so any one-time cold work the measured iterations reuse — e.g. a project's
  // initial file-index build — is already warm. Without this the first measured
  // iteration alone carries that cost and governs the p95/max percentiles,
  // making them noisy even when the steady-state p50 is stable.
  for (std::size_t w = 0; w < scenario.warmup_iterations; ++w) {
    std::cerr << "[perf] scenario=" << scenario.name << " warmup=" << (w + 1) << "/"
              << scenario.warmup_iterations << '\n';
    ScenarioContext warmup_context(driver.shell, driver.window, driver.renderer);
    try {
      scenario.run(warmup_context);
    } catch (const std::exception& ex) {
      warmup_context.RestoreExternalFileChanges();
      HarnessError() = std::string("scenario threw during warmup: ") + ex.what();
      ShutdownDriver(&driver);
      return std::nullopt;
    } catch (...) {
      warmup_context.RestoreExternalFileChanges();
      HarnessError() = "scenario threw unknown exception during warmup";
      ShutdownDriver(&driver);
      return std::nullopt;
    }
    // Every iteration must start from the same fixture bytes, warmups included:
    // an append left in place makes iteration N+1 measure a bigger diff than
    // iteration N (TD-2026-08-06-155).
    warmup_context.RestoreExternalFileChanges();
  }

  // MICROIDE_PERF_SUMMARY=1 folds every trace scope into a ranked self-time
  // table. The harness is the tool you reach for when hunting a hotspot, yet it
  // was the one binary that never printed that table -- it has no shutdown path
  // that calls DumpSummaryOnce, so the env var silently did nothing here and the
  // ranked view was reachable only from a hand-driven live session. Scope the
  // table to one scenario's measured iterations: reset after warmup (whose
  // one-time cold work would otherwise dominate every row) and write it out
  // before the next scenario starts, so each scenario gets its own ranking
  // instead of one process-wide blend of 70 unrelated workloads.
  util::PerformanceTrace::ResetSummary();

  for (std::size_t i = 0; i < options.iterations; ++i) {
    std::cerr << "[perf] scenario=" << scenario.name << " iteration=" << (i + 1) << "/"
              << options.iterations << '\n';
    ScenarioContext context(driver.shell, driver.window, driver.renderer);
    if (options.layout_mode_override.has_value()) {
      if (!workspace::WorkspaceShell::TestAccess::SetSettingValue(
              driver.shell, "ui.layout_mode", *options.layout_mode_override)) {
        HarnessError() = "failed to apply layout mode override";
        ShutdownDriver(&driver);
        return std::nullopt;
      }
    }
    // Outside the measured window on purpose: this is a reading of the machine,
    // not of the scenario, and must not be charged to either cpu_ms or wall_ms.
    const std::uint64_t calibration_ns = MeasureCpuCalibrationNanoseconds();
    context.RecordCpuCalibration(calibration_ns);
    // Both resident readings are taken on a trimmed heap so the delta between them
    // is retained memory, not arena state. Trimming here (before cpu_ms_before) and
    // after cpu_ms_after keeps its own cost out of every measured metric.
    SettleResidentSet();
    const util::PerfCounterSnapshot counter_before = util::CapturePerformanceCounters();
    const AllocationSnapshot before = Allocations::Snapshot();
    const double cpu_ms_before = ProcessCpuMilliseconds();
    const std::uint64_t rss_before = ProcessResidentBytes();
    const auto start = std::chrono::steady_clock::now();
    try {
      scenario.run(context);
    } catch (const std::exception& ex) {
      context.RestoreExternalFileChanges();
      HarnessError() = std::string("scenario threw exception: ") + ex.what();
      ShutdownDriver(&driver);
      return std::nullopt;
    } catch (...) {
      context.RestoreExternalFileChanges();
      HarnessError() = "scenario threw unknown exception";
      ShutdownDriver(&driver);
      return std::nullopt;
    }
    const auto end = std::chrono::steady_clock::now();
    // The scenario declared it cannot run. Stop here rather than recording nine
    // more empty iterations: the aggregate is not a measurement, and the caller
    // reports it as a skip (and, for a gated scenario, a failure).
    if (context.skipped()) {
      context.RestoreExternalFileChanges();
      aggregate.skip_reason = context.skip_reason();
      aggregate.iterations.clear();
      ShutdownDriver(&driver);
      return aggregate;
    }
    const AllocationDelta delta = Allocations::DeltaSince(before);
    const double cpu_ms_after = ProcessCpuMilliseconds();
    SettleResidentSet();
    const std::uint64_t rss_after = ProcessResidentBytes();
    const util::PerfCounterSnapshot counter_after = util::CapturePerformanceCounters();
    // After every measurement is captured, so the filesystem work is charged to
    // nothing, and before the next iteration starts, so each one sees the same
    // fixture bytes (TD-2026-08-06-155).
    context.RestoreExternalFileChanges();
    std::vector<std::pair<std::string, std::uint64_t>> counter_deltas;
    for (const auto& [name, value] : util::NonZeroCounterDelta(counter_before, counter_after)) {
      counter_deltas.emplace_back(std::string(name), value);
    }
    for (auto& entry : context.TakeHarnessCounters()) {
      counter_deltas.push_back(std::move(entry));
    }
    aggregate.iterations.push_back(Iteration{
        .index = i,
        .metrics =
            MetricSnapshot{
                .wall_ms =
                    std::chrono::duration<double, std::milli>(end - start).count(),
                .allocations = static_cast<std::uint64_t>(std::max<std::int64_t>(0, delta.allocations)),
                .frees = static_cast<std::uint64_t>(std::max<std::int64_t>(0, delta.frees)),
                .bytes_allocated =
                    static_cast<std::uint64_t>(std::max<std::int64_t>(0, delta.bytes_allocated)),
                .bytes_freed =
                    static_cast<std::uint64_t>(std::max<std::int64_t>(0, delta.bytes_freed)),
                .cpu_ms = std::max(0.0, cpu_ms_after - cpu_ms_before),
                .cpu_calibration_ns = calibration_ns,
                .rss_growth_bytes = rss_after > rss_before ? rss_after - rss_before : 0,
                // Signed and unclamped, unlike the two byte counters above: the
                // sign is the measurement. See MetricSnapshot::net_heap_bytes.
                .net_heap_bytes = delta.bytes_allocated - delta.bytes_freed,
            },
        .phase_metrics = context.TakePhaseMetrics(),
        .perf_counters = std::move(counter_deltas),
    });
  }
  if (util::PerformanceTrace::SummaryEnabled()) {
    std::cerr << "[perf] trace summary for scenario=" << scenario.name << " ("
              << options.iterations << " measured iterations)\n";
    util::PerformanceTrace::WriteSummary(stderr);
  }
  aggregate.metrics = AggregateMetrics(aggregate.iterations);
  aggregate.phases = AggregatePhaseMetrics(aggregate.iterations);
  ShutdownDriver(&driver);
  return aggregate;
}

bool PerfHarness::InitializeDriver(Driver* driver,
                                   std::optional<std::uint64_t> random_seed,
                                   bool keep_artifacts,
                                   std::string_view renderer_driver,
                                   std::string_view video_driver) {
  if (driver == nullptr) {
    return false;
  }

  // Resolve the requested video driver the same way, and for the same reason:
  // the lane has to be a property of the harness, not of whatever the caller
  // happened to export. "dummy" (default) presents to nothing, which is what the
  // committed wall baselines are recorded against; "auto"/"default" leaves the
  // environment alone so an operator can measure a real window system on
  // purpose. Setting it here (before SDL_Init) is what makes a bare
  // `microide_perf` run reproduce the gate without an xvfb wrapper.
  if (video_driver == "auto" || video_driver == "default") {
    // Deliberately not unset: "auto" means "whatever this environment picks",
    // which includes an operator-exported SDL_VIDEODRIVER.
  } else if (!video_driver.empty()) {
    const std::string video_driver_owner(video_driver);
    setenv("SDL_VIDEODRIVER", video_driver_owner.c_str(), 1);
  }

  // Resolve the requested renderer driver. "software" (default) pins the
  // portable reference backend; "auto"/"default"/"" lets SDL pick the platform
  // GPU backend (clear the hint so SDL's own selection wins); anything else
  // forces that specific SDL driver. `requested_driver` is the string passed to
  // SDL_CreateRenderer (null => SDL auto-pick).
  const bool use_software = renderer_driver.empty() || renderer_driver == "software";
  const bool use_auto = renderer_driver == "auto" || renderer_driver == "default";
  std::string driver_name_owner;
  const char* requested_driver = nullptr;
  if (use_software) {
    setenv("SDL_HINT_RENDER_DRIVER", "software", 1);
    requested_driver = "software";
  } else if (use_auto) {
    unsetenv("SDL_HINT_RENDER_DRIVER");
    requested_driver = nullptr;
  } else {
    driver_name_owner = std::string(renderer_driver);
    setenv("SDL_HINT_RENDER_DRIVER", driver_name_owner.c_str(), 1);
    requested_driver = driver_name_owner.c_str();
  }
  const std::string seed_text = std::to_string(ResolveSeed(random_seed));
  setenv("MICROIDE_PERF_SEED", seed_text.c_str(), 1);

  driver->keep_artifacts = keep_artifacts;
  // Normally already established by RunScenario BEFORE the Driver was declared —
  // see the comment there; the shell's constructor loads user state, so doing it
  // here would be too late. Kept as a fallback for a caller that constructs a
  // Driver itself, where late isolation still beats none.
  if (driver->isolated_app_root.empty()) {
    std::string isolate_error;
    driver->isolated_app_root = EstablishIsolatedAppRoot(keep_artifacts, &isolate_error);
    if (driver->isolated_app_root.empty()) {
      HarnessError() = isolate_error.empty() ? "failed to establish isolated app-root"
                                              : isolate_error;
      return false;
    }
  }

  if (!SDL_Init(SDL_INIT_VIDEO)) {
    HarnessError() = std::string("SDL_Init failed: ") + SDL_GetError();
    return false;
  }
  if (const char* resolved_video = SDL_GetCurrentVideoDriver(); resolved_video != nullptr) {
    ResolvedVideoDriverStorage() = resolved_video;
  }

  driver->window = SDL_CreateWindow("microide-perf", 1920, 1080, SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (driver->window == nullptr) {
    HarnessError() = std::string("SDL_CreateWindow failed: ") + SDL_GetError();
    ShutdownDriver(driver);
    return false;
  }
  driver->renderer = SDL_CreateRenderer(driver->window, requested_driver);
  if (driver->renderer == nullptr) {
    HarnessError() = std::string("SDL_CreateRenderer failed: ") + SDL_GetError();
    ShutdownDriver(driver);
    return false;
  }
  SDL_SetRenderVSync(driver->renderer, 0);
  // Record the backend actually selected and gate the shell's batched-text path
  // on it, exactly as Application does in production, so the GPU advisory lane
  // measures the real GPU draw path (and the software lane keeps the composite
  // path).
  ResolvedRendererDriverStorage() = std::string(render::RendererDriverName(driver->renderer));
  driver->shell.SetRenderBackendInfo(ResolvedRendererDriverStorage(),
                                     render::RendererIsGpu(driver->renderer));

  // Do not pass current_path(): the harness cwd is usually the microide repo itself,
  // and treating it as the active project pulls async git refresh into unrelated scenarios.
  if (!driver->shell.Initialize({})) {
    HarnessError() = "WorkspaceShell initialize failed";
    ShutdownDriver(driver);
    return false;
  }
  driver->shell.SetDialogWindow(driver->window);
  driver->initialized = true;
  return true;
}

std::string PerfHarness::ResolvedRendererDriver() {
  return ResolvedRendererDriverStorage();
}

std::string PerfHarness::ResolvedVideoDriver() {
  return ResolvedVideoDriverStorage();
}

void PerfHarness::ShutdownDriver(Driver* driver) {
  if (driver == nullptr) {
    return;
  }
  if (driver->initialized) {
    driver->shell.SetDialogWindow(nullptr);
    driver->shell.Shutdown();
    driver->initialized = false;
  }
  if (driver->renderer != nullptr) {
    SDL_DestroyRenderer(driver->renderer);
    driver->renderer = nullptr;
  }
  if (driver->window != nullptr) {
    SDL_DestroyWindow(driver->window);
    driver->window = nullptr;
  }
  SDL_Quit();
  CleanupIsolatedAppRoot(driver->isolated_app_root, driver->keep_artifacts);
  driver->isolated_app_root.clear();
}

std::filesystem::path PerfHarness::FixtureManifestPath(const std::filesystem::path& root) {
  std::filesystem::path manifest = root;
  manifest += ".sha256";
  return manifest;
}

bool PerfHarness::VerifyFixtureTree(const std::filesystem::path& root, std::string* error) {
  const std::filesystem::path expected_hash_file = FixtureManifestPath(root);
  std::ifstream expected_in(expected_hash_file);
  if (!expected_in) {
    if (error) {
      *error = "missing expected hash file: " + expected_hash_file.string();
    }
    return false;
  }
  std::string expected;
  std::getline(expected_in, expected);
  if (expected.empty()) {
    if (error) {
      *error = "empty expected hash file: " + expected_hash_file.string();
    }
    return false;
  }

  // In-process, matching the committed manifests written by the generator scripts:
  // for each regular file under `root`, in ascending relative-POSIX-path order,
  // feed <relative path> NUL <file bytes> NUL.
  //
  // This used to popen a python3 heredoc with the root interpolated into the
  // script. That is the same subprocess-per-hash pattern TD-2026-07-17-060 removed
  // from tool-download verification, and it fails the same three ways: it needs a
  // python3 on PATH to run the perf gate at all, it cannot distinguish "hash
  // differs" from "the interpreter never produced a line", and it interpolates a
  // filesystem path into shell+python source. util::Sha256 already exists for
  // exactly this.
  std::vector<std::string> relative_paths;
  std::error_code walk_error;
  for (std::filesystem::recursive_directory_iterator it(root, walk_error), end; it != end;
       it.increment(walk_error)) {
    if (walk_error) {
      if (error) {
        *error = "failed to walk fixture tree " + root.string() + ": " + walk_error.message();
      }
      return false;
    }
    std::error_code kind_error;
    if (!it->is_regular_file(kind_error) || kind_error) {
      continue;
    }
    relative_paths.push_back(it->path().lexically_relative(root).generic_string());
  }
  if (walk_error) {
    if (error) {
      *error = "failed to walk fixture tree " + root.string() + ": " + walk_error.message();
    }
    return false;
  }
  // Byte-wise on the relative POSIX string, which is what the generators' `sorted()`
  // over Path objects reduces to. std::filesystem::path's own operator< compares
  // element-by-element and orders "a/b" before "a.b" where the string compare orders
  // them the other way, so sorting paths here would silently disagree with the
  // committed manifests on any tree holding both.
  std::sort(relative_paths.begin(), relative_paths.end());

  util::Sha256 digest;
  constexpr char kSeparator = '\0';
  std::string file_buffer;
  for (const std::string& relative : relative_paths) {
    digest.Update(relative);
    digest.Update(std::string_view(&kSeparator, 1));
    std::ifstream file_in(root / relative, std::ios::binary);
    if (!file_in) {
      if (error) {
        *error = "failed to read fixture file: " + (root / relative).string();
      }
      return false;
    }
    // One reused buffer for the whole tree; the fixtures run to tens of MB.
    constexpr std::size_t kChunkBytes = 1u << 16;
    file_buffer.resize(kChunkBytes);
    while (file_in.read(file_buffer.data(), static_cast<std::streamsize>(kChunkBytes)) ||
           file_in.gcount() > 0) {
      digest.Update(std::string_view(file_buffer.data(), static_cast<std::size_t>(file_in.gcount())));
      if (!file_in) {
        break;
      }
    }
    if (file_in.bad()) {
      if (error) {
        *error = "failed to read fixture file: " + (root / relative).string();
      }
      return false;
    }
    digest.Update(std::string_view(&kSeparator, 1));
  }

  const std::string actual = digest.FinishHex();
  if (actual != expected) {
    if (error) {
      *error = "fixture hash mismatch for " + root.string() + " expected=" + expected +
               " actual=" + actual + " (" + std::to_string(relative_paths.size()) + " files)";
    }
    return false;
  }
  return true;
}

std::string PerfHarness::LastError() {
  return HarnessError();
}

ScenarioRegistration::ScenarioRegistration(const Scenario& scenario) {
  PerfHarness::RegisterScenario(scenario);
}

}  // namespace microide::tests::perf
