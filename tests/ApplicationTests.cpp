#include "TestSupport.h"

#include <filesystem>
#include <string_view>
#include <vector>

#include <SDL3/SDL.h>

#include "app/Application.h"
#include "app/AppStartupOptions.h"
#include "app/ApplicationPresentationCache.h"
#include "app/EventDrainBudget.h"
#include "app/IdleWaitStrategy.h"
#include "app/RedrawTraceAccumulator.h"

namespace microide::tests {

// Narrow seam onto Application's private headless lifecycle (declared a friend in
// src/app/Application.h). Lets the suite drive Initialize()/Render()/Shutdown()
// in-process under the dummy SDL video driver so teardown is verified under ASAN.
struct ApplicationTestAccess {
  static bool Initialize(app::Application& application) { return application.Initialize(); }
  static void Render(app::Application& application, std::vector<SDL_FRect> dirty_rects = {}) {
    application.Render(std::move(dirty_rects));
  }
  static void Shutdown(app::Application& application) { application.Shutdown(); }

  static bool Initialized(const app::Application& application) { return application.initialized_; }
  static SDL_Window* Window(const app::Application& application) { return application.window_; }
  static SDL_Renderer* Renderer(const app::Application& application) {
    return application.renderer_;
  }
  static bool SceneValid(const app::Application& application) {
    return application.scene_texture_.valid();
  }
  static bool FirstRenderComplete(const app::Application& application) {
    return application.first_render_complete_;
  }
  static workspace::WorkspaceShell::EventResult HandleEvent(app::Application& application,
                                                            const SDL_Event& event) {
    return application.HandleEvent(event);
  }
  static void InvalidateScene(app::Application& application) {
    application.scene_texture_.Invalidate();
  }
};

namespace {

using microide::app::CanReuseCachedPresentationState;
using microide::app::ChooseIdleWait;
using microide::app::IdleWaitMode;
using microide::app::RedrawFrameStats;
using microide::app::RedrawTraceAccumulator;
using WindowPresentationState = microide::workspace::WorkspaceShell::WindowPresentationState;
using IdleWaitState = microide::workspace::WorkspaceShell::IdleWaitState;
using IdleHint = microide::workspace::WorkspaceShell::IdleHint;

void TestPresentationCacheReusedWhenUiScaleMatches() {
  const std::optional<WindowPresentationState> cached = WindowPresentationState{
      .logical_width = 1200,
      .logical_height = 800,
      .scale_x = 1.25f,
      .scale_y = 1.25f,
  };
  Expect(CanReuseCachedPresentationState(false, cached, 1.25f, 1.25f),
         "presentation cache should be reusable when the ui scale matches");
}

void TestPresentationCacheInvalidatedWhenUiScaleChanges() {
  const std::optional<WindowPresentationState> cached = WindowPresentationState{
      .logical_width = 1200,
      .logical_height = 800,
      .scale_x = 1.25f,
      .scale_y = 1.25f,
  };
  Expect(!CanReuseCachedPresentationState(false, cached, 1.0f, 1.25f),
         "presentation cache should be bypassed when the ui scale changes without a window event");
}

void TestChooseIdleWaitMapsHints() {
  const auto full = ChooseIdleWait(IdleWaitState{.hint = IdleHint::Full});
  Expect(full.mode == IdleWaitMode::Poll, "Full hint should poll without blocking");

  const auto idle = ChooseIdleWait(IdleWaitState{.hint = IdleHint::Idle});
  Expect(idle.mode == IdleWaitMode::Wait, "Idle hint should block on SDL_WaitEvent");

  const auto caret =
      ChooseIdleWait(IdleWaitState{.hint = IdleHint::CaretOnly, .caret_remaining_ms = 250});
  Expect(caret.mode == IdleWaitMode::WaitTimeout, "CaretOnly hint should wait with a timeout");
  Expect(caret.timeout_ms == 250, "CaretOnly timeout should pass through the remaining ms");

  const auto caret_zero =
      ChooseIdleWait(IdleWaitState{.hint = IdleHint::CaretOnly, .caret_remaining_ms = 0});
  Expect(caret_zero.timeout_ms == 1,
         "CaretOnly should clamp a zero timeout to 1ms so the loop never busy-spins");
}

// The event-drain budget yields to render ONLY once a redraw is pending and the
// budget is reached; with no pending redraw the loop keeps draining. TD-2026-07-17A-100.
void TestEventDrainBudgetYieldsOnlyWithPendingRedraw() {
  using microide::app::ShouldYieldEventDrain;
  using microide::app::kMaxEventsPerDrain;

  // No pending redraw: never yield, no matter how many events processed.
  Expect(!ShouldYieldEventDrain(kMaxEventsPerDrain + 1000, /*redraw_pending=*/false),
         "with no pending redraw the drain never yields (idle input still fully drains)");
  // Pending redraw but under budget: keep draining.
  Expect(!ShouldYieldEventDrain(kMaxEventsPerDrain - 1, /*redraw_pending=*/true),
         "under the budget the drain continues even with a pending redraw");
  // Pending redraw and at/over budget: yield to render.
  Expect(ShouldYieldEventDrain(kMaxEventsPerDrain, /*redraw_pending=*/true),
         "at the budget with a pending redraw the drain yields to render");
  Expect(ShouldYieldEventDrain(kMaxEventsPerDrain + 1, /*redraw_pending=*/true),
         "past the budget with a pending redraw the drain yields to render");
}

void TestRedrawTraceAccumulatorCountsAndFlushes() {
  RedrawTraceAccumulator accumulator;
  accumulator.Configure(/*enabled=*/true, /*verbose=*/false);

  const RedrawFrameStats full_frame{
      .full_redraw = true, .dirty_rect_count = 0, .rendered_clip_count = 0, .reason = "full"};
  const RedrawFrameStats partial_frame{
      .full_redraw = false, .dirty_rect_count = 3, .rendered_clip_count = 2, .reason = "partial"};

  accumulator.Record(full_frame);
  accumulator.Record(partial_frame);
  accumulator.Record(partial_frame);
  Expect(accumulator.frames() == 3, "every recorded frame should be counted");
  Expect(accumulator.full_frames() == 1, "full frames should be tallied separately");
  Expect(accumulator.partial_frames() == 2, "partial frames should be tallied separately");

  // Reaching the log interval flushes and resets the rolling counters.
  for (Uint64 i = accumulator.frames(); i < RedrawTraceAccumulator::kLogInterval; ++i) {
    accumulator.Record(full_frame);
  }
  Expect(accumulator.frames() == 0, "the accumulator should reset after flushing at the interval");
}

void TestRedrawTraceAccumulatorIgnoresFramesWhenDisabled() {
  RedrawTraceAccumulator accumulator;  // disabled by default
  accumulator.Record(RedrawFrameStats{.full_redraw = true, .reason = "full"});
  Expect(accumulator.frames() == 0,
         "a disabled accumulator should not tally frames");
}

// Redirect config/state/session writes into a throwaway tree so the headless
// lifecycle never touches the developer's real user profile. The returned guards
// must outlive the Application under test.
struct HeadlessHomeGuard {
  explicit HeadlessHomeGuard(const std::filesystem::path& root)
      : home(root / "home"),
        config(root / "config"),
        state(root / "state"),
        scoped_home("HOME", (std::filesystem::create_directories(home), home.string())),
        scoped_config("XDG_CONFIG_HOME",
                      (std::filesystem::create_directories(config), config.string())),
        scoped_state("XDG_STATE_HOME",
                     (std::filesystem::create_directories(state), state.string())),
        scoped_localappdata("LOCALAPPDATA", state.string()),
        scoped_appdata("APPDATA", config.string()) {}

