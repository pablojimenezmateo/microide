#include "perf/PerfHarness.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#if defined(__unix__)
#include <sys/resource.h>
#include <unistd.h>
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

double Percentile(std::vector<double> values, double p) {
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

double MaxOr0(const std::vector<double>& values) {
  return values.empty() ? 0.0 : *std::max_element(values.begin(), values.end());
}

MetricSet AggregateMetrics(const std::vector<Iteration>& iterations) {
  std::vector<double> wall_ms;
  std::vector<double> allocations;
  std::vector<double> cpu_ms;
  std::vector<double> rss_growth_bytes;
  wall_ms.reserve(iterations.size());
  allocations.reserve(iterations.size());
  cpu_ms.reserve(iterations.size());
  rss_growth_bytes.reserve(iterations.size());
  for (const Iteration& iteration : iterations) {
    wall_ms.push_back(iteration.metrics.wall_ms);
    allocations.push_back(static_cast<double>(iteration.metrics.allocations));
    cpu_ms.push_back(iteration.metrics.cpu_ms);
    rss_growth_bytes.push_back(static_cast<double>(iteration.metrics.rss_growth_bytes));
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
  const AllocationSnapshot before = Allocations::Snapshot();
  const auto start = std::chrono::steady_clock::now();
  action();
  const auto end = std::chrono::steady_clock::now();
  const AllocationDelta delta = Allocations::DeltaSince(before);
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
  return WaitForGitSidebarEntries(0, timeout) &&
         !workspace::WorkspaceShell::TestAccess::GitSidebarRefreshing(shell_);
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

  Driver driver;
  if (!InitializeDriver(&driver, options.random_seed, options.keep_artifacts,
                        options.renderer_driver, options.video_driver)) {
    return std::nullopt;
  }

  Aggregate aggregate;
  aggregate.scenario_name = scenario.name;
  aggregate.smoke = scenario.smoke;
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
      HarnessError() = std::string("scenario threw during warmup: ") + ex.what();
      ShutdownDriver(&driver);
      return std::nullopt;
    } catch (...) {
      HarnessError() = "scenario threw unknown exception during warmup";
      ShutdownDriver(&driver);
      return std::nullopt;
    }
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
    const util::PerfCounterSnapshot counter_before = util::CapturePerformanceCounters();
    const AllocationSnapshot before = Allocations::Snapshot();
    const double cpu_ms_before = ProcessCpuMilliseconds();
    const std::uint64_t rss_before = ProcessResidentBytes();
    const auto start = std::chrono::steady_clock::now();
    try {
      scenario.run(context);
    } catch (const std::exception& ex) {
      HarnessError() = std::string("scenario threw exception: ") + ex.what();
      ShutdownDriver(&driver);
      return std::nullopt;
    } catch (...) {
      HarnessError() = "scenario threw unknown exception";
      ShutdownDriver(&driver);
      return std::nullopt;
    }
    const auto end = std::chrono::steady_clock::now();
    const AllocationDelta delta = Allocations::DeltaSince(before);
    const double cpu_ms_after = ProcessCpuMilliseconds();
    const std::uint64_t rss_after = ProcessResidentBytes();
    const util::PerfCounterSnapshot counter_after = util::CapturePerformanceCounters();
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
                .rss_growth_bytes = rss_after > rss_before ? rss_after - rss_before : 0,
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

  std::string isolate_error;
  driver->keep_artifacts = keep_artifacts;
  driver->isolated_app_root = EstablishIsolatedAppRoot(keep_artifacts, &isolate_error);
  if (driver->isolated_app_root.empty()) {
    HarnessError() = isolate_error.empty() ? "failed to establish isolated app-root"
                                            : isolate_error;
    return false;
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
