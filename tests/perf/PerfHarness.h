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

#include "workspace/WorkspaceShell.h"

namespace microide::tests::perf {

struct MetricSnapshot {
  double wall_ms = 0.0;
  std::uint64_t allocations = 0;
  std::uint64_t frees = 0;
  std::uint64_t bytes_allocated = 0;
  std::uint64_t bytes_freed = 0;
};

struct MetricSet {
  double p50_wall_ms = 0.0;
  double p95_wall_ms = 0.0;
  double max_wall_ms = 0.0;
  double p50_allocations = 0.0;
  double p95_allocations = 0.0;
  double max_allocations = 0.0;
};

struct Iteration {
  std::size_t index = 0;
  MetricSnapshot metrics;
};

struct Aggregate {
  std::string scenario_name;
  std::vector<Iteration> iterations;
  MetricSet metrics;
  bool smoke = false;
};

class ScenarioContext {
 public:
  ScenarioContext(workspace::WorkspaceShell& shell, SDL_Window* window, SDL_Renderer* renderer);

  void PumpFrames(std::size_t count);
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
  bool AssertNoAllocationsDuringDraw(std::string* error = nullptr);
  std::uint64_t RandomU64();
  void OpenFileFinder();
  void ActivateGitSidebar();
  void StartSearch(std::string_view query);

 private:
  workspace::WorkspaceShell& shell_;
  SDL_Window* window_ = nullptr;
  SDL_Renderer* renderer_ = nullptr;
  std::mt19937_64 rng_;
};

struct Scenario {
  std::string name;
  bool smoke = false;
  std::function<void(ScenarioContext&)> run;
};

class PerfHarness {
 public:
  struct RunOptions {
    std::vector<std::string> scenario_names;
    bool smoke_only = false;
    std::size_t iterations = 10;
    std::optional<std::uint64_t> random_seed;
  };

  static void RegisterScenario(const Scenario& scenario);
  static std::vector<Scenario> RegisteredScenarios();
  static std::optional<Aggregate> RunScenario(const Scenario& scenario, const RunOptions& options);
  static bool VerifyFixtureTree(const std::filesystem::path& root,
                                const std::filesystem::path& expected_hash_file,
                                std::string* error);
  static std::string LastError();

 private:
  struct Driver {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    workspace::WorkspaceShell shell;
    bool initialized = false;
  };

  static bool InitializeDriver(Driver* driver, std::optional<std::uint64_t> random_seed);
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
          .run = FN,                                                                 \
      });                                                                             \
  }  // namespace
