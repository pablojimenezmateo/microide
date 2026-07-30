#include "perf/PerfHarness.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <thread>
#include <limits>
#include <sstream>

#include "perf/AllocationCounter.h"
#include "perf/PerfHarnessIsolation.h"
#include "WorkspaceShellEventHelpers.h"
#include "render/RendererInfo.h"
#include "util/PerformanceCounters.h"
#include "workspace/WorkspaceShellTestAccess.h"

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

MetricSet AggregateMetrics(const std::vector<Iteration>& iterations) {
  std::vector<double> wall_ms;
  std::vector<double> allocations;
  wall_ms.reserve(iterations.size());
  allocations.reserve(iterations.size());
  for (const Iteration& iteration : iterations) {
    wall_ms.push_back(iteration.metrics.wall_ms);
    allocations.push_back(static_cast<double>(iteration.metrics.allocations));
  }
  MetricSet out;
  out.p50_wall_ms = Percentile(wall_ms, 0.50);
  out.p95_wall_ms = Percentile(wall_ms, 0.95);
  out.max_wall_ms = wall_ms.empty() ? 0.0 : *std::max_element(wall_ms.begin(), wall_ms.end());
  out.p50_allocations = Percentile(allocations, 0.50);
  out.p95_allocations = Percentile(allocations, 0.95);
  out.max_allocations =
      allocations.empty() ? 0.0 : *std::max_element(allocations.begin(), allocations.end());
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

ScenarioContext::ScenarioContext(workspace::WorkspaceShell& shell,
                                 SDL_Window* window,
                                 SDL_Renderer* renderer)
    : shell_(shell), window_(window), renderer_(renderer), rng_(ResolveSeed(std::nullopt)) {}

void ScenarioContext::PumpFrames(std::size_t count) {
  for (std::size_t i = 0; i < count; ++i) {
    shell_.Render(renderer_, 1920, 1080);
    SDL_RenderPresent(renderer_);
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
    if (idle_state.hint == workspace::WorkspaceShell::IdleHint::Idle) {
      std::this_thread::sleep_for(std::min(remaining, std::chrono::milliseconds(20)));
      continue;
    }
    if (idle_state.hint == workspace::WorkspaceShell::IdleHint::CaretOnly &&
        idle_state.caret_remaining_ms > 1) {
      const auto caret_delay = std::chrono::milliseconds(idle_state.caret_remaining_ms);
      std::this_thread::sleep_for(std::min(remaining, caret_delay));
      continue;
    }

    const auto wake = shell_.HandleScheduledWake();
    if (wake.handled) {
      ++wake_count;
      PumpFrames(1);
      continue;
    }
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
                        options.renderer_driver)) {
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
    const util::PerfCounterSnapshot counter_after = util::CapturePerformanceCounters();
    std::vector<std::pair<std::string, std::uint64_t>> counter_deltas;
    for (const auto& [name, value] : util::NonZeroCounterDelta(counter_before, counter_after)) {
      counter_deltas.emplace_back(std::string(name), value);
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
            },
        .phase_metrics = context.TakePhaseMetrics(),
        .perf_counters = std::move(counter_deltas),
    });
  }
  aggregate.metrics = AggregateMetrics(aggregate.iterations);
  ShutdownDriver(&driver);
  return aggregate;
}

bool PerfHarness::InitializeDriver(Driver* driver,
                                   std::optional<std::uint64_t> random_seed,
                                   bool keep_artifacts,
                                   std::string_view renderer_driver) {
  if (driver == nullptr) {
    return false;
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

bool PerfHarness::VerifyFixtureTree(const std::filesystem::path& root,
                                    const std::filesystem::path& expected_hash_file,
                                    std::string* error) {
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
  std::ostringstream command;
  command << "python3 - <<'PY'\n"
          << "import hashlib\n"
          << "from pathlib import Path\n"
          << "root=Path(r'" << root.string() << "')\n"
          << "d=hashlib.sha256()\n"
          << "for p in sorted([x for x in root.rglob('*') if x.is_file()]):\n"
          << "  rel=p.relative_to(root).as_posix().encode('utf-8')\n"
          << "  d.update(rel); d.update(b'\\0'); d.update(p.read_bytes()); d.update(b'\\0')\n"
          << "print(d.hexdigest())\n"
          << "PY";
  FILE* pipe = popen(command.str().c_str(), "r");
  if (pipe == nullptr) {
    if (error) {
      *error = "failed to compute fixture hash";
    }
    return false;
  }
  char buffer[256] = {0};
  std::string actual;
  if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
    actual = buffer;
  }
  pclose(pipe);
  while (!actual.empty() && (actual.back() == '\n' || actual.back() == '\r')) {
    actual.pop_back();
  }
  if (actual != expected) {
    if (error) {
      *error = "fixture hash mismatch expected=" + expected + " actual=" + actual;
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
