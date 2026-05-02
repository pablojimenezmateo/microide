#include "perf/PerfHarness.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <thread>

#include "perf/AllocationCounter.h"
#include "WorkspaceShellEventHelpers.h"
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
    return true;
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

bool ScenarioContext::Open(const std::filesystem::path& project_root) {
  return workspace::WorkspaceShell::TestAccess::OpenProjectTab(shell_, project_root, false, false);
}

void ScenarioContext::OpenTab(const std::filesystem::path& path) {
  workspace::WorkspaceShell::TestAccess::OpenFile(shell_, path);
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
  while (std::chrono::steady_clock::now() < end) {
    const auto wake = shell_.HandleScheduledWake();
    if (wake.handled) {
      ++wake_count;
      PumpFrames(1);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
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
    workspace::WorkspaceShell::TestAccess::ConsumePluginAsyncProcessCallbacks(shell_);
    const auto wake = shell_.HandleScheduledWake();
    if (wake.handled) {
      PumpFrames(1);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return workspace::WorkspaceShell::TestAccess::DiagnosticsForPath(shell_, path) != nullptr;
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

std::uint64_t ScenarioContext::RandomU64() {
  return rng_();
}

void ScenarioContext::OpenFileFinder() {
  ExecuteCommand("files");
}

void ScenarioContext::ActivateGitSidebar() {
  ExecuteCommand("sidebar-show git");
}

void ScenarioContext::StartSearch(std::string_view query) {
  ExecuteCommand("project-search " + std::string(query));
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
  if (!InitializeDriver(&driver, options.random_seed)) {
    return std::nullopt;
  }

  Aggregate aggregate;
  aggregate.scenario_name = scenario.name;
  aggregate.smoke = scenario.smoke;
  aggregate.iterations.reserve(options.iterations);
  for (std::size_t i = 0; i < options.iterations; ++i) {
    std::cerr << "[perf] scenario=" << scenario.name << " iteration=" << (i + 1) << "/"
              << options.iterations << '\n';
    ScenarioContext context(driver.shell, driver.window, driver.renderer);
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
    });
  }
  aggregate.metrics = AggregateMetrics(aggregate.iterations);
  ShutdownDriver(&driver);
  return aggregate;
}

bool PerfHarness::InitializeDriver(Driver* driver, std::optional<std::uint64_t> random_seed) {
  if (driver == nullptr) {
    return false;
  }

  setenv("SDL_HINT_RENDER_DRIVER", "software", 1);
  const std::string seed_text = std::to_string(ResolveSeed(random_seed));
  setenv("MICROIDE_PERF_SEED", seed_text.c_str(), 1);
  const std::filesystem::path isolated_config = std::filesystem::temp_directory_path() / "microide-perf-config";
  std::error_code mkdir_error;
  std::filesystem::create_directories(isolated_config, mkdir_error);
  setenv("XDG_CONFIG_HOME", isolated_config.string().c_str(), 1);

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
  driver->renderer = SDL_CreateRenderer(driver->window, "software");
  if (driver->renderer == nullptr) {
    HarnessError() = std::string("SDL_CreateRenderer failed: ") + SDL_GetError();
    ShutdownDriver(driver);
    return false;
  }
  SDL_SetRenderVSync(driver->renderer, 0);

  if (!driver->shell.Initialize(std::filesystem::current_path())) {
    HarnessError() = "WorkspaceShell initialize failed";
    ShutdownDriver(driver);
    return false;
  }
  driver->shell.SetDialogWindow(driver->window);
  driver->initialized = true;
  return true;
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
