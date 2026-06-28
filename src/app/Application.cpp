#include "app/Application.h"

#include <SDL3/SDL.h>

#include "workspace/WorkspaceStartupOptions.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include <fstream>
#include <iterator>

#include "app/DirtyRegionPolicy.h"
#include "app/ApplicationPresentationCache.h"
#include "app/IdleWaitStrategy.h"
#include "editor/RuntimeSyntaxRegistry.h"
#include "render/RendererInfo.h"
#include "workspace/ControlSpec.h"
#include "util/StartupTrace.h"
#include "util/PerformanceTrace.h"
#include "util/WindowPresentation.h"

namespace microide::app {

namespace {

constexpr int kInitialWindowWidth = 1440;
constexpr int kInitialWindowHeight = 900;
constexpr std::size_t kRenderPerfPartialClipWarnThreshold = 8;

bool EventUsesRenderCoordinates(Uint32 event_type) {
  switch (event_type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_WHEEL:
      return true;
    default:
      return false;
  }
}

bool CustomWindowChromeEnabled(SDL_Window* window) {
  return window != nullptr && (SDL_GetWindowFlags(window) & SDL_WINDOW_BORDERLESS) != 0;
}

std::optional<workspace::WorkspaceShell::WindowPresentationState> CaptureWindowPresentationState(
    SDL_Window* window,
    SDL_Renderer* renderer,
    float ui_scale) {
  if (window == nullptr || renderer == nullptr) {
    return std::nullopt;
  }

  int pixel_width = 0;
  int pixel_height = 0;
  if (!SDL_GetRenderOutputSize(renderer, &pixel_width, &pixel_height)) {
    SDL_Log("SDL_GetRenderOutputSize failed: %s", SDL_GetError());
    return std::nullopt;
  }
  if (pixel_width <= 0 || pixel_height <= 0) {
    return std::nullopt;
  }

  const util::WindowPresentation presentation =
      util::ComputeWindowPresentation(pixel_width, pixel_height,
                                      SDL_GetWindowDisplayScale(window), ui_scale);
  const SDL_WindowFlags flags = SDL_GetWindowFlags(window);
  return workspace::WorkspaceShell::WindowPresentationState{
      .logical_width = presentation.logical_width,
      .logical_height = presentation.logical_height,
      .scale_x = presentation.presentation_scale_x,
      .scale_y = presentation.presentation_scale_y,
      .chrome =
          workspace::WorkspaceShell::WindowChromeState{
              .custom_enabled = CustomWindowChromeEnabled(window),
              .maximized = (flags & SDL_WINDOW_MAXIMIZED) != 0,
              .fullscreen = (flags & SDL_WINDOW_FULLSCREEN) != 0,
          },
  };
}

void SyncWindowState(SDL_Window* window) {
  if (window == nullptr) {
    return;
  }
  if (!SDL_SyncWindow(window)) {
    SDL_Log("SDL_SyncWindow failed: %s", SDL_GetError());
  }
}

}  // namespace

Application::Application(AppStartupOptions startup_options)
    : startup_options_(std::move(startup_options)) {}

Application::~Application() {
  Shutdown();
  // Safety net: join the warmup thread even if Shutdown() early-returned
  // because Initialize() failed mid-startup.
  if (syntax_registry_warmup_.joinable()) {
    syntax_registry_warmup_.join();
  }
}

int Application::Run() {
  if (!Initialize()) {
    return 1;
  }

  running_ = true;
  bool full_redraw_pending = true;
  bool window_shown = false;
  std::vector<SDL_FRect> dirty_rects;
  const char* redraw_reason = "startup";

  while (running_) {
    if (full_redraw_pending || !dirty_rects.empty()) {
      Render(full_redraw_pending ? std::vector<SDL_FRect>{} : dirty_rects, redraw_reason);
      if (workspace_shell_.ConsumePostRenderFullRedrawRequest()) {
        full_redraw_pending = true;
        dirty_rects.clear();
        redraw_reason = "render-settle";
        continue;
      }
      full_redraw_pending = false;
      dirty_rects.clear();
      redraw_reason = "event";

      // After a frame settles, ask the background worker to tokenize a band
      // around the (possibly newly scrolled) editor viewport so later frames
      // hit the highlight cache instead of tokenizing on the render path.
      workspace_shell_.RequestActiveHighlightPrefetch();

      // Map the window exactly once, after the first real frame is painted. The
      // window was created hidden, so this is its first and only map: it appears
      // already borderless and already drawn, with no black flash or remap. The
      // present above may be dropped while hidden, so re-blit the retained scene
      // texture to the freshly-mapped window (a cheap texture copy, not a second
      // full render).
      if (!window_shown && first_render_complete_ && window_ != nullptr) {
        SDL_ShowWindow(window_);
        if (scene_texture_.valid()) {
          SDL_RenderTexture(renderer_, scene_texture_.texture(), nullptr, nullptr);
          SDL_RenderPresent(renderer_);
        }
        window_shown = true;
      }
    }

    SDL_Event event;
    const workspace::WorkspaceShell::IdleWaitState idle_wait_state =
        workspace_shell_.CurrentIdleWaitState();
    const IdleWaitDecision idle_wait = ChooseIdleWait(idle_wait_state);
    bool has_event = false;
    switch (idle_wait.mode) {
      case IdleWaitMode::Poll:
        has_event = SDL_PollEvent(&event);
        break;
      case IdleWaitMode::WaitTimeout:
        has_event = SDL_WaitEventTimeout(&event, idle_wait.timeout_ms);
        break;
      case IdleWaitMode::Wait:
        has_event = SDL_WaitEvent(&event);
        break;
    }
    if (!has_event) {
      if (idle_wait.mode == IdleWaitMode::Wait) {
        SDL_Log("SDL_WaitEvent failed: %s", SDL_GetError());
        break;
      }
      const auto scheduled = workspace_shell_.HandleScheduledWake();
      if (scheduled.handled) {
        if (scheduled.redraw.full) {
          full_redraw_pending = true;
          dirty_rects.clear();
          redraw_reason = "scheduled-full";
        } else {
          full_redraw_pending = false;
          dirty_rects = scheduled.redraw.rects;
          redraw_reason = "scheduled-partial";
        }
      }
      continue;
    }

    do {
      // Coalesce consecutive mouse-motion events to the latest position.
      // High-poll-rate mice can fire many motion events per frame; processing
      // each one runs the full hover/cursor/selection pipeline only to be
      // obsoleted by the next motion. We only collapse when the next event in
      // the queue is also a motion (same window) to preserve ordering relative
      // to keyboard/button events. No handler reads motion.xrel/yrel.
      if (event.type == SDL_EVENT_MOUSE_MOTION) {
        SDL_Event next;
        while (SDL_PeepEvents(&next, 1, SDL_PEEKEVENT, SDL_EVENT_FIRST,
                              SDL_EVENT_LAST) > 0 &&
               next.type == SDL_EVENT_MOUSE_MOTION &&
               next.motion.windowID == event.motion.windowID) {
          SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_EVENT_MOUSE_MOTION,
                         SDL_EVENT_MOUSE_MOTION);
        }
      }
      const auto result = HandleEvent(event);
      if (result.handled) {
        if (result.redraw.full) {
          full_redraw_pending = true;
          dirty_rects.clear();
        } else if (!full_redraw_pending) {
          dirty_rects.insert(dirty_rects.end(), result.redraw.rects.begin(), result.redraw.rects.end());
        }
        redraw_reason = result.redraw.full ? "event-full" : "event-partial";
      }
    } while (SDL_PollEvent(&event));
  }

