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

  // The active project's launcher, stamped onto every blame request so its git
  // runs where the project lives (TD-2026-09-22-301). Wired once by the shell;
  // unset or null means the process-wide local launcher.
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

  // BuildEditorOverlay at most once per prepared frame for one pane: every render
  // of frame `frame_id` (each clip of a partial frame, and the QA redraw
  // verifier's full reference) gets the same overlay. The blame cache is filled by
  // a worker thread, so reading it per render let one frame paint blame in some
  // clips and not others when a result landed mid-frame.
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
  // The editor's overlay, remembering the buffer state it was built over: the
  // viewport, its caret line, its scroll and its content revision. Frame prep
  // builds it for an active editor pane (TD-2026-10-24-389); when the active tab
  // is a diff or a merge instead, prep builds none and that surface sets its own
  // as it paints -- ForgetVisibleOverlayIfStale, called at frame prep, drops an
  // editor overlay that no longer describes the active buffer in between, so no
  // hover card or click resolves against the editor a tab switch left.
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
  // Set only for the editor's overlay; the compare and merge overlays are not
  // checked (their cards are not laid out from it at frame prep).
  std::optional<OverlaySource> visible_source_;
  static OverlaySource SourceOf(const editor::TextViewport& viewport);
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
  // EditorOverlayForFrame's one slot: the frame, pane and rect it was built for.
  mutable std::uint64_t frame_overlay_frame_id_ = 0;
  mutable const editor::TextViewport* frame_overlay_viewport_ = nullptr;
  mutable SDL_FRect frame_overlay_rect_{};
  mutable std::optional<editor::EditorBlameOverlay> frame_overlay_;
  mutable project::GitBlameRequest compare_request_;
};

}  // namespace microide::workspace
