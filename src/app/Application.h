#pragma once

#include <SDL3/SDL.h>

#include <optional>
#include <vector>

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
  workspace::WorkspaceShell::EventResult HandleEvent(const SDL_Event& event);
  void Render(std::vector<SDL_FRect> dirty_rects = {},
              const char* reason = "full");
  bool StartWindowDrag(const SDL_Event& converted_event);
  bool UpdateWindowDrag();
  void StopWindowDrag();
  bool UpdateRendererPresentation(int* logical_width = nullptr, int* logical_height = nullptr);
  bool EnsureSceneTexture(int logical_width, int logical_height);
  void DestroySceneTexture();
  void RecordRenderStats(bool full_redraw,
                         std::size_t dirty_rect_count,
                         const char* reason,
                         Uint64 elapsed_ns);
  void LogRenderStatsIfNeeded(bool force = false);
  void ConsumeWindowActions();
  SDL_HitTestResult WindowHitTest(const SDL_Point& area) const;
  static SDL_HitTestResult SDLCALL WindowHitTestCallback(SDL_Window* window,
                                                         const SDL_Point* area,
                                                         void* data);

  SDL_Window* window_ = nullptr;
  SDL_Renderer* renderer_ = nullptr;
  SDL_Texture* scene_texture_ = nullptr;
  bool initialized_ = false;
  bool running_ = false;
  bool first_render_complete_ = false;
  bool scene_texture_valid_ = false;
  bool redraw_trace_enabled_ = false;
  bool window_drag_active_ = false;
  int window_drag_origin_x_ = 0;
  int window_drag_origin_y_ = 0;
  int scene_texture_width_ = 0;
  int scene_texture_height_ = 0;
  Uint64 redraw_trace_frames_ = 0;
  Uint64 redraw_trace_full_frames_ = 0;
  Uint64 redraw_trace_partial_frames_ = 0;
  Uint64 redraw_trace_total_ns_ = 0;
  float window_drag_mouse_x_ = 0.0f;
  float window_drag_mouse_y_ = 0.0f;

  workspace::WorkspaceShell workspace_shell_;
};

}  // namespace microide::app