  return 0;
}

bool Application::Initialize() {
  if (initialized_) {
    return true;
  }

  util::StartupTrace::Reset();
  util::StartupTrace::Scope trace_scope("Application::Initialize");

  {
    util::StartupTrace::Scope sdl_init_scope("SDL_Init");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
      SDL_Log("SDL_Init failed: %s", SDL_GetError());
      return false;
    }
  }

  // Create the window hidden so all chrome setup (borderless, hit test) happens
  // before it is ever mapped, and so the first frame the user sees is the real,
  // fully-painted UI. Showing it later (after the first render, in Run()) maps
  // the window exactly once and avoids the black-flash + borderless-remap
  // "double popup" that toggling decorations on an already-mapped window caused.
  const SDL_WindowFlags window_flags =
      SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN;

  {
    util::StartupTrace::Scope create_window_scope("SDL_CreateWindow");
    window_ = SDL_CreateWindow(
        "microide",
        kInitialWindowWidth,
        kInitialWindowHeight,
        window_flags);
  }
  if (window_ == nullptr) {
    SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
    return false;
  }

  {
    util::StartupTrace::Scope create_renderer_scope("SDL_CreateRenderer");
    renderer_ = SDL_CreateRenderer(window_, nullptr);
  }
  if (renderer_ == nullptr) {
    SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
    return false;
  }

  SDL_SetRenderVSync(renderer_, 1);

  // Record + report which SDL backend we actually got. The batched-text path is
  // GPU-only (it regresses on the software renderer), so this gate is what makes
  // that path safe to enable, and the log line confirms GPU vs software at
  // runtime / in CI without a profiler.
  {
    const std::string_view driver = render::RendererDriverName(renderer_);
    const bool is_gpu = render::RendererIsGpu(renderer_);
    SDL_Log("microide render: SDL renderer driver='%.*s' (gpu=%s)",
            static_cast<int>(driver.size()), driver.data(), is_gpu ? "yes" : "no");
    workspace_shell_.SetRenderBackendInfo(std::string(driver), is_gpu);
  }

  // Warm up the syntax-highlight registry on a background thread so its
  // ~30ms parse cost overlaps with the WorkspaceShell construction below.
  // Magic-static init in MutableRegistry() synchronizes against the first
  // foreground access, so the worker can race safely; whichever thread arrives
  // second blocks on the same init barrier.
  syntax_registry_warmup_ = std::thread([]() {
    editor::runtime_syntax::EnsureInitialized();
  });

  {
    // Parse a cold-start control spec (if any) up front: its `project` selects
    // the project to open, overriding the positional path / cwd.
    workspace::ControlSpec control_spec;
    if (startup_options_.control_spec_path.has_value()) {
      std::ifstream in(*startup_options_.control_spec_path);
      if (in) {
        const std::string json((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        control_spec = workspace::ParseControlSpec(json);
        if (!control_spec.valid) {
          SDL_Log("control spec parse error: %s", control_spec.parse_error.c_str());
        }
      } else {
        SDL_Log("could not read control spec: %s",
                startup_options_.control_spec_path->string().c_str());
      }
    }

    // The explicit project (spec `project` wins over the positional path) makes
    // the named project win over a saved session via skip-restore; leave it unset
    // for the cwd fallback so a bare launch still restores the previous session.
    std::optional<std::filesystem::path> explicit_project;
    if (control_spec.valid && control_spec.project.has_value()) {
      explicit_project = *control_spec.project;
    } else if (startup_options_.project_path.has_value()) {
      explicit_project = startup_options_.project_path;
    }

    workspace::WorkspaceStartupOptions shell_startup;
    shell_startup.disable_plugins = startup_options_.disable_plugins;
    shell_startup.safe_mode = startup_options_.safe_mode;
    shell_startup.project_path = explicit_project;
    shell_startup.control_stdout = startup_options_.control_stdout;
    shell_startup.setting_overrides = startup_options_.setting_overrides;
    workspace_shell_.SetStartupOptions(std::move(shell_startup));

    std::filesystem::path initial_project;
    if (explicit_project.has_value()) {
      initial_project = *explicit_project;
    } else if (!startup_options_.safe_mode) {
      initial_project = std::filesystem::current_path();
    }

    util::StartupTrace::Scope workspace_init_scope("WorkspaceShell::Initialize");
    if (!workspace_shell_.Initialize(initial_project)) {
      SDL_Log("Workspace initialization failed");
      return false;
    }

    // Force-start the channel first (so the `ready` handshake leads the JSONL
    // stream), then apply transient `--set` overrides, then the cold-start spec.
    workspace_shell_.ForceStartControlChannel();
    workspace_shell_.ApplyStartupSettingOverrides();
    if (control_spec.valid) {
      util::StartupTrace::Scope apply_spec_scope("WorkspaceShell::ApplyControlSpec");
      workspace_shell_.ApplyControlSpec(control_spec);
    }
  }
  workspace_shell_.SetDialogWindow(window_);

  {
    util::StartupTrace::Scope window_chrome_scope("WindowChromeSetup");
    if (!SDL_SetWindowBordered(window_, false)) {
      SDL_Log("SDL_SetWindowBordered(false) failed: %s", SDL_GetError());
    } else if (!SDL_SetWindowHitTest(window_, &Application::WindowHitTestCallback, this)) {
      SDL_Log("SDL_SetWindowHitTest failed: %s", SDL_GetError());
      SDL_SetWindowBordered(window_, true);
    }
    SyncWindowState(window_);
  }

  {
    util::StartupTrace::Scope presentation_scope("UpdateRendererPresentation");
    UpdateRendererPresentation();
  }

  {
    util::StartupTrace::Scope text_input_scope("SDL_StartTextInput");
    if (!SDL_StartTextInput(window_)) {
      SDL_Log("SDL_StartTextInput failed: %s", SDL_GetError());
    }
  }

  initialized_ = true;
  first_render_complete_ = false;
  redraw_trace_.Configure(util::PerformanceTrace::FlagEnabled("MICROIDE_TRACE_REDRAW"),
                          util::PerformanceTrace::FlagEnabled("MICROIDE_TRACE_REDRAW_VERBOSE"));
  return true;
}

void Application::Shutdown() {
  if (!initialized_) {
    return;
  }

  if (syntax_registry_warmup_.joinable()) {
    syntax_registry_warmup_.join();
  }

  workspace_shell_.SetDialogWindow(nullptr);

  // Destroy the window and renderer immediately so the app disappears from the
  // screen before any blocking workspace shutdown work (persisting state, waiting
  // for terminal processes to exit, etc.).
  if (window_ != nullptr) {
    SDL_StopTextInput(window_);
  }
  scene_texture_.Destroy();
  if (renderer_ != nullptr) {
    SDL_DestroyRenderer(renderer_);
    renderer_ = nullptr;
  }
  if (window_ != nullptr) {
    SDL_DestroyWindow(window_);
    window_ = nullptr;
  }

  workspace_shell_.Shutdown();

  // Reset lifecycle state so the destructor and any second Shutdown() are clean
  // no-ops, and so an in-process Initialize()/Shutdown() cycle (headless tests)
  // rebuilds presentation state instead of trusting the prior renderer's cache.
  initialized_ = false;
  first_render_complete_ = false;
  presentation_state_dirty_ = true;

  // All user state has been saved. Exit immediately rather than waiting for
  // destructor chains (terminal sessions, background thread joins, etc.).
  // The OS reclaims all child processes and resources. Tests disable this so
  // they can drive Initialize()/Shutdown() and verify clean teardown under
  // sanitizers.
  if (startup_options_.quick_exit_on_shutdown) {
    std::quick_exit(0);
  }
}

workspace::WorkspaceShell::EventResult Application::HandleEvent(const SDL_Event& event) {
  switch (event.type) {
    case SDL_EVENT_QUIT:
      workspace_shell_.RequestQuit();
      if (workspace_shell_.ConsumeQuitRequested()) {
        running_ = false;
      }
      return workspace::WorkspaceShell::EventResult{
          .handled = true,
          .redraw = {},
      };
    case SDL_EVENT_WINDOW_SHOWN:
    case SDL_EVENT_WINDOW_EXPOSED:
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
      presentation_state_dirty_ = true;
      UpdateRendererPresentation();
      return workspace::WorkspaceShell::EventResult{
          .handled = true,
          .redraw = workspace::WorkspaceShell::RenderInvalidation{
              .full = true,
              .rects = {},
          },
      };
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
      presentation_state_dirty_ = true;
      scene_texture_.NoteResizeEvent(SDL_GetTicksNS());
      UpdateRendererPresentation();
      // A compositor-driven resize can leave the displayed cursor stale (e.g. the
      // resize cursor lingering over a border); force the next update to re-apply.
      workspace_shell_.ForceCursorReassert();
      return workspace::WorkspaceShell::EventResult{
          .handled = true,
          .redraw = workspace::WorkspaceShell::RenderInvalidation{
              .full = true,
              .rects = {},
          },
      };
    default:
      break;
  }

  SDL_Event converted_event = event;
  if (renderer_ != nullptr && EventUsesRenderCoordinates(event.type)) {
    SDL_ConvertEventToRenderCoordinates(renderer_, &converted_event);
  }

  const auto result = workspace_shell_.HandleEvent(converted_event);
  ConsumeWindowActions();
  if (workspace_shell_.ConsumeQuitRequested()) {
    running_ = false;
    return workspace::WorkspaceShell::EventResult{
        .handled = true,
        .redraw = result.redraw,
    };
  }
  return result;
}

void Application::Render(std::vector<SDL_FRect> dirty_rects, const char* reason) {
  if (renderer_ == nullptr) {
    return;
  }

  const bool full_redraw_requested = dirty_rects.empty() || !scene_texture_.valid();
  util::PerformanceTrace::Scope trace_scope(
      full_redraw_requested ? "Application::Render(full)" : "Application::Render(partial)");
  std::optional<util::StartupTrace::Scope> first_render_scope;
  if (!first_render_complete_ && util::StartupTrace::Enabled()) {
    first_render_scope.emplace("Application::FirstRender");
  }

  int width = 0;
  int height = 0;
  {
    util::PerformanceTrace::Scope presentation_scope("Application::UpdateRendererPresentation");
    if (!UpdateRendererPresentation(&width, &height)) {
      return;
    }
  }

  const std::size_t dirty_rect_count = dirty_rects.size();
  const render::TextClipPadding clip_padding = workspace_shell_.PartialRedrawClipPadding();
  const DirtyRegionAnalysis dirty_region_analysis =
      AnalyzeDirtyRegions(dirty_rects, clip_padding, width, height);
  const std::size_t merged_clip_count = dirty_region_analysis.merged_clip_rects.size();
  const float dirty_coverage = dirty_region_analysis.coverage;
  const bool promote_partial_to_full =
      scene_texture_.valid() && ShouldPromotePartialFrameToFull(dirty_region_analysis);
  const bool full_redraw = dirty_rects.empty() || !scene_texture_.valid() || promote_partial_to_full;
  const Uint64 render_start = SDL_GetTicksNS();
  std::size_t rendered_clip_count = 0;
  workspace::WorkspaceShell::FrameToken frame_token;
  {
    util::PerformanceTrace::Scope prepare_scope("Application::WorkspacePrepareFrame");
    frame_token = workspace_shell_.PrepareFrameOnce(renderer_, width, height);
  }
#if defined(__SANITIZE_ADDRESS__)
  SDL_assert(frame_token.valid() &&
             "Application::WorkspaceRenderClip requires PrepareFrameOnce() in current frame");
#endif
  bool scene_texture_ready = false;
  {
    util::PerformanceTrace::Scope scene_texture_scope("Application::EnsureSceneTexture");
    scene_texture_ready = scene_texture_.Ensure(renderer_, width, height);
  }
  if (scene_texture_ready) {
    if (!SDL_SetRenderTarget(renderer_, scene_texture_.texture())) {
      SDL_Log("SDL_SetRenderTarget(scene texture) failed: %s", SDL_GetError());
      scene_texture_.Destroy();
      {
        util::PerformanceTrace::Scope fallback_scope("Application::WorkspaceRender(fallback-full)");
        workspace_shell_.RenderClip(frame_token, renderer_, width, height);
      }
      SDL_RenderPresent(renderer_);
      redraw_trace_.Record(RedrawFrameStats{.full_redraw_requested = true,
                                            .full_redraw = true,
                                            .promoted_partial_to_full = false,
                                            .dirty_rect_count = 0,
                                            .rendered_clip_count = 0,
                                            .reason = "fallback-full",
                                            .elapsed_ns = SDL_GetTicksNS() - render_start});
      first_render_complete_ = true;
      workspace_shell_.OnFramePresented();
      return;
    }

    if (full_redraw) {
      if (promote_partial_to_full && util::PerformanceTrace::Enabled()) {
        SDL_Log(
            "microide perf: promoting partial frame to full redraw (%zu dirty rects, %zu coalesced clip rects, %.1f%% coalesced coverage)",
            dirty_rect_count, merged_clip_count, dirty_coverage * 100.0f);
      }
      util::PerformanceTrace::Scope workspace_scope("Application::WorkspaceRender(full)");
      workspace_shell_.RenderClip(frame_token, renderer_, width, height);
    } else {
      bool rendered_partial = false;
      // Only materialize the descriptive label when tracing is active; building
      // it every partial frame would heap-allocate on the render hot path.
      std::string partial_loop_label;
      std::string_view partial_loop_label_view = "Application::WorkspaceRender(partial-loop)";
      if (util::PerformanceTrace::Enabled()) {
        partial_loop_label =
            "Application::WorkspaceRender(partial-loop " + std::to_string(merged_clip_count) +
            " coalesced clip rects from " + std::to_string(dirty_rect_count) + " dirty rects)";
        partial_loop_label_view = partial_loop_label;
      }
      util::PerformanceTrace::Scope partial_loop_scope(partial_loop_label_view);
      for (const SDL_Rect& clip_rect : dirty_region_analysis.merged_clip_rects) {
        SDL_SetRenderClipRect(renderer_, &clip_rect);
        util::PerformanceTrace::Scope partial_scope(
            "Application::WorkspaceRender(partial-clip)");
        const SDL_FRect dirty_rect_hint{
            static_cast<float>(clip_rect.x), static_cast<float>(clip_rect.y),
            static_cast<float>(clip_rect.w), static_cast<float>(clip_rect.h)};
        workspace_shell_.RenderClip(frame_token, renderer_, width, height, dirty_rect_hint);
        rendered_partial = true;
        ++rendered_clip_count;
      }
      if (!rendered_partial) {
        util::PerformanceTrace::Scope partial_fallback_scope(
            "Application::WorkspaceRender(partial-fallback-full)");
        workspace_shell_.RenderClip(frame_token, renderer_, width, height);
        dirty_rects.clear();
      }
    }

    SDL_SetRenderClipRect(renderer_, nullptr);
    SDL_SetRenderTarget(renderer_, nullptr);
    {
      util::PerformanceTrace::Scope present_scope("Application::PresentRetainedScene");
      SDL_RenderTexture(renderer_, scene_texture_.texture(), nullptr, nullptr);
      SDL_RenderPresent(renderer_);
    }
    scene_texture_.MarkValid();
    if (!full_redraw && util::PerformanceTrace::Enabled() &&
        rendered_clip_count >= kRenderPerfPartialClipWarnThreshold) {
      SDL_Log(
          "microide perf: partial frame replayed %zu coalesced clip rects from %zu dirty rects",
          rendered_clip_count, dirty_rect_count);
    }
    redraw_trace_.Record(RedrawFrameStats{.full_redraw_requested = full_redraw_requested,
                                          .full_redraw = full_redraw,
                                          .promoted_partial_to_full = promote_partial_to_full,
                                          .dirty_rect_count = dirty_rect_count,
                                          .rendered_clip_count = rendered_clip_count,
                                          .reason = reason,
                                          .elapsed_ns = SDL_GetTicksNS() - render_start});
  } else {
    util::PerformanceTrace::Scope fallback_scope("Application::WorkspaceRender(fallback-full)");
    workspace_shell_.RenderClip(frame_token, renderer_, width, height);
    SDL_RenderPresent(renderer_);
    redraw_trace_.Record(RedrawFrameStats{.full_redraw_requested = true,
                                          .full_redraw = true,
                                          .promoted_partial_to_full = false,
                                          .dirty_rect_count = 0,
                                          .rendered_clip_count = 0,
                                          .reason = "fallback-full",
                                          .elapsed_ns = SDL_GetTicksNS() - render_start});
  }
  first_render_complete_ = true;
  workspace_shell_.OnFramePresented();
}

bool Application::UpdateRendererPresentation(int* logical_width, int* logical_height) {
  if (window_ == nullptr || renderer_ == nullptr) {
    return false;
  }

  // Fast path: when no window event has invalidated the cached presentation,
  // skip the SDL_GetRenderOutputSize / SDL_GetWindowDisplayScale queries plus
  // SDL_SetRenderLogicalPresentation reapply. They are individually cheap but
  // run every frame of the render loop, so caching saves measurable time on
  // steady-state full-redraw frames.
  const float current_ui_scale = workspace_shell_.UiScale();
  const auto cached = workspace_shell_.CurrentWindowPresentationState();
  if (CanReuseCachedPresentationState(presentation_state_dirty_, cached, last_presented_ui_scale_,
                                      current_ui_scale)) {
    if (logical_width != nullptr) {
      *logical_width = cached->logical_width;
    }
    if (logical_height != nullptr) {
      *logical_height = cached->logical_height;
    }
    return true;
  }

  const auto presentation = CaptureWindowPresentationState(window_, renderer_, current_ui_scale);
  if (!presentation.has_value()) {
    return false;
  }

  workspace_shell_.SetWindowPresentationState(*presentation);
  if (!SDL_SetRenderLogicalPresentation(renderer_, presentation->logical_width,
                                        presentation->logical_height,
                                        SDL_LOGICAL_PRESENTATION_STRETCH)) {
    SDL_Log("SDL_SetRenderLogicalPresentation failed: %s", SDL_GetError());
    return false;
  }

  presentation_state_dirty_ = false;
  last_presented_ui_scale_ = current_ui_scale;
  if (logical_width != nullptr) {
    *logical_width = presentation->logical_width;
  }
  if (logical_height != nullptr) {
    *logical_height = presentation->logical_height;
  }
  return true;
}

void Application::ConsumeWindowActions() {
  if (window_ == nullptr) {
    return;
  }

  switch (workspace_shell_.ConsumeWindowAction()) {
    case workspace::WorkspaceShell::WindowAction::None:
      return;
    case workspace::WorkspaceShell::WindowAction::Minimize:
      SDL_MinimizeWindow(window_);
      return;
    case workspace::WorkspaceShell::WindowAction::ToggleMaximize: {
      const SDL_WindowFlags flags = SDL_GetWindowFlags(window_);
      if ((flags & SDL_WINDOW_MAXIMIZED) != 0) {
        SDL_RestoreWindow(window_);
      } else {
        SDL_MaximizeWindow(window_);
      }
      SyncWindowState(window_);
      UpdateRendererPresentation();
      return;
    }
    case workspace::WorkspaceShell::WindowAction::ToggleFullscreen: {
      const SDL_WindowFlags flags = SDL_GetWindowFlags(window_);
      SDL_SetWindowFullscreen(window_, (flags & SDL_WINDOW_FULLSCREEN) == 0);
      SyncWindowState(window_);
      UpdateRendererPresentation();
      return;
    }
  }
}

SDL_HitTestResult Application::WindowHitTest(const SDL_Point& area) const {
  if (renderer_ == nullptr) {
    return SDL_HITTEST_NORMAL;
  }

  float render_x = static_cast<float>(area.x);
  float render_y = static_cast<float>(area.y);
  if (!SDL_RenderCoordinatesFromWindow(renderer_, static_cast<float>(area.x),
                                       static_cast<float>(area.y), &render_x, &render_y)) {
    return SDL_HITTEST_NORMAL;
  }

  return workspace_shell_.WindowHitTest(render_x, render_y);
}

SDL_HitTestResult SDLCALL Application::WindowHitTestCallback(SDL_Window* window,
                                                             const SDL_Point* area,
                                                             void* data) {
  (void) window;
  if (area == nullptr || data == nullptr) {
    return SDL_HITTEST_NORMAL;
  }
  return static_cast<Application*>(data)->WindowHitTest(*area);
}

}  // namespace microide::app