  std::filesystem::path home;
  std::filesystem::path config;
  std::filesystem::path state;
  ScopedEnvVar scoped_home;
  ScopedEnvVar scoped_config;
  ScopedEnvVar scoped_state;
  ScopedEnvVar scoped_localappdata;
  ScopedEnvVar scoped_appdata;
};

app::AppStartupOptions HeadlessStartupOptions() {
  app::AppStartupOptions options;
  // Run Initialize()/Shutdown() in-process instead of std::quick_exit()-ing, and
  // keep the plugin runtime out of the headless path.
  options.quick_exit_on_shutdown = false;
  options.disable_plugins = true;
  options.safe_mode = true;
  return options;
}

void TestHeadlessInitializeAndShutdownTearsDownCleanly() {
  EnsureDummySdlVideoInitialized();
  TemporaryDirectory temp_dir;
  HeadlessHomeGuard homes(temp_dir.path());

  app::Application application(HeadlessStartupOptions());
  Expect(!ApplicationTestAccess::Initialized(application),
         "a freshly constructed application should report not initialized");

  Expect(ApplicationTestAccess::Initialize(application),
         "headless Initialize() should succeed under the dummy video driver");
  Expect(ApplicationTestAccess::Initialized(application),
         "Initialize() should mark the application initialized");
  Expect(ApplicationTestAccess::Window(application) != nullptr,
         "Initialize() should create the SDL window");
  Expect(ApplicationTestAccess::Renderer(application) != nullptr,
         "Initialize() should create the SDL renderer");

  ApplicationTestAccess::Shutdown(application);
  Expect(!ApplicationTestAccess::Initialized(application),
         "Shutdown() should clear the initialized flag");
  Expect(ApplicationTestAccess::Window(application) == nullptr,
         "Shutdown() should destroy and null the SDL window");
  Expect(ApplicationTestAccess::Renderer(application) == nullptr,
         "Shutdown() should destroy and null the SDL renderer");

  // Shutdown() must be idempotent: a second call on an already-torn-down app is a
  // no-op rather than a double-free.
  ApplicationTestAccess::Shutdown(application);
  Expect(!ApplicationTestAccess::Initialized(application),
         "a second Shutdown() should remain a clean no-op");
}

void TestInitializeEnablesFocusClickThrough() {
  EnsureDummySdlVideoInitialized();
  TemporaryDirectory temp_dir;
  HeadlessHomeGuard homes(temp_dir.path());

  app::Application application(HeadlessStartupOptions());
  Expect(ApplicationTestAccess::Initialize(application),
         "headless Initialize() should succeed under the dummy video driver");

  // The click that activates an unfocused window must reach the app so a single
  // click both focuses microide and fires the button under the pointer, instead
  // of SDL swallowing the focusing click and forcing a second click.
  const char* hint = SDL_GetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH);
  Expect(hint != nullptr && std::string_view(hint) == "1",
         "Initialize() should enable SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH");

