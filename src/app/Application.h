#pragma once

#include <SDL3/SDL.h>

#include <optional>
#include <vector>

#include "app/AppStartupOptions.h"
#include "app/RedrawTraceAccumulator.h"
#include "app/SceneTexturePresenter.h"
#include "workspace/WorkspaceShell.h"

namespace microide::tests {
// Narrow headless-lifecycle test seam (see tests/ApplicationTests.cpp). Lets the
// suite drive the private Initialize()/Render()/Shutdown() in-process under the
// dummy SDL video driver so teardown can be verified under sanitizers.
struct ApplicationTestAccess;
}  // namespace microide::tests

namespace microide::app {

class Application {
 public:
  explicit Application(AppStartupOptions startup_options = {});
  ~Application();

  Application(const Application&) = delete;
  Application& operator=(const Application&) = delete;

  int Run();

 private:
  friend struct ::microide::tests::ApplicationTestAccess;

  bool Initialize();
  void Shutdown();
  workspace::WorkspaceShell::EventResult HandleEvent(const SDL_Event& event);
  void Render(std::vector<SDL_FRect> dirty_rects = {},
              const char* reason = "full");
  bool UpdateRendererPresentation(int* logical_width = nullptr, int* logical_height = nullptr);
  // Re-seed the shell's cached pointer position from a live query, then force the
  // cursor to re-apply. Used on window resize/move/restore/maximize, where the
  // window manager can suppress the motion events that keep the position fresh and
  // can repaint the displayed cursor behind SDL's back. The live query happens here
  // (event time, renderer available) so the render path never polls input.
  void ReseedPointerAndForceCursorReassert();
  void ConsumeWindowActions();
  // Destroy the SDL scene texture, renderer, and window if allocated. Idempotent and
  // independent of `initialized_`, so a partial Initialize() failure (window/renderer
  // created but init not finished) still releases them instead of leaking. (TD-42.)
  void DestroySdlResources();
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
};

}  // namespace microide::app
