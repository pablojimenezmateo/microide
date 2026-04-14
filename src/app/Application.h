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
  bool StartWindowDrag(const SDL_Event& converted_event);
  bool UpdateWindowDrag();
  void StopWindowDrag();
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
  bool first_render_complete_ = false;
  bool window_drag_active_ = false;
  int window_drag_origin_x_ = 0;
  int window_drag_origin_y_ = 0;
  float window_drag_mouse_x_ = 0.0f;
  float window_drag_mouse_y_ = 0.0f;

  workspace::WorkspaceShell workspace_shell_;
};

}  // namespace microide::app
