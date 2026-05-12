#include "app/Application.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "app/DirtyRegionPolicy.h"
#include "util/StartupTrace.h"
#include "util/PerformanceTrace.h"
#include "util/WindowPresentation.h"

namespace microide::app {

namespace {

constexpr int kInitialWindowWidth = 1440;
constexpr int kInitialWindowHeight = 900;
constexpr Uint64 kRenderTraceLogInterval = 120;
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

Application::~Application() {
  Shutdown();
}

int Application::Run() {
  if (!Initialize()) {
    return 1;
  }

  running_ = true;
  bool full_redraw_pending = true;
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
    }

    SDL_Event event;
    const workspace::WorkspaceShell::IdleWaitState idle_wait_state =
        workspace_shell_.CurrentIdleWaitState();
    bool has_event = false;
    switch (idle_wait_state.hint) {
      case workspace::WorkspaceShell::IdleHint::Full:
        has_event = SDL_PollEvent(&event);
        break;
      case workspace::WorkspaceShell::IdleHint::CaretOnly:
        has_event = SDL_WaitEventTimeout(
            &event, static_cast<Sint32>(std::max<Uint32>(1, idle_wait_state.caret_remaining_ms)));
        break;
      case workspace::WorkspaceShell::IdleHint::Idle:
        has_event = SDL_WaitEvent(&event);
        break;
    }
    if (!has_event) {
      if (idle_wait_state.hint == workspace::WorkspaceShell::IdleHint::Idle) {
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

  const SDL_WindowFlags window_flags =
      SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;

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

  // Present a blank frame before the (potentially slow) workspace initialization
  // so the compositor gets a committed frame immediately. This satisfies the
  // desktop startup-notification protocol (xdg-activation / _NET_STARTUP_INFO)
  // and dismisses the launcher busy indicator without waiting for the full init.
  SDL_RenderClear(renderer_);
  SDL_RenderPresent(renderer_);

  {
    util::StartupTrace::Scope workspace_init_scope("WorkspaceShell::Initialize");
    if (!workspace_shell_.Initialize(std::filesystem::current_path())) {
      SDL_Log("Workspace initialization failed");
      return false;
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
  redraw_trace_enabled_ = util::PerformanceTrace::FlagEnabled("MICROIDE_TRACE_REDRAW");
  redraw_trace_verbose_enabled_ =
      util::PerformanceTrace::FlagEnabled("MICROIDE_TRACE_REDRAW_VERBOSE");
  return true;
}

void Application::Shutdown() {
  if (!initialized_) {
    return;
  }

  workspace_shell_.SetDialogWindow(nullptr);

  // Destroy the window and renderer immediately so the app disappears from the
  // screen before any blocking workspace shutdown work (persisting state, waiting
  // for terminal processes to exit, etc.).
  if (window_ != nullptr) {
    SDL_StopTextInput(window_);
  }
  DestroySceneTexture();
  if (renderer_ != nullptr) {
    SDL_DestroyRenderer(renderer_);
    renderer_ = nullptr;
  }
  if (window_ != nullptr) {
    SDL_DestroyWindow(window_);
    window_ = nullptr;
  }

  workspace_shell_.Shutdown();

  // All user state has been saved. Exit immediately rather than waiting for
  // destructor chains (terminal sessions, background thread joins, etc.).
  // The OS reclaims all child processes and resources.
  std::quick_exit(0);
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
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
      UpdateRendererPresentation();
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

  const bool full_redraw_requested = dirty_rects.empty() || !scene_texture_valid_;
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
      scene_texture_valid_ && ShouldPromotePartialFrameToFull(dirty_region_analysis);
  const bool full_redraw = dirty_rects.empty() || !scene_texture_valid_ || promote_partial_to_full;
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
    scene_texture_ready = EnsureSceneTexture(width, height);
  }
  if (scene_texture_ready) {
    if (!SDL_SetRenderTarget(renderer_, scene_texture_)) {
      SDL_Log("SDL_SetRenderTarget(scene texture) failed: %s", SDL_GetError());
      DestroySceneTexture();
      {
        util::PerformanceTrace::Scope fallback_scope("Application::WorkspaceRender(fallback-full)");
        workspace_shell_.RenderClip(frame_token, renderer_, width, height);
      }
      SDL_RenderPresent(renderer_);
      RecordRenderStats(true, true, false, 0, 0, "fallback-full", SDL_GetTicksNS() - render_start);
      first_render_complete_ = true;
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
      const std::string partial_loop_label =
          "Application::WorkspaceRender(partial-loop " + std::to_string(merged_clip_count) +
          " coalesced clip rects from " + std::to_string(dirty_rect_count) +
          " dirty rects)";
      util::PerformanceTrace::Scope partial_loop_scope(partial_loop_label);
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
      SDL_RenderTexture(renderer_, scene_texture_, nullptr, nullptr);
      SDL_RenderPresent(renderer_);
    }
    scene_texture_valid_ = true;
    if (!full_redraw && util::PerformanceTrace::Enabled() &&
        rendered_clip_count >= kRenderPerfPartialClipWarnThreshold) {
      SDL_Log(
          "microide perf: partial frame replayed %zu coalesced clip rects from %zu dirty rects",
          rendered_clip_count, dirty_rect_count);
    }
    RecordRenderStats(full_redraw_requested, full_redraw, promote_partial_to_full, dirty_rect_count,
                      rendered_clip_count, reason, SDL_GetTicksNS() - render_start);
  } else {
    util::PerformanceTrace::Scope fallback_scope("Application::WorkspaceRender(fallback-full)");
    workspace_shell_.RenderClip(frame_token, renderer_, width, height);
    SDL_RenderPresent(renderer_);
    RecordRenderStats(true, true, false, 0, 0, "fallback-full", SDL_GetTicksNS() - render_start);
  }
  first_render_complete_ = true;
}

bool Application::UpdateRendererPresentation(int* logical_width, int* logical_height) {
  if (window_ == nullptr || renderer_ == nullptr) {
    return false;
  }

  const auto presentation =
      CaptureWindowPresentationState(window_, renderer_, workspace_shell_.UiScale());
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

  if (logical_width != nullptr) {
    *logical_width = presentation->logical_width;
  }
  if (logical_height != nullptr) {
    *logical_height = presentation->logical_height;
  }
  return true;
}

bool Application::EnsureSceneTexture(int logical_width, int logical_height) {
  if (renderer_ == nullptr || logical_width <= 0 || logical_height <= 0) {
    return false;
  }

  int output_width = 0;
  int output_height = 0;
  if (!SDL_GetRenderOutputSize(renderer_, &output_width, &output_height) ||
      output_width <= 0 || output_height <= 0) {
    DestroySceneTexture();
    return false;
  }
  if (output_width != logical_width || output_height != logical_height) {
    DestroySceneTexture();
    return false;
  }

  if (scene_texture_ != nullptr && scene_texture_width_ == logical_width &&
      scene_texture_height_ == logical_height) {
    return true;
  }

  DestroySceneTexture();
  scene_texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA8888,
                                     SDL_TEXTUREACCESS_TARGET, logical_width, logical_height);
  if (scene_texture_ == nullptr) {
    return false;
  }

  SDL_SetTextureScaleMode(scene_texture_, SDL_SCALEMODE_NEAREST);
  scene_texture_width_ = logical_width;
  scene_texture_height_ = logical_height;
  scene_texture_valid_ = false;
  return true;
}

void Application::DestroySceneTexture() {
  if (scene_texture_ != nullptr) {
    SDL_DestroyTexture(scene_texture_);
    scene_texture_ = nullptr;
  }
  scene_texture_width_ = 0;
  scene_texture_height_ = 0;
  scene_texture_valid_ = false;
}

void Application::RecordRenderStats(bool full_redraw_requested,
                                    bool full_redraw,
                                    bool promoted_partial_to_full,
                                    std::size_t dirty_rect_count,
                                    std::size_t rendered_clip_count,
                                    const char* reason,
                                    Uint64 elapsed_ns) {
  if (redraw_trace_verbose_enabled_) {
    SDL_Log(
        "microide redraw frame: reason=%s requested=%s rendered=%s promoted=%s dirty=%zu clips=%zu elapsed=%.2f ms",
        reason != nullptr ? reason : "unknown", full_redraw_requested ? "full" : "partial",
        full_redraw ? "full" : "partial", promoted_partial_to_full ? "yes" : "no",
        dirty_rect_count, rendered_clip_count, static_cast<double>(elapsed_ns) / 1'000'000.0);
  }

  if (!redraw_trace_enabled_) {
    return;
  }

  ++redraw_trace_frames_;
  redraw_trace_total_ns_ += elapsed_ns;
  redraw_trace_total_dirty_rects_ += dirty_rect_count;
  redraw_trace_total_rendered_clips_ += rendered_clip_count;
  redraw_trace_max_dirty_rects_ = std::max(redraw_trace_max_dirty_rects_, dirty_rect_count);
  redraw_trace_max_rendered_clips_ =
      std::max(redraw_trace_max_rendered_clips_, rendered_clip_count);
  if (full_redraw || dirty_rect_count == 0) {
    ++redraw_trace_full_frames_;
  } else {
    ++redraw_trace_partial_frames_;
  }

  if (redraw_trace_frames_ < kRenderTraceLogInterval) {
    return;
  }

  const double average_ms = static_cast<double>(redraw_trace_total_ns_) /
                            static_cast<double>(redraw_trace_frames_) / 1'000'000.0;
  const double average_dirty_rects =
      redraw_trace_frames_ == 0
          ? 0.0
          : static_cast<double>(redraw_trace_total_dirty_rects_) /
                static_cast<double>(redraw_trace_frames_);
  const double average_rendered_clips =
      redraw_trace_partial_frames_ == 0
          ? 0.0
          : static_cast<double>(redraw_trace_total_rendered_clips_) /
                static_cast<double>(redraw_trace_partial_frames_);
  SDL_Log(
      "microide redraw: %llu frames | %llu full | %llu partial | avg %.2f ms | avg dirty %.2f "
      "| avg clips %.2f | max dirty %zu | max clips %zu | last=%s",
          static_cast<unsigned long long>(redraw_trace_frames_),
          static_cast<unsigned long long>(redraw_trace_full_frames_),
          static_cast<unsigned long long>(redraw_trace_partial_frames_), average_ms,
          average_dirty_rects, average_rendered_clips, redraw_trace_max_dirty_rects_,
          redraw_trace_max_rendered_clips_,
          reason != nullptr ? reason : "unknown");
  LogRenderStatsIfNeeded(true);
}

void Application::LogRenderStatsIfNeeded(bool force) {
  if (!redraw_trace_enabled_ || (!force && redraw_trace_frames_ < kRenderTraceLogInterval)) {
    return;
  }

  redraw_trace_frames_ = 0;
  redraw_trace_full_frames_ = 0;
  redraw_trace_partial_frames_ = 0;
  redraw_trace_total_ns_ = 0;
  redraw_trace_total_dirty_rects_ = 0;
  redraw_trace_total_rendered_clips_ = 0;
  redraw_trace_max_dirty_rects_ = 0;
  redraw_trace_max_rendered_clips_ = 0;
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
