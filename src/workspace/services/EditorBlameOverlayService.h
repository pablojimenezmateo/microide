#pragma once

#include <SDL3/SDL.h>

#include <filesystem>
#include <functional>
#include <optional>

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

  std::optional<editor::EditorBlameOverlay> BuildCompareOverlay(
      const std::filesystem::path& project_root,
      render::TextRenderer& text_renderer,
      project::GitBlameService& git_blame_service,
      CompareTabState& compare_tab,
      const CompareOverlayLayout& layout,
      // Where a right-pane document line's inline annotation goes, given the line's
      // full visual width: its on-screen row and the on-screen column just past its
      // text, or nullopt when the line is not represented in the diff. Resolved by
      // the caller because only it knows the presentation and soft-wrap row model —
      // the service used to place the annotation at the MODEL row index, which is a
      // different row space from the surface's scroll offset the moment the diff
      // collapses a run or wraps a line.
      const std::function<std::optional<CompareBlameAnchor>(std::size_t, std::size_t)>&
          compare_blame_anchor) const;

  void SetVisibleOverlay(std::optional<editor::EditorBlameOverlay> overlay);
  void ClearVisibleOverlay();

  const editor::EditorBlameLine* VisibleLine(std::size_t line_index) const;
  const editor::EditorBlameLine* LineAtPosition(float x, float y) const;

 private:
  std::optional<editor::EditorBlameOverlay> visible_overlay_;
  // The query descriptors, kept alive between frames rather than rebuilt inside
  // each Build* call. `GitBlameRequest` owns two `std::filesystem::path`s, and a
  // libstdc++ path copy is ~3.5 allocations (the pathname string, the component
  // list, a string per component); inline blame builds a request on EVERY painted
  // frame, so constructing the aggregate cost ~7 allocations and ~1.3 KB per frame
  // for two values that change only when the file or the project does. Assigned
  // through AssignPathIfChanged, so a steady frame allocates nothing here.
  //
  // One each because the two surfaces can be on screen in the same session; each
  // is written by a single caller, and inline blame renders only the ACTIVE pane,
  // so a split does not make two viewports fight over one descriptor.
  mutable project::GitBlameRequest editor_request_;
  mutable project::GitBlameRequest compare_request_;
};

}  // namespace microide::workspace
