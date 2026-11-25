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

#include <sys/resource.h>
#include <fcntl.h>
#include <unistd.h>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

#include "app/DirtyRegionPolicy.h"
#include "workspace/debug/DapProtocol.h"
#include "workspace/debug/DebugVariablesModel.h"
#include "platform/Subprocess.h"
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

std::string& ResolvedRendererDriverStorage() {
  static std::string driver;
  return driver;
}

std::string& ResolvedVideoDriverStorage() {
  static std::string driver;
  return driver;
}

double MaxOr0(const std::vector<double>& values) {
  return values.empty() ? 0.0 : *std::max_element(values.begin(), values.end());
}

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
  return static_cast<std::uint64_t>(
             std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()) |
         (sink & 0U);
}

}

double ProcessCpuMilliseconds() {
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0) {
    return 0.0;
  }
  const auto to_ms = [](const timeval& tv) {
    return static_cast<double>(tv.tv_sec) * 1000.0 + static_cast<double>(tv.tv_usec) / 1000.0;
  };
  return to_ms(usage.ru_utime) + to_ms(usage.ru_stime);
}

void SettleResidentSet() {
#if defined(__GLIBC__)
  malloc_trim(0);
#endif
}

std::uint64_t ProcessResidentBytes() {
  const int fd = ::open("/proc/self/statm", O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return 0;
  }
  char buffer[128];
  const ssize_t read_bytes = ::read(fd, buffer, sizeof(buffer) - 1);
  ::close(fd);
  if (read_bytes <= 0) {
    return 0;
  }
  buffer[read_bytes] = '\0';
  const char* cursor = buffer;
  while (*cursor != '\0' && *cursor != ' ') {
    ++cursor;
  }
  while (*cursor == ' ') {
    ++cursor;
  }
  std::uint64_t resident_pages = 0;
  if (*cursor < '0' || *cursor > '9') {
    return 0;
  }
  for (; *cursor >= '0' && *cursor <= '9'; ++cursor) {
    resident_pages = resident_pages * 10 + static_cast<std::uint64_t>(*cursor - '0');
  }
  const long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0) {
    return 0;
  }
  return resident_pages * static_cast<std::uint64_t>(page_size);
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
      util::PerformanceTrace::Scope scope("PerfHarness::PumpFrames::ShellRender");
      shell_.Render(renderer_, 1920, 1080);
    }
    {
      util::PerformanceTrace::Scope scope("PerfHarness::PumpFrames::Present");
      SDL_RenderPresent(renderer_);
    }
  }
}

ScenarioContext::PumpedFrameDamage ScenarioContext::PumpPartialFrame(
    const workspace::WorkspaceShell::RenderInvalidation& event_damage) {
  BumpHarnessCounter(HarnessCounter::kFramesPumped);
  const workspace::WorkspaceShell::RenderInvalidation pending =
      shell_.ConsumePendingRenderInvalidation();
  partial_dirty_rects_.assign(event_damage.rects.begin(), event_damage.rects.end());
  partial_dirty_rects_.insert(partial_dirty_rects_.end(), pending.rects.begin(),
                              pending.rects.end());
  const bool frame_owed = event_damage.frame || pending.frame;
  bool requested_full = event_damage.full || pending.full || (partial_dirty_rects_.empty() && !frame_owed);
  const workspace::WorkspaceShell::FrameToken frame_token =
      shell_.PrepareFrameOnce(renderer_, 1920, 1080);
  if (const workspace::WorkspaceShell::RenderInvalidation prepared =
          shell_.ConsumePendingRenderInvalidation();
      !requested_full) {
    if (prepared.full) {
      requested_full = true;
    } else {
      partial_dirty_rects_.insert(partial_dirty_rects_.end(), prepared.rects.begin(), prepared.rects.end());
    }
  }
  if (!requested_full && partial_dirty_rects_.empty()) {
    return PumpedFrameDamage{};
  }
  PumpedFrameDamage damage{.full = requested_full};
  for (const SDL_FRect& rect : partial_dirty_rects_) {
    damage.area += static_cast<double>(rect.w) * static_cast<double>(rect.h);
  }
  app::AnalyzeDirtyRegions(partial_dirty_rects_, shell_.PartialRedrawClipPadding(), 1920, 1080, SDL_FPoint{1.0f, 1.0f},
                           partial_analysis_);
  const app::DirtyRegionAnalysis& analysis = partial_analysis_;
  const bool promoted = !requested_full && app::ShouldPromotePartialFrameToFull(analysis);
  const bool full_frame = requested_full || analysis.merged_clip_rects.empty() || promoted;
  if (full_frame) {
    if (promoted) {
      util::AddPerformanceCounter(util::PerfCounterId::RenderPromotedFullFrames);
    }
    util::PerformanceTrace::Scope scope("PerfHarness::PumpPartialFrames::ShellRender(full)");
    shell_.RenderClip(frame_token, renderer_, 1920, 1080);
  } else {
    util::PerformanceTrace::Scope scope("PerfHarness::PumpPartialFrames::ShellRender(partial)");
    for (const SDL_Rect& clip_rect : analysis.merged_clip_rects) {
      SDL_SetRenderClipRect(renderer_, &clip_rect);
      const SDL_FRect dirty_rect_hint{
          static_cast<float>(clip_rect.x), static_cast<float>(clip_rect.y),
          static_cast<float>(clip_rect.w), static_cast<float>(clip_rect.h)};
      shell_.RenderClip(frame_token, renderer_, 1920, 1080, dirty_rect_hint);
      util::AddPerformanceCounter(util::PerfCounterId::RenderPartialClips);
      util::AddPerformanceCounter(
          util::PerfCounterId::RenderPartialClipPixels,
          static_cast<std::uint64_t>(clip_rect.w) * static_cast<std::uint64_t>(clip_rect.h));
    }
    SDL_SetRenderClipRect(renderer_, nullptr);
  }
  {
    util::PerformanceTrace::Scope scope("PerfHarness::PumpPartialFrames::Present");
    SDL_RenderPresent(renderer_);
  }
  if (damage.full) {
    damage.area = 1920.0 * 1080.0;
  }
  return damage;
}

