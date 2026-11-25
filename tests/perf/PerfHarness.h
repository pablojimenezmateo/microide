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

#include "app/DirtyRegionPolicy.h"
#include "workspace/debug/DapProtocol.h"
#include "workspace/shell/WorkspaceShell.h"
#include "workspace/WorkspaceVirtualDocument.h"

#include "compare/MergeModel.h"
#include "editor/DiagnosticsStore.h"
#include "editor/TextViewport.h"
#include "perf/PerfHarnessIsolation.h"

namespace microide::tests::perf {

inline bool PathExistsNoThrow(const std::filesystem::path& path) {
  std::error_code ec;
  return std::filesystem::exists(path, ec) && !ec;
}

inline bool DirectoryExistsNoThrow(const std::filesystem::path& path) {
  std::error_code ec;
  return std::filesystem::is_directory(path, ec) && !ec;
}

class ScenarioContext;

bool RequireFixture(ScenarioContext& context,
                    const std::filesystem::path& fixture,
                    std::string_view scenario_label);

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
  double cpu_ms = 0.0;
  std::uint64_t cpu_calibration_ns = 0;
  std::uint64_t rss_growth_bytes = 0;
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
  double mean_rss_growth_bytes = 0.0;
  double p50_net_heap_bytes = 0.0;
  double p50_cpu_calibration_ns = 0.0;
};

double ProcessCpuMilliseconds();
std::uint64_t ProcessResidentBytes();
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

struct PhaseMetricSet {
  std::string name;
  double p50_allocations = 0.0;
  double max_allocations = 0.0;
  double p50_wall_ms = 0.0;
  std::size_t iterations = 0;
};

inline std::string PerfBuildConfig() {
#ifdef MICROIDE_PERF_BUILD_CONFIG
  return MICROIDE_PERF_BUILD_CONFIG;
#else
  return {};
#endif
}

struct Aggregate {
  std::string scenario_name;
  std::vector<Iteration> iterations;
  MetricSet metrics;
  std::vector<PhaseMetricSet> phases;
  bool smoke = false;
  std::size_t measurement_revision = 1;
  std::string build_config = PerfBuildConfig();
  std::string skip_reason;
};