  ApplicationTestAccess::Shutdown(application);
}

void TestHeadlessRendersRetainedSceneFrame() {
  EnsureDummySdlVideoInitialized();
  TemporaryDirectory temp_dir;
  HeadlessHomeGuard homes(temp_dir.path());

  const std::filesystem::path project = temp_dir.path() / "project";
  std::filesystem::create_directories(project);
  app::AppStartupOptions options = HeadlessStartupOptions();
  options.project_path = project;

  app::Application application(options);
  Expect(ApplicationTestAccess::Initialize(application),
         "headless Initialize() with a project should succeed");
  Expect(!ApplicationTestAccess::FirstRenderComplete(application),
         "no frame should be presented before the first Render()");

  // A full frame renders into the retained scene render-target texture (the path
  // the prior deferred note doubted the headless software renderer could drive)
  // and then re-presents it.
  ApplicationTestAccess::Render(application);
  Expect(ApplicationTestAccess::FirstRenderComplete(application),
         "the first Render() should mark the frame presented");
  Expect(ApplicationTestAccess::SceneValid(application),
         "a full headless frame should populate and validate the retained scene texture");

  // A subsequent partial frame replays a dirty clip rect against the retained
  // texture without invalidating it.
  ApplicationTestAccess::Render(application, {SDL_FRect{0.0f, 0.0f, 64.0f, 64.0f}});
  Expect(ApplicationTestAccess::SceneValid(application),
         "a partial headless frame should keep the retained scene texture valid");

  ApplicationTestAccess::Shutdown(application);
  Expect(ApplicationTestAccess::Renderer(application) == nullptr,
         "Shutdown() after rendering should destroy the renderer and scene texture");
}

void TestExposeRePresentsRetainedSceneInsteadOfRerendering() {
  EnsureDummySdlVideoInitialized();
  TemporaryDirectory temp_dir;
  HeadlessHomeGuard homes(temp_dir.path());

  const std::filesystem::path project = temp_dir.path() / "project";
  std::filesystem::create_directories(project);
  app::AppStartupOptions options = HeadlessStartupOptions();
  options.project_path = project;

  app::Application application(options);
  Expect(ApplicationTestAccess::Initialize(application), "headless Initialize() should succeed");
  ApplicationTestAccess::Render(application);
  Expect(ApplicationTestAccess::SceneValid(application),
         "the first frame should populate the retained scene texture");

  // An expose damaged the window, not the scene. Every frame ends by blitting the
  // whole retained scene texture and presenting it, so the repair is a present --
  // re-rendering the workspace would recompute pixel-identical content. On the
  // startup path that mattered: mapping the window fires SHOWN right after the
  // first frame, and answering it with a full redraw rendered that frame twice.
  SDL_Event exposed{};
  exposed.type = SDL_EVENT_WINDOW_EXPOSED;
  const auto exposed_result = ApplicationTestAccess::HandleEvent(application, exposed);
  Expect(exposed_result.handled, "an expose should be handled");
  Expect(!exposed_result.redraw.full,
         "an expose with a valid retained scene should not force a full redraw");
  Expect(exposed_result.redraw.rects.size() == 1,
         "an expose should ask for the minimal damage rect that triggers a present");

  SDL_Event shown{};
  shown.type = SDL_EVENT_WINDOW_SHOWN;
  Expect(!ApplicationTestAccess::HandleEvent(application, shown).redraw.full,
         "window-shown should take the same present-only path as an expose");

  // With nothing retained to present, the expose has to be a real full redraw.
  ApplicationTestAccess::InvalidateScene(application);
  Expect(ApplicationTestAccess::HandleEvent(application, exposed).redraw.full,
         "an expose with no valid retained scene should fall back to a full redraw");

  // A display-scale change alters what the scene should CONTAIN (every glyph is
  // re-rasterized), so it keeps the full redraw even with a valid scene.
  ApplicationTestAccess::Render(application);
  Expect(ApplicationTestAccess::SceneValid(application), "the scene should be valid again");
  SDL_Event scale_changed{};
  scale_changed.type = SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED;
  Expect(ApplicationTestAccess::HandleEvent(application, scale_changed).redraw.full,
         "a display-scale change should still force a full redraw");

  ApplicationTestAccess::Shutdown(application);
}

void TestHeadlessReinitializeAfterShutdown() {
  EnsureDummySdlVideoInitialized();
  TemporaryDirectory temp_dir;
  HeadlessHomeGuard homes(temp_dir.path());

  app::Application application(HeadlessStartupOptions());
  for (int cycle = 0; cycle < 2; ++cycle) {
    Expect(ApplicationTestAccess::Initialize(application),
           "each Initialize() cycle should rebuild the window and renderer");
    Expect(ApplicationTestAccess::Window(application) != nullptr,
           "re-initialization should recreate the SDL window");
    ApplicationTestAccess::Render(application);
    ApplicationTestAccess::Shutdown(application);
    Expect(ApplicationTestAccess::Window(application) == nullptr,
           "each Shutdown() cycle should null the SDL window");
  }
}

void TestHeadlessDestructorShutsDownInitializedApp() {
  EnsureDummySdlVideoInitialized();
  TemporaryDirectory temp_dir;
  HeadlessHomeGuard homes(temp_dir.path());

  // Initialize but never call Shutdown(): ~Application() must run teardown and
  // join the syntax-warmup thread. ASAN/TSAN catch a leaked window/renderer or
  // an unjoined worker here.
  {
    app::Application application(HeadlessStartupOptions());
    Expect(ApplicationTestAccess::Initialize(application),
           "Initialize() should succeed before destructor-driven teardown");
    Expect(ApplicationTestAccess::Window(application) != nullptr,
           "the window should exist before the destructor runs");
  }
}

}  // namespace

void RegisterApplicationTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Application/PresentationCacheReusedWhenUiScaleMatches",
          TestPresentationCacheReusedWhenUiScaleMatches);
  AddTest(tests, "Application/PresentationCacheInvalidatedWhenUiScaleChanges",
          TestPresentationCacheInvalidatedWhenUiScaleChanges);
  AddTest(tests, "Application/ChooseIdleWaitMapsHints", TestChooseIdleWaitMapsHints);
  AddTest(tests, "Application/EventDrainBudgetYieldsOnlyWithPendingRedraw",
          TestEventDrainBudgetYieldsOnlyWithPendingRedraw);
  AddTest(tests, "Application/RedrawTraceAccumulatorCountsAndFlushes",
          TestRedrawTraceAccumulatorCountsAndFlushes);
  AddTest(tests, "Application/RedrawTraceAccumulatorIgnoresFramesWhenDisabled",
          TestRedrawTraceAccumulatorIgnoresFramesWhenDisabled);
  AddTest(tests, "Application/InitializeEnablesFocusClickThrough",
          TestInitializeEnablesFocusClickThrough);
  AddTest(tests, "Application/HeadlessInitializeAndShutdownTearsDownCleanly",
          TestHeadlessInitializeAndShutdownTearsDownCleanly);
  AddTest(tests, "Application/HeadlessRendersRetainedSceneFrame",
          TestHeadlessRendersRetainedSceneFrame);
  AddTest(tests, "Application/ExposeRePresentsRetainedSceneInsteadOfRerendering",
          TestExposeRePresentsRetainedSceneInsteadOfRerendering);
  AddTest(tests, "Application/HeadlessReinitializeAfterShutdown",
          TestHeadlessReinitializeAfterShutdown);
  AddTest(tests, "Application/HeadlessDestructorShutsDownInitializedApp",
          TestHeadlessDestructorShutsDownInitializedApp);
}

}  // namespace microide::tests
