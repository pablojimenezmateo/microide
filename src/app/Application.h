#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "app/AppStartupOptions.h"
#include "app/DirtyRegionPolicy.h"
#include "app/RedrawTraceAccumulator.h"
#include "app/RedrawVerifier.h"
#include "app/SceneTexturePresenter.h"
#include "app/TerminationSignals.h"
#include "workspace/shell/WorkspaceShell.h"

namespace microide::tests {
struct ApplicationTestAccess;
}

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
              const char* reason = "full",
              bool prepared_only = false);
  void PresentRetainedScene();
  bool UpdateRendererPresentation(int* logical_width = nullptr, int* logical_height = nullptr);
  void ApplyMinimumWindowSize(SDL_FPoint window_per_unit);
  void ReseedPointerAndForceCursorReassert();
  void ConsumeWindowActions();
  void DestroySdlResources();
  SDL_HitTestResult WindowHitTest(const SDL_Point& area) const;
  static SDL_HitTestResult SDLCALL WindowHitTestCallback(SDL_Window* window,
                                                         const SDL_Point* area,
                                                         void* data);

  SDL_Window* window_ = nullptr;
  SDL_Renderer* renderer_ = nullptr;
  bool initialized_ = false;
  bool running_ = false;
  std::uint32_t termination_event_type_ = 0;
  std::uint32_t frame_event_type_ = 0;
  std::unique_ptr<TerminationSignalWatcher> termination_signals_;
  bool first_render_complete_ = false;
  bool presentation_state_dirty_ = true;
  float last_presented_ui_scale_ = 1.0f;
  int applied_minimum_window_width_ = 0;
  int applied_minimum_window_height_ = 0;
  SceneTexturePresenter scene_texture_;
  RedrawTraceAccumulator redraw_trace_;
  RedrawVerifier redraw_verifier_;
  std::uint64_t frame_index_ = 0;
  DirtyRegionAnalysis dirty_region_analysis_;

  AppStartupOptions startup_options_;
  workspace::WorkspaceShell workspace_shell_;
};

}
