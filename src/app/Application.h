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

  SDL_Window* window_ = nullptr;
  SDL_Renderer* renderer_ = nullptr;
  bool initialized_ = false;
  bool running_ = false;

  workspace::WorkspaceShell workspace_shell_;
};

}  // namespace microide::app
