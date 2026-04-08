#pragma once

#include <SDL3/SDL.h>

#include "workspace/WorkspaceShell.h"

namespace microide::app {

class Application {
 public:
  Application() = default;
  ~Application();

  Application(const Application&) = delete;
  Application& operator=(const Application&) = delete;

  int Run();

 private:
  bool Initialize();
  void Shutdown();
  bool HandleEvent(const SDL_Event& event);
  void Render();
  bool UpdateRendererPresentation(int* logical_width = nullptr, int* logical_height = nullptr);
  void ConsumeWindowActions();
  SDL_HitTestResult WindowHitTest(const SDL_Point& area) const;
  static SDL_HitTestResult SDLCALL WindowHitTestCallback(SDL_Window* window,
                                                         const SDL_Point* area,
                                                         void* data);

  SDL_Window* window_ = nullptr;
  SDL_Renderer* renderer_ = nullptr;
  bool initialized_ = false;
  bool running_ = false;
  bool custom_window_chrome_enabled_ = false;
  bool first_render_complete_ = false;

  workspace::WorkspaceShell workspace_shell_;
};

}  // namespace microide::app
