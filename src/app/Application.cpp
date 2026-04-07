#include "app/Application.h"

#include <SDL3/SDL.h>

#include <filesystem>
#include <optional>

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

  if (!SDL_Init(SDL_INIT_VIDEO)) {
    SDL_Log("SDL_Init failed: %s", SDL_GetError());
    return false;
  }

  const SDL_WindowFlags window_flags =
      SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;

  window_ = SDL_CreateWindow(
      "microide",
      kInitialWindowWidth,
      kInitialWindowHeight,
      window_flags);
  if (window_ == nullptr) {
    SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
    return false;
  }

  renderer_ = SDL_CreateRenderer(window_, nullptr);
  if (renderer_ == nullptr) {
    SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
    return false;
  }

  SDL_SetRenderVSync(renderer_, 1);

  if (!workspace_shell_.Initialize(std::filesystem::current_path())) {
    SDL_Log("Workspace initialization failed");
    return false;
  }

  if (!SDL_StartTextInput(window_)) {
    SDL_Log("SDL_StartTextInput failed: %s", SDL_GetError());
  }

  initialized_ = true;
  return true;
}

void Application::Shutdown() {
  if (!initialized_) {
    return;
  }

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
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
      return true;
  }

  const bool handled = workspace_shell_.HandleEvent(event);
  if (workspace_shell_.ConsumeQuitRequested()) {
    running_ = false;
    return true;
  }
  return handled;
}

void Application::Render() {
  if (renderer_ == nullptr) {
    return;
  }

  int width = 0;
  int height = 0;
  if (!SDL_GetRenderOutputSize(renderer_, &width, &height)) {
    SDL_Log("SDL_GetRenderOutputSize failed: %s", SDL_GetError());
    return;
  }

  workspace_shell_.Render(renderer_, width, height);
  SDL_RenderPresent(renderer_);
}

}  // namespace microide::app