void ScenarioContext::PumpPartialFrames(std::size_t count) {
  for (std::size_t i = 0; i < count; ++i) {
    PumpPartialFrame(workspace::WorkspaceShell::RenderInvalidation{});
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
  using TA = workspace::WorkspaceShell::TestAccess;
  std::error_code error;
  const std::filesystem::path resolved = std::filesystem::absolute(path, error).lexically_normal();
  TA::OpenFile(shell_, error || resolved.empty() ? path : resolved);
  TA::FlushPendingFileReads(shell_);
}

void ScenarioContext::CloseActiveTab() {
  if (workspace::WorkspaceShell::TestAccess::FocusedGroupOpenTabCount(shell_) == 0) {
    return;
  }
  workspace::WorkspaceShell::TestAccess::CloseTab(
      shell_, workspace::WorkspaceShell::TestAccess::ActiveTabIndex(shell_));
}

workspace::WorkspaceShell::RenderInvalidation ScenarioContext::Type(std::string_view text) {
  SDL_Event event{};
  event.type = SDL_EVENT_TEXT_INPUT;
  std::string storage(text);
  event.text.text = storage.c_str();
  return shell_.HandleEvent(event).redraw;
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
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    PumpEvents();
    workspace::WorkspaceShell::TestAccess::ConsumeGitSidebarRefresh(shell_);
    if (!workspace::WorkspaceShell::TestAccess::GitSidebarRefreshing(shell_)) {
      return true;
    }
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
  KeyDown(key, SDL_KMOD_ALT);
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
  platform::IndexUpdateBatch batch;
  batch.is_initial = false;
  platform::IndexUpdateBatch::Change change;
  change.kind = platform::IndexUpdateBatch::Kind::CreatedOrModified;
  const std::filesystem::path root = workspace::WorkspaceShell::TestAccess::ProjectRoot(shell_);
  change.entry.relative_path =
      root.empty() ? resolved : resolved.lexically_relative(root.lexically_normal());
  std::error_code mtime_error;
  change.entry.mtime = std::filesystem::last_write_time(resolved, mtime_error);
  std::error_code size_error;
  change.entry.size = std::filesystem::file_size(resolved, size_error);
  batch.changes.push_back(std::move(change));
  if (!workspace::WorkspaceShell::TestAccess::DispatchFileIndexWatcherBatchForTesting(
          shell_, std::move(batch))) {
    (void)workspace::WorkspaceShell::TestAccess::ReloadProjectIfFilesChanged(shell_, true);
  }
  (void)workspace::WorkspaceShell::TestAccess::ReloadProjectIfFilesChanged(shell_, false);
  PumpFrames(2);
}

void ScenarioContext::RestoreExternalFileChanges() {
  for (const auto& [path, size] : external_file_restores_) {
    std::error_code error;
    std::filesystem::resize_file(path, size, error);
    if (error) {
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

void ScenarioContext::ShowOutputChannel(std::string_view id) {
  using TA = workspace::WorkspaceShell::TestAccess;
  TA::SetOutputChannelTabs(shell_, {std::string(id)}, std::string(id));
  TA::CurrentProjectState(shell_).panel.output.follow_tail = true;
}

void ScenarioContext::AppendOutputLine(std::string_view id, std::string line) {
  workspace::WorkspaceShell::TestAccess::AppendOutputLine(shell_, id, "Perf Output", std::move(line));
}

void ScenarioContext::ShowDebugVariables(std::size_t count) {
  using TA = workspace::WorkspaceShell::TestAccess;
  if (!TA::SetSettingValueTransient(shell_, "debug.enabled", "true")) {
    throw std::runtime_error("ShowDebugVariables: debug.enabled did not take");
  }
  TA::SetDebugPaneVisible(shell_, true);
  TA::SetDebugPaneMode(shell_, workspace::DebugPaneMode::Variables);
  auto& variables = TA::DebugVariables(shell_);
  variables.BeginFrame(1);
  workspace::dap_protocol::DapScope locals;
  locals.name = "Locals";
  locals.variables_reference = 1000;
  variables.ApplyScopes({locals});
  ApplyDebugVariables(DebugVariableSteps(count, 1).front());
}

std::vector<ScenarioContext::DebugValues> ScenarioContext::DebugVariableSteps(std::size_t count, int steps) {
  std::vector<DebugValues> built;
  for (int step = 1; step <= steps; ++step) {
    DebugValues& values = built.emplace_back();
    values.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
      workspace::dap_protocol::DapVariable variable;
      variable.name = "local_" + std::to_string(i);
      variable.value = std::to_string(i % 8 == 0 ? static_cast<int>(i) * 1000 + step : static_cast<int>(i));
      variable.type = "int";
      values.push_back(std::move(variable));
    }
  }
  return built;
}

void ScenarioContext::ApplyDebugVariables(const DebugValues& values) {
  (void)workspace::WorkspaceShell::TestAccess::DebugVariables(shell_).ApplyVariables(1000, values, 0);
}

void ScenarioContext::ShowDebugPaneVariables(bool variables) {
  workspace::WorkspaceShell::TestAccess::SetDebugPaneMode(
      shell_, variables ? workspace::DebugPaneMode::Variables : workspace::DebugPaneMode::CallStack);
}

void ScenarioContext::FeedTerminalOutput(std::string_view bytes) {
  if (workspace::WorkspaceShell::TestAccess::TerminalTabCount(shell_) == 0) {
    throw std::runtime_error("FeedTerminalOutput: no terminal is open");
  }
  TerminalSessionTestAccess::AppendOutput(
      workspace::WorkspaceShell::TestAccess::ActiveTerminalSession(shell_), bytes);
  PumpEvents();
}

void ScenarioContext::CloseAllTerminals() {
  while (workspace::WorkspaceShell::TestAccess::TerminalTabCount(shell_) > 0) {
    workspace::WorkspaceShell::TestAccess::CloseTerminalTab(shell_, 0);
  }
  PumpFrames(1);
}

workspace::WorkspaceShell::RenderInvalidation ScenarioContext::MousePress(float x, float y) {
  SDL_Event event{};
  event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
  event.button.button = SDL_BUTTON_LEFT;
  event.button.x = x;
  event.button.y = y;
  event.button.clicks = 1;
  return shell_.HandleEvent(event).redraw;
}

workspace::WorkspaceShell::RenderInvalidation ScenarioContext::MouseMoveDragging(float x, float y) {
  SDL_Event event{};
  event.type = SDL_EVENT_MOUSE_MOTION;
  event.motion.x = x;
  event.motion.y = y;
  event.motion.state = SDL_BUTTON_LMASK;
  return shell_.HandleEvent(event).redraw;
}

workspace::WorkspaceShell::EventResult ScenarioContext::MouseMoveHover(float x, float y) {
  SDL_Event event{};
  event.type = SDL_EVENT_MOUSE_MOTION;
  event.motion.x = x;
  event.motion.y = y;
  event.motion.state = 0;
  return shell_.HandleEvent(event);
}

workspace::WorkspaceShell::RenderInvalidation ScenarioContext::MouseRelease(float x, float y) {
  SDL_Event event{};
  event.type = SDL_EVENT_MOUSE_BUTTON_UP;
  event.button.button = SDL_BUTTON_LEFT;
  event.button.x = x;
  event.button.y = y;
  return shell_.HandleEvent(event).redraw;
}

SDL_FRect ScenarioContext::EditorTabRect(std::size_t tab_index) {
  return workspace::WorkspaceShell::TestAccess::EditorTabRect(shell_, tab_index);
}

SDL_FRect ScenarioContext::EditorGroupTabStripRect(std::size_t group_index) {
  return workspace::WorkspaceShell::TestAccess::GroupTabStripRect(shell_, group_index);
}

std::size_t ScenarioContext::EditorGroupCount() {
  return workspace::WorkspaceShell::TestAccess::EditorGroupCount(shell_);
}

SDL_FRect ScenarioContext::EditorGroupSurfaceRect(std::size_t group_index) {
  return workspace::WorkspaceShell::TestAccess::GroupEditorSurfaceRect(shell_, group_index);
}

std::size_t ScenarioContext::EditorSplitDividerCount() {
  return workspace::WorkspaceShell::TestAccess::EditorSplitDividerRects(shell_).size();
}

SDL_FRect ScenarioContext::EditorSplitDividerRect(std::size_t divider_index) {
  const auto dividers = workspace::WorkspaceShell::TestAccess::EditorSplitDividerRects(shell_);
  return divider_index < dividers.size() ? dividers[divider_index].rect : SDL_FRect{};
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

  {
    ScenarioContext warm_process(driver.shell, driver.window, driver.renderer);
    warm_process.PumpFrames(3);
  }

  Aggregate aggregate;
  aggregate.scenario_name = scenario.name;
  aggregate.smoke = scenario.smoke;
  aggregate.measurement_revision = scenario.measurement_revision;
  aggregate.iterations.reserve(options.iterations);

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
    warmup_context.RestoreExternalFileChanges();
  }

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
    const std::uint64_t calibration_ns = MeasureCpuCalibrationNanoseconds();
    context.RecordCpuCalibration(calibration_ns);
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

  if (video_driver == "auto" || video_driver == "default") {
  } else if (!video_driver.empty()) {
    const std::string video_driver_owner(video_driver);
    setenv("SDL_VIDEODRIVER", video_driver_owner.c_str(), 1);
  }

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
  ResolvedRendererDriverStorage() = std::string(render::RendererDriverName(driver->renderer));
  driver->shell.SetRenderBackendInfo(ResolvedRendererDriverStorage(),
                                     render::RendererIsGpu(driver->renderer));

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

namespace {

bool FixtureGitStatus(const std::filesystem::path& root, std::string* out, std::string* error) {
  std::vector<std::string> argv = {"git", "-C", root.string()};
  for (const char* pin : {"core.excludesFile=/dev/null", "core.attributesFile=/dev/null",
                          "core.autocrlf=false", "core.safecrlf=false", "core.hooksPath=/dev/null",
                          "commit.gpgsign=false", "gc.auto=0", "init.defaultBranch=main",
                          "user.name=Fixture", "user.email=fixture@microide"}) {
    argv.emplace_back("-c");
    argv.emplace_back(pin);
  }
  for (const char* arg : {"status", "--porcelain=v1", "-uall", "-z"}) {
    argv.emplace_back(arg);
  }
  platform::SubprocessOptions options;
  options.capture_stderr = true;
  options.timeout_ms = 60000;
  for (const char* name : {"GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_COMMON_DIR",
                           "GIT_OBJECT_DIRECTORY", "GIT_ALTERNATE_OBJECT_DIRECTORIES",
                           "GIT_NAMESPACE", "GIT_PREFIX"}) {
    options.environment_overrides.push_back({.name = name, .value = std::nullopt});
  }
  options.environment_overrides.push_back({.name = "GIT_CONFIG_NOSYSTEM", .value = "1"});
  options.environment_overrides.push_back(
      {.name = "HOME", .value = "/nonexistent-microide-fixture-home"});
  options.environment_overrides.push_back(
      {.name = "XDG_CONFIG_HOME", .value = "/nonexistent-microide-fixture-home"});
  platform::SubprocessResult result = platform::RunSubprocess(argv, options);
  if (result.exit_code != 0) {
    if (error) {
      *error = "git status failed in fixture " + root.string() + ": " + result.stderr_text;
    }
    return false;
  }
  *out = std::move(result.stdout_text);
  return true;
}

}

bool PerfHarness::VerifyPresentFixtureTrees(const std::filesystem::path& fixtures_root,
                                            std::string* error) {
  std::vector<std::filesystem::path> trees;
  std::error_code list_error;
  for (std::filesystem::directory_iterator it(fixtures_root, list_error), end;
       !list_error && it != end; it.increment(list_error)) {
    if (it->path().extension() != ".sha256") {
      continue;
    }
    std::filesystem::path tree = it->path();
    tree.replace_extension();
    std::error_code tree_error;
    if (std::filesystem::is_directory(tree, tree_error) && !tree_error) {
      trees.push_back(std::move(tree));
    }
  }
  std::sort(trees.begin(), trees.end());
  for (const std::filesystem::path& tree : trees) {
    if (!VerifyFixtureTree(tree, error)) {
      return false;
    }
  }
  return true;
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

  std::error_code git_error;
  const bool git_repository = std::filesystem::exists(root / ".git", git_error) && !git_error;
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
    if (git_repository && it->path().filename() == ".git") {
      it.disable_recursion_pending();
      continue;
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

  if (git_repository) {
    std::string status;
    if (!FixtureGitStatus(root, &status, error)) {
      return false;
    }
    digest.Update(std::string_view("git-status\0", 11));
    digest.Update(status);
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

}
