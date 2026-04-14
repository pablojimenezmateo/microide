#include "app/Application.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <optional>

#include "util/StartupTrace.h"
#include "util/WindowPresentation.h"

namespace microide::app {

namespace {

constexpr int kInitialWindowWidth = 1440;
constexpr int kInitialWindowHeight = 900;

}  // namespace

Application::~Application() {
  Shutdown();
}

int Application::Run() {
  if (!Initialize()) {
    return 1;
  }

  running_ = true;
  bool dirty = true;

  while (running_) {
    if (dirty) {
      Render();
      dirty = false;
    }

    SDL_Event event;
    const std::optional<Uint32> next_delay = workspace_shell_.NextAnimationDelayMs();
    const bool has_event =
        next_delay.has_value() ? SDL_WaitEventTimeout(&event, static_cast<Sint32>(*next_delay))
                               : SDL_WaitEvent(&event);
    if (!has_event) {
      if (next_delay.has_value()) {
        dirty = true;
        continue;
      }
      SDL_Log("SDL_WaitEvent failed: %s", SDL_GetError());
      break;
    }

    do {
      dirty = HandleEvent(event) || dirty;
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

  {
    util::StartupTrace::Scope workspace_init_scope("WorkspaceShell::Initialize");
    if (!workspace_shell_.Initialize(std::filesystem::current_path())) {
      SDL_Log("Workspace initialization failed");
      return false;
    }
  }
  workspace_shell_.SetDialogWindow(window_);

  custom_window_chrome_enabled_ = false;
  {
    util::StartupTrace::Scope window_chrome_scope("WindowChromeSetup");
    if (!SDL_SetWindowBordered(window_, false)) {
      SDL_Log("SDL_SetWindowBordered(false) failed: %s", SDL_GetError());
    } else if (!SDL_SetWindowHitTest(window_, &Application::WindowHitTestCallback, this)) {
      SDL_Log("SDL_SetWindowHitTest failed: %s", SDL_GetError());
      SDL_SetWindowBordered(window_, true);
    } else {
      custom_window_chrome_enabled_ = true;
    }
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
  return true;
}

void Application::Shutdown() {
  if (!initialized_) {
    return;
  }

  StopWindowDrag();
  workspace_shell_.SetDialogWindow(nullptr);
  workspace_shell_.Shutdown();

  if (window_ != nullptr) {
    SDL_StopTextInput(window_);
  }

  if (renderer_ != nullptr) {
    SDL_DestroyRenderer(renderer_);
    renderer_ = nullptr;
  }

  if (window_ != nullptr) {
    SDL_DestroyWindow(window_);
    window_ = nullptr;
  }

  SDL_Quit();
  initialized_ = false;
}

bool Application::HandleEvent(const SDL_Event& event) {
  switch (event.type) {
    case SDL_EVENT_QUIT:
      workspace_shell_.RequestQuit();
      if (workspace_shell_.ConsumeQuitRequested()) {
        running_ = false;
      }
      return true;
    case SDL_EVENT_WINDOW_EXPOSED:
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
      UpdateRendererPresentation();
      return true;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
      StopWindowDrag();
      break;
  }

  if (window_drag_active_) {
    if (event.type == SDL_EVENT_MOUSE_MOTION) {
      return UpdateWindowDrag();
    }
    if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
        event.button.button == SDL_BUTTON_LEFT) {
      StopWindowDrag();
      return true;
    }
  }

  UpdateRendererPresentation();
  SDL_Event converted_event = event;
  if (renderer_ != nullptr) {
    SDL_ConvertEventToRenderCoordinates(renderer_, &converted_event);
  }

  const bool start_window_drag =
      event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
      event.button.button == SDL_BUTTON_LEFT && event.button.clicks == 1 &&
      window_ != nullptr &&
      (SDL_GetWindowFlags(window_) & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED)) == 0 &&
      workspace_shell_.WindowDragRegionContains(converted_event.button.x,
                                                converted_event.button.y);

  const bool handled = workspace_shell_.HandleEvent(converted_event);
  ConsumeWindowActions();
  if (workspace_shell_.ConsumeQuitRequested()) {
    running_ = false;
    return true;
  }
  if (start_window_drag && StartWindowDrag(converted_event)) {
    return true;
  }
  return handled;
}

bool Application::StartWindowDrag(const SDL_Event& converted_event) {
  if (window_ == nullptr || converted_event.type != SDL_EVENT_MOUSE_BUTTON_DOWN ||
      converted_event.button.button != SDL_BUTTON_LEFT || window_drag_active_) {
    return false;
  }

  if (!workspace_shell_.WindowDragRegionContains(converted_event.button.x,
                                                 converted_event.button.y)) {
    return false;
  }

  if (!SDL_GetWindowPosition(window_, &window_drag_origin_x_, &window_drag_origin_y_)) {
    return false;
  }
  SDL_GetGlobalMouseState(&window_drag_mouse_x_, &window_drag_mouse_y_);
  SDL_CaptureMouse(true);
  window_drag_active_ = true;
  return true;
}

bool Application::UpdateWindowDrag() {
  if (!window_drag_active_ || window_ == nullptr) {
    return false;
  }

  float global_mouse_x = 0.0f;
  float global_mouse_y = 0.0f;
  SDL_GetGlobalMouseState(&global_mouse_x, &global_mouse_y);
  const int target_x = window_drag_origin_x_ +
                       static_cast<int>(std::lround(global_mouse_x - window_drag_mouse_x_));
  const int target_y = window_drag_origin_y_ +
                       static_cast<int>(std::lround(global_mouse_y - window_drag_mouse_y_));
  SDL_SetWindowPosition(window_, target_x, target_y);
  return true;
}

void Application::StopWindowDrag() {
  if (!window_drag_active_) {
    return;
  }

  window_drag_active_ = false;
  SDL_CaptureMouse(false);
}

void Application::Render() {
  if (renderer_ == nullptr) {
    return;
  }

  std::optional<util::StartupTrace::Scope> first_render_scope;
  if (!first_render_complete_ && util::StartupTrace::Enabled()) {
    first_render_scope.emplace("Application::FirstRender");
  }

  int width = 0;
  int height = 0;
  if (!UpdateRendererPresentation(&width, &height)) {
    return;
  }

  workspace_shell_.Render(renderer_, width, height);
  SDL_RenderPresent(renderer_);
  first_render_complete_ = true;
}

bool Application::UpdateRendererPresentation(int* logical_width, int* logical_height) {
  if (window_ == nullptr || renderer_ == nullptr) {
    return false;
  }

  int pixel_width = 0;
  int pixel_height = 0;
  if (!SDL_GetRenderOutputSize(renderer_, &pixel_width, &pixel_height)) {
    SDL_Log("SDL_GetRenderOutputSize failed: %s", SDL_GetError());
    return false;
  }
  if (pixel_width <= 0 || pixel_height <= 0) {
    return false;
  }

  const util::WindowPresentation presentation = util::ComputeWindowPresentation(
      pixel_width, pixel_height, SDL_GetWindowDisplayScale(window_), workspace_shell_.UiScale());
  const SDL_WindowFlags flags = SDL_GetWindowFlags(window_);
  workspace_shell_.SetPresentationScale(presentation.presentation_scale_x,
                                        presentation.presentation_scale_y);
  workspace_shell_.SetWindowChromeState(presentation.logical_width, presentation.logical_height,
                                        (flags & SDL_WINDOW_MAXIMIZED) != 0,
                                        (flags & SDL_WINDOW_FULLSCREEN) != 0,
                                        custom_window_chrome_enabled_);

  if (!SDL_SetRenderLogicalPresentation(renderer_, presentation.logical_width,
                                        presentation.logical_height,
                                        SDL_LOGICAL_PRESENTATION_STRETCH)) {
    SDL_Log("SDL_SetRenderLogicalPresentation failed: %s", SDL_GetError());
    return false;
  }

  if (logical_width != nullptr) {
    *logical_width = presentation.logical_width;
  }
  if (logical_height != nullptr) {
    *logical_height = presentation.logical_height;
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
      UpdateRendererPresentation();
      return;
    }
    case workspace::WorkspaceShell::WindowAction::ToggleFullscreen: {
      const SDL_WindowFlags flags = SDL_GetWindowFlags(window_);
      SDL_SetWindowFullscreen(window_, (flags & SDL_WINDOW_FULLSCREEN) == 0);
      UpdateRendererPresentation();
      return;
    }
  }
}

SDL_HitTestResult Application::WindowHitTest(const SDL_Point& area) const {
  if (!custom_window_chrome_enabled_ || renderer_ == nullptr) {
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
