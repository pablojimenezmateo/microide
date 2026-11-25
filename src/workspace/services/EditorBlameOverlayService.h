#pragma once

#include <SDL3/SDL.h>

#include <filesystem>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>

#include "editor/EditorViewRenderer.h"
#include "editor/TextViewport.h"
#include "project/GitBlameService.h"
#include "render/TextRenderer.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/state/WorkspaceTabState.h"

namespace microide::workspace {

class EditorBlameOverlayService {
 public:
  struct CompareBlameAnchor {
    std::size_t visual_row = 0;
    std::size_t end_column = 0;
  };

  struct CompareOverlayLayout {
    SDL_FRect pane_rect{};
    float right_x = 0.0f;
    float gutter_width = 0.0f;
    float right_width = 0.0f;
    float rows_y = 0.0f;
    float line_height = 0.0f;
    int visible_rows = 0;
    std::size_t visible_columns = 0;
  };

  void SetProjectLauncherSource(std::function<const platform::ProcessLauncher*()> source) {
    project_launcher_ = std::move(source);
  }

  bool FitsPane(render::TextRenderer& text_renderer,
                const editor::TextViewport& viewport,
                const SDL_FRect& rect,
                float minimum_pane_width,
                bool show_line_numbers) const;

  std::optional<editor::EditorBlameOverlay> BuildEditorOverlay(
      const std::filesystem::path& project_root,
      render::TextRenderer& text_renderer,
      project::GitBlameService& git_blame_service,
      editor::TextViewport& viewport,
      const SDL_FRect& rect,
      float minimum_pane_width,
      std::size_t sticky_scroll_rows,
      bool show_line_numbers) const;

  const std::optional<editor::EditorBlameOverlay>& EditorOverlayForFrame(
      std::uint64_t frame_id,
      const std::filesystem::path& project_root,
      render::TextRenderer& text_renderer,
      project::GitBlameService& git_blame_service,
      editor::TextViewport& viewport,
      const SDL_FRect& rect,
      float minimum_pane_width,
      std::size_t sticky_scroll_rows,
      bool show_line_numbers) const;

  std::optional<editor::EditorBlameOverlay> BuildCompareOverlay(
      const std::filesystem::path& project_root,
      render::TextRenderer& text_renderer,
      project::GitBlameService& git_blame_service,
      CompareTabState& compare_tab,
      const CompareOverlayLayout& layout,
      const std::function<std::optional<CompareBlameAnchor>(std::size_t, std::size_t)>&
          compare_blame_anchor) const;

  void SetVisibleOverlay(std::optional<editor::EditorBlameOverlay> overlay);
  void SetVisibleOverlay(std::optional<editor::EditorBlameOverlay> overlay,
                         const editor::TextViewport& source);
  void ForgetVisibleOverlayIfStale(const editor::TextViewport* active);
  const std::optional<editor::EditorBlameOverlay>& VisibleOverlay() const { return visible_overlay_; }
  void ClearVisibleOverlay();

  const editor::EditorBlameLine* VisibleLine(std::size_t line_index) const;
  const editor::EditorBlameLine* LineAtPosition(float x, float y) const;

 private:
  const platform::ProcessLauncher* ProjectLauncher() const;

  std::function<const platform::ProcessLauncher*()> project_launcher_;
  std::optional<editor::EditorBlameOverlay> visible_overlay_;
  struct OverlaySource {
    const editor::TextViewport* viewport = nullptr;
    std::size_t caret_line = 0;
    std::size_t scroll_line = 0;
    std::size_t horizontal_scroll = 0;
    std::uint64_t content_revision = 0;
    bool operator==(const OverlaySource&) const = default;
  };
  std::optional<OverlaySource> visible_source_;
  static OverlaySource SourceOf(const editor::TextViewport& viewport);
  mutable project::GitBlameRequest editor_request_;
  mutable std::uint64_t frame_overlay_frame_id_ = 0;
  mutable const editor::TextViewport* frame_overlay_viewport_ = nullptr;
  mutable SDL_FRect frame_overlay_rect_{};
  mutable std::optional<editor::EditorBlameOverlay> frame_overlay_;
  mutable project::GitBlameRequest compare_request_;
};

}