inline std::vector<PhaseMetricSet> AggregatePhaseMetrics(const std::vector<Iteration>& iterations) {
  std::vector<PhaseMetricSet> phases;
  std::vector<std::vector<double>> allocation_samples;
  std::vector<std::vector<double>> wall_samples;
  std::unordered_map<std::string_view, std::size_t> index_by_name;

  for (const Iteration& iteration : iterations) {
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
  std::string runner_class;
  std::string sdl_video_driver;
  std::string sdl_renderer_driver;
  std::vector<std::string> scenarios;
  std::size_t iterations = 0;
  std::string layout_mode;
  std::uint64_t seed = 0;
  std::string provenance;
  std::string isolated_app_root;
  std::string cpu_affinity;
  std::string timestamp_utc;
};


class ScenarioContext {
 public:
  ScenarioContext(workspace::WorkspaceShell& shell, SDL_Window* window, SDL_Renderer* renderer);

  void PumpFrames(std::size_t count);
  struct PumpedFrameDamage {
    bool full = false;
    double area = 0.0;
  };
  PumpedFrameDamage PumpPartialFrame(const workspace::WorkspaceShell::RenderInvalidation& event_damage);
  void PumpPartialFrames(std::size_t count);
  void PumpEvents();
  bool Open(const std::filesystem::path& project_root);
  void OpenTab(const std::filesystem::path& path);
  void CloseActiveTab();
  workspace::WorkspaceShell::RenderInvalidation Type(std::string_view text);
  void Scroll(int vertical_ticks);
  void KeyDown(SDL_Keycode key, SDL_Keymod modifiers = SDL_KMOD_NONE);
  bool ExecuteCommand(std::string_view command_line);
  void ToggleProjectSearchPatternMode();
  void ConsumeProjectSearchUpdates();
  std::uint64_t Wait(std::chrono::milliseconds duration);
  bool WaitForDiagnostics(const std::filesystem::path& path,
                          std::chrono::milliseconds timeout);
  void PublishDiagnostics(std::string_view owner,
                          const std::filesystem::path& path,
                          std::vector<editor::Diagnostic> diagnostics);
  bool HasDiagnostics(const std::filesystem::path& path) const;
  bool WaitForFileIndexPath(const std::filesystem::path& relative_path,
                            std::chrono::milliseconds timeout);
  bool WaitForProjectSearchFinished(std::chrono::milliseconds timeout);
  bool AssertNoAllocationsDuringDraw(std::string* error = nullptr);
  double Measure(std::string_view phase_name, const std::function<void()>& action);
  std::vector<Iteration::PhaseMetrics> TakePhaseMetrics();
  std::vector<std::pair<std::string, std::uint64_t>> TakeHarnessCounters();
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
  void RestoreExternalFileChanges();
  void StartSearch(std::string_view query);
  void OpenTerminal(std::string_view command);
  void FeedTerminalOutput(std::string_view bytes);
  void ShowOutputChannel(std::string_view id);
  void AppendOutputLine(std::string_view id, std::string line);
  void ShowDebugVariables(std::size_t count);
  using DebugValues = std::vector<workspace::dap_protocol::DapVariable>;
  static std::vector<DebugValues> DebugVariableSteps(std::size_t count, int steps);
  void ApplyDebugVariables(const DebugValues& values);
  void ShowDebugPaneVariables(bool variables);
  void CloseAllTerminals();
  void ResizeWindow(int width, int height);

  workspace::WorkspaceShell::RenderInvalidation MousePress(float x, float y);
  workspace::WorkspaceShell::RenderInvalidation MouseMoveDragging(float x, float y);
  workspace::WorkspaceShell::EventResult MouseMoveHover(float x, float y);
  workspace::WorkspaceShell::RenderInvalidation MouseRelease(float x, float y);
  SDL_FRect EditorTabRect(std::size_t tab_index);
  SDL_FRect EditorGroupTabStripRect(std::size_t group_index);
  std::size_t EditorGroupCount();
  SDL_FRect EditorGroupSurfaceRect(std::size_t group_index);
  std::size_t EditorSplitDividerCount();
  SDL_FRect EditorSplitDividerRect(std::size_t divider_index);

  void SetSetting(std::string_view id, std::string value);
  void ApplyEditorPreferencesToAllTabs();
  editor::TextViewport& ActiveViewport();
  bool SaveActiveTab();
  void RegisterVirtualDocument(const workspace::VirtualDocumentSpec& spec);
  bool OpenVirtualDocument(std::string_view uri);
  bool ExpandSnippetAtCaret(std::string_view snippet_body);

  void PrimeEditorEssentialsIdleSoakSurface();

  workspace::WorkspaceShell& Shell() { return shell_; }

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
  std::vector<SDL_FRect> partial_dirty_rects_;
  app::DirtyRegionAnalysis partial_analysis_;
  std::vector<std::pair<std::filesystem::path, std::uintmax_t>> external_file_restores_;

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
  std::size_t warmup_iterations = 0;
  double tolerance_p50_percent = 100.0;
  double tolerance_p95_percent = 150.0;
  double tolerance_max_percent = 200.0;
  double tolerance_alloc_p50_percent = 10.0;
  double tolerance_alloc_p95_percent = 20.0;
  double tolerance_alloc_max_percent = 50.0;
  double tolerance_rss_percent = 25.0;
  double tolerance_net_heap_percent = 10.0;
  bool gate_cpu_metrics = true;
  bool gate_net_heap_metrics = true;
  std::size_t measurement_revision = 1;
  std::function<void(ScenarioContext&)> run;
};

namespace tolerance {

inline constexpr double kJitterWallP50 = 100.0;
inline constexpr double kJitterWallP95 = 250.0;
inline constexpr double kJitterWallMax = 400.0;

inline constexpr double kExactAllocP50 = 10.0;
inline constexpr double kExactAllocP95 = 20.0;
inline constexpr double kExactAllocMax = 50.0;

}

class PerfHarness {
 public:
  struct RunOptions {
    std::vector<std::string> scenario_names;
    bool smoke_only = false;
    std::size_t iterations = 10;
    std::optional<std::uint64_t> random_seed;
    std::optional<std::string> layout_mode_override;
    bool keep_artifacts = false;
    std::string renderer_driver = "software";
    std::string video_driver = "dummy";
  };

  static void RegisterScenario(const Scenario& scenario);
  static std::vector<Scenario> RegisteredScenarios();
  static std::optional<Aggregate> RunScenario(const Scenario& scenario, const RunOptions& options);
  static std::filesystem::path FixtureManifestPath(const std::filesystem::path& root);
  static bool VerifyFixtureTree(const std::filesystem::path& root, std::string* error);
  static bool VerifyPresentFixtureTrees(const std::filesystem::path& fixtures_root,
                                        std::string* error);
  static std::string LastError();
  static std::string ResolvedRendererDriver();
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

}

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
  }
