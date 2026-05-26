#pragma once

#include <SDL3/SDL.h>

#include <optional>
#include <thread>
#include <vector>

#include "app/AppStartupOptions.h"
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
  bool EnsureSceneTexture(int logical_width, int logical_height);
  void DestroySceneTexture();
  void RecordRenderStats(bool full_redraw_requested,
                         bool full_redraw,
                         bool promoted_partial_to_full,
                         std::size_t dirty_rect_count,
                         std::size_t rendered_clip_count,
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
  bool redraw_trace_verbose_enabled_ = false;
  int scene_texture_width_ = 0;
  int scene_texture_height_ = 0;
  // Wall-clock timestamp of the most recent window-resize / pixel-size-change
  // event. Used to coalesce scene-texture reallocation during active resize
  // drags so we don't pay GPU texture alloc per resize event. Zero means "no
  // recent resize"; the texture is allowed to be (re)allocated immediately.
  Uint64 last_resize_event_ns_ = 0;
  bool presentation_state_dirty_ = true;
  float last_presented_ui_scale_ = 1.0f;
  Uint64 redraw_trace_frames_ = 0;
  Uint64 redraw_trace_full_frames_ = 0;
  Uint64 redraw_trace_partial_frames_ = 0;
  Uint64 redraw_trace_total_ns_ = 0;
  Uint64 redraw_trace_total_dirty_rects_ = 0;
  Uint64 redraw_trace_total_rendered_clips_ = 0;
  std::size_t redraw_trace_max_dirty_rects_ = 0;
  std::size_t redraw_trace_max_rendered_clips_ = 0;

  AppStartupOptions startup_options_;
  workspace::WorkspaceShell workspace_shell_;
  // Background-init worker for the syntax-highlight registry. Runs in
  // parallel with shell construction so its parse cost is hidden behind
  // the vsync-blocked blank present. Joined in Shutdown() to ensure no
  // worker is live when static destructors run.
  std::thread syntax_registry_warmup_;
};

}  // namespace microide::app
