#pragma once

#include <SDL3/SDL.h>

#include <optional>
#include <thread>
#include <vector>

#include "app/AppStartupOptions.h"
#include "app/RedrawTraceAccumulator.h"
#include "app/SceneTexturePresenter.h"
#include "workspace/WorkspaceShell.h"

namespace microide::app {

class Application {
 public:
  explicit Application(AppStartupOptions startup_options = {});
  ~Application();

  Application(const Application&) = delete;
  Application& operator=(const Application&) = delete;

  int Run();

 private:
  bool Initialize();
  void Shutdown();
  workspace::WorkspaceShell::EventResult HandleEvent(const SDL_Event& event);
  void Render(std::vector<SDL_FRect> dirty_rects = {},
              const char* reason = "full");
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
  bool presentation_state_dirty_ = true;
  float last_presented_ui_scale_ = 1.0f;
  // Owns the retained scene render-target texture and its resize-coalescing.
  SceneTexturePresenter scene_texture_;
  // Owns the MICROIDE_TRACE_REDRAW timing/coverage bookkeeping.
  RedrawTraceAccumulator redraw_trace_;

  AppStartupOptions startup_options_;
  workspace::WorkspaceShell workspace_shell_;
  // Background-init worker for the syntax-highlight registry. Runs in
  // parallel with shell construction so its parse cost is hidden behind
  // the vsync-blocked blank present. Joined in Shutdown() to ensure no
  // worker is live when static destructors run.
  std::thread syntax_registry_warmup_;
};

}  // namespace microide::app
