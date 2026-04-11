#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "workspace/WorkspaceShellShared.h"

namespace microide::workspace {

namespace {

constexpr float kDivider = 1.0f;
constexpr float kSidebarHeaderHeight = 26.0f;
constexpr float kBottomPanelHeaderHeight = 28.0f;
constexpr float kSidebarInset = 10.0f;
constexpr float kSidebarRowHeight = 20.0f;
constexpr float kTreeIndentWidth = 14.0f;
constexpr float kTreeChevronSlotWidth = 12.0f;
constexpr float kDirtyPromptWidth = 460.0f;
constexpr float kDirtyPromptHeight = 176.0f;
constexpr float kDirtyPromptButtonWidth = 96.0f;
constexpr float kDirtyPromptButtonHeight = 28.0f;
constexpr float kDirtyPromptButtonGap = 10.0f;
constexpr float kPromptSurfaceWidth = 520.0f;
constexpr float kPromptSurfaceHeight = 188.0f;
constexpr float kPromptSurfaceInputHeight = 24.0f;
constexpr float kPromptSurfaceButtonWidth = 108.0f;
constexpr float kPromptSurfaceButtonHeight = 28.0f;
constexpr float kPromptSurfaceButtonGap = 10.0f;
constexpr float kScrollbarThickness = 10.0f;
constexpr float kScrollbarInset = 2.0f;
constexpr float kBottomPanelCommandTopPadding = 8.0f;
constexpr float kCompareMarkerLaneWidth = 6.0f;
constexpr float kCompareMarkerLaneGap = 3.0f;

SDL_FRect ComputeDirtyPromptRect(const SDL_FRect& full) {
  const float width = std::min(kDirtyPromptWidth, full.w - 32.0f);
  const float height = std::min(kDirtyPromptHeight, full.h - 32.0f);
  return MakeRect(full.x + std::floor((full.w - width) * 0.5f),
                  full.y + std::floor((full.h - height) * 0.5f), width, height);
}

std::array<SDL_FRect, 3> ComputeDirtyPromptButtonRects(const SDL_FRect& dialog) {
  const float total_width =
      kDirtyPromptButtonWidth * 3.0f + kDirtyPromptButtonGap * 2.0f;
  const float start_x = dialog.x + dialog.w - total_width - 16.0f;
  const float y = dialog.y + dialog.h - kDirtyPromptButtonHeight - 16.0f;
  return {
      MakeRect(start_x, y, kDirtyPromptButtonWidth, kDirtyPromptButtonHeight),
      MakeRect(start_x + kDirtyPromptButtonWidth + kDirtyPromptButtonGap, y,
               kDirtyPromptButtonWidth, kDirtyPromptButtonHeight),
      MakeRect(start_x + (kDirtyPromptButtonWidth + kDirtyPromptButtonGap) * 2.0f, y,
               kDirtyPromptButtonWidth, kDirtyPromptButtonHeight),
  };
}

SDL_FRect ComputePromptSurfaceRect(const SDL_FRect& full) {
  const float width = std::min(kPromptSurfaceWidth, full.w - 32.0f);
  const float height = std::min(kPromptSurfaceHeight, full.h - 32.0f);
  return MakeRect(full.x + std::floor((full.w - width) * 0.5f),
                  full.y + std::floor((full.h - height) * 0.5f), width, height);
}

std::array<SDL_FRect, 2> ComputePromptSurfaceButtonRects(const SDL_FRect& dialog) {
  const float total_width =
      kPromptSurfaceButtonWidth * 2.0f + kPromptSurfaceButtonGap;
  const float start_x = dialog.x + dialog.w - total_width - 16.0f;
  const float y = dialog.y + dialog.h - kPromptSurfaceButtonHeight - 16.0f;
  return {
      MakeRect(start_x, y, kPromptSurfaceButtonWidth, kPromptSurfaceButtonHeight),
      MakeRect(start_x + kPromptSurfaceButtonWidth + kPromptSurfaceButtonGap, y,
               kPromptSurfaceButtonWidth, kPromptSurfaceButtonHeight),
  };
}

SDL_FRect ComputePromptSurfaceInputRect(const SDL_FRect& dialog) {
  return MakeRect(dialog.x + 16.0f, dialog.y + 98.0f, dialog.w - 32.0f,
                  kPromptSurfaceInputHeight);
}

void DrawScrollbarTrack(SDL_Renderer* renderer,
                        const render::Theme& theme,
                        const SDL_FRect& track) {
  if (renderer == nullptr || track.w <= 0.0f || track.h <= 0.0f) {
    return;
  }

  SDL_SetRenderDrawColor(renderer, theme.surface_raised.r, theme.surface_raised.g,
                         theme.surface_raised.b, theme.surface_raised.a);
  SDL_RenderFillRect(renderer, &track);
}

void DrawScrollbarThumb(SDL_Renderer* renderer,
                        const render::Theme& theme,
                        const SDL_FRect& thumb,
                        bool active = false) {
  if (renderer == nullptr || thumb.w <= 0.0f || thumb.h <= 0.0f) {
    return;
  }

  const SDL_Color thumb_color = active ? theme.accent : theme.text_disabled;
  SDL_SetRenderDrawColor(renderer, thumb_color.r, thumb_color.g, thumb_color.b, thumb_color.a);
  SDL_RenderFillRect(renderer, &thumb);
}

void DrawScrollbar(SDL_Renderer* renderer,
                   const render::Theme& theme,
                   const SDL_FRect& track,
                   const SDL_FRect& thumb,
                   bool active = false) {
  DrawScrollbarTrack(renderer, theme, track);
  DrawScrollbarThumb(renderer, theme, thumb, active);
}

SDL_Color CompareMarkerColor(const render::Theme& theme, compare::CompareRowKind kind) {
  switch (kind) {
    case compare::CompareRowKind::Added:
      return theme.diff_added;
    case compare::CompareRowKind::Deleted:
      return theme.diff_deleted;
    case compare::CompareRowKind::Modified:
      return theme.diff_modified;
    case compare::CompareRowKind::Unchanged:
    default:
      return theme.text_muted;
  }
}

SDL_Color MergeMarkerColor(const render::Theme& theme,
                           compare::MergeChoice choice,
                           bool valid) {
  if (!valid) {
    return theme.text_disabled;
  }

  switch (choice) {
    case compare::MergeChoice::Incoming:
      return theme.diff_added;
    case compare::MergeChoice::Current:
      return theme.diff_modified;
    case compare::MergeChoice::Both:
      return theme.accent;
    case compare::MergeChoice::Base:
    default:
      return theme.diff_deleted;
  }
}

void DrawCompareScrollbarMarkers(SDL_Renderer* renderer,
                                 const render::Theme& theme,
                                 const SDL_FRect& track,
                                 const compare::CompareModel& model) {
  if (renderer == nullptr) {
    return;
  }

  const auto markers = BuildCompareScrollbarMarkers(track, model);
  for (const CompareScrollbarMarker& marker : markers) {
    const SDL_Color color = CompareMarkerColor(theme, marker.kind);
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    SDL_RenderFillRect(renderer, &marker.rect);
  }
}

void DrawMergeScrollbarMarkers(SDL_Renderer* renderer,
                               const render::Theme& theme,
                               const SDL_FRect& track,
                               std::size_t total_rows,
                               const std::vector<MergeScrollbarMarkerInput>& inputs) {
  if (renderer == nullptr) {
    return;
  }

  const auto markers = BuildMergeScrollbarMarkers(track, total_rows, inputs);
  for (const MergeScrollbarMarker& marker : markers) {
    const SDL_Color color = MergeMarkerColor(theme, marker.choice, marker.valid);
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    SDL_RenderFillRect(renderer, &marker.rect);
  }
}

std::size_t MaxVisualColumns(const editor::TextViewport& viewport) {
  return viewport.max_visual_columns();
}

char GitMarker(project::GitFileStatus status) {
  switch (status) {
    case project::GitFileStatus::Conflicted:
      return '!';
    case project::GitFileStatus::Modified:
      return 'M';
    case project::GitFileStatus::Added:
      return 'A';
    case project::GitFileStatus::Deleted:
      return 'D';
    case project::GitFileStatus::Untracked:
      return 'U';
    case project::GitFileStatus::Clean:
    default:
      return ' ';
  }
}

SDL_Color GitMarkerColor(const render::Theme& theme, project::GitFileStatus status) {
  switch (status) {
    case project::GitFileStatus::Conflicted:
      return theme.accent;
    case project::GitFileStatus::Modified:
      return theme.diff_modified;
    case project::GitFileStatus::Added:
      return theme.diff_added;
    case project::GitFileStatus::Deleted:
      return theme.diff_deleted;
    case project::GitFileStatus::Untracked:
      return theme.accent;
    case project::GitFileStatus::Clean:
    default:
      return theme.text_disabled;
  }
}

void DrawChevron(SDL_Renderer* renderer, float x, float center_y, bool expanded, SDL_Color color) {
  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
  if (expanded) {
    SDL_RenderLine(renderer, x, center_y - 2.0f, x + 4.0f, center_y + 2.0f);
    SDL_RenderLine(renderer, x + 8.0f, center_y - 2.0f, x + 4.0f, center_y + 2.0f);
    return;
  }

  SDL_RenderLine(renderer, x + 2.0f, center_y - 4.0f, x + 6.0f, center_y);
  SDL_RenderLine(renderer, x + 2.0f, center_y + 4.0f, x + 6.0f, center_y);
}

void DrawCloseGlyph(SDL_Renderer* renderer, const SDL_FRect& rect, SDL_Color color) {
  if (renderer == nullptr) {
    return;
  }

  const float center_x = std::floor(rect.x + rect.w * 0.5f);
  const float center_y = std::floor(rect.y + rect.h * 0.5f);
  const auto draw_dot = [&](float x, float y) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    const SDL_FRect dot = MakeRect(x, y, 1.0f, 1.0f);
    SDL_RenderFillRect(renderer, &dot);
  };

  for (int offset = -3; offset <= 3; ++offset) {
    draw_dot(center_x + static_cast<float>(offset), center_y + static_cast<float>(offset));
    draw_dot(center_x + static_cast<float>(offset), center_y - static_cast<float>(offset));
  }
}

void DrawWindowControlGlyph(SDL_Renderer* renderer,
                            const SDL_FRect& rect,
                            microide::workspace::WorkspaceShell::WindowControlButtonId id,
                            SDL_Color color,
                            bool maximized) {
  if (renderer == nullptr) {
    return;
  }

  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
  const float left = rect.x + 4.0f;
  const float right = rect.x + rect.w - 4.0f;
  const float top = rect.y + 4.0f;
  const float bottom = rect.y + rect.h - 4.0f;
  const float center_y = rect.y + rect.h * 0.5f;

  switch (id) {
    case microide::workspace::WorkspaceShell::WindowControlButtonId::Minimize:
      SDL_RenderLine(renderer, left, center_y + 2.0f, right, center_y + 2.0f);
      return;
    case microide::workspace::WorkspaceShell::WindowControlButtonId::Maximize:
      if (maximized) {
        const SDL_FRect back = SDL_FRect{left + 1.5f, top + 3.0f, rect.w - 9.0f, rect.h - 9.0f};
        const SDL_FRect front = SDL_FRect{left - 1.0f, top + 1.0f, rect.w - 9.0f, rect.h - 9.0f};
        SDL_RenderRect(renderer, &back);
        SDL_RenderRect(renderer, &front);
      } else {
        const SDL_FRect outline = SDL_FRect{left, top + 1.0f, rect.w - 8.0f, rect.h - 8.0f};
        SDL_RenderRect(renderer, &outline);
      }
      return;
    case microide::workspace::WorkspaceShell::WindowControlButtonId::Close:
      SDL_RenderLine(renderer, left, top, right, bottom);
      SDL_RenderLine(renderer, right, top, left, bottom);
      return;
  }
}

}  // namespace

void WorkspaceShell::ResizeTerminalToPanel(const SDL_FRect& panel_rect) {
  auto* terminal_tab = ActiveTerminalTab();
  if (terminal_tab == nullptr) {
    return;
  }

  const int rows = BottomPanelVisibleRows(panel_rect.h);
  const float usable_width =
      std::max(16.0f, panel_rect.w - 24.0f - kScrollbarThickness - 6.0f);
  const int columns = std::max(
      1, static_cast<int>(std::floor(usable_width / std::max(1.0f, text_renderer_.CharWidth()))));
  terminal_tab->session.Resize(static_cast<std::size_t>(rows), static_cast<std::size_t>(columns));
}

void WorkspaceShell::DrawFilledRect(SDL_Renderer* renderer, const SDL_FRect& rect, SDL_Color color) const {
  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
  SDL_RenderFillRect(renderer, &rect);
}

void WorkspaceShell::DrawRect(SDL_Renderer* renderer, const SDL_FRect& rect, SDL_Color color) const {
  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
  SDL_RenderRect(renderer, &rect);
}

std::string WorkspaceShell::TruncateLabel(std::string_view text, float max_width) const {
  return text_renderer_.TruncateToWidth(text, max_width);
}

void WorkspaceShell::Render(SDL_Renderer* renderer, int width, int height) {
  if (renderer == nullptr || width <= 0 || height <= 0) {
    return;
  }

  ConsumePendingProjectOpenDialogResult();
  ConsumeProjectSearchUpdates();
  text_renderer_.EnsureInitialized(renderer, presentation_scale_x_, presentation_scale_y_);
  last_window_width_ = width;
  last_window_height_ = height;
  surface_.sidebar_width = ClampSidebarWidth(surface_.sidebar_width, static_cast<float>(width));
  surface_.bottom_panel_height = ClampBottomPanelHeight(surface_.bottom_panel_height, static_cast<float>(height));

  const WorkspaceLayout layout =
      ComputeLayout(static_cast<float>(width), static_cast<float>(height), surface_.sidebar_visible,
                    BottomPanelVisible(), surface_.sidebar_width, surface_.bottom_panel_height);
  SDL_Window* render_window = SDL_GetRenderWindow(renderer);
  SyncTextInputSurface(render_window);
  if (ActiveTabIsEditor()) {
    SyncActiveEditorTab();
    if (auto* editor_tab = ActiveEditorTab(); editor_tab != nullptr) {
      NormalizeEditorSplitTree(*editor_tab);
    }
  }
  if (ActiveTerminalTab() != nullptr) {
    ResizeTerminalToPanel(layout.bottom_panel);
  }
  float mouse_x = 0.0f;
  float mouse_y = 0.0f;
  SDL_GetMouseState(&mouse_x, &mouse_y);
  SDL_RenderCoordinatesFromWindow(renderer, mouse_x, mouse_y, &mouse_x, &mouse_y);
  UpdateMouseCursor(mouse_x, mouse_y);
  const std::vector<terminal::TerminalLine> terminal_lines =
      ActiveTerminalTab() != nullptr
          ? ActiveTerminalTab()->session.SnapshotLines()
          : std::vector<terminal::TerminalLine>{};
  std::optional<SDL_FRect> active_editor_pane_rect;
  visible_editor_blame_overlay_.reset();
  const bool draw_editor_caret =
      CaretVisibleNow() &&
      !(CurrentTextInputSurface() == TextInputSurface::Editor && !text_composition_.text.empty());

  DrawFilledRect(renderer, layout.full, theme_.window_background);
  DrawFilledRect(renderer, layout.menu_bar, theme_.chrome_background);
  DrawFilledRect(renderer,
                 MakeRect(layout.menu_bar.x, layout.menu_bar.y + layout.menu_bar.h - kDivider,
                          layout.menu_bar.w, kDivider),
                 theme_.border);
  DrawFilledRect(renderer, layout.project_tab_strip, theme_.chrome_background);
  DrawFilledRect(renderer,
                 MakeRect(layout.project_tab_strip.x,
                          layout.project_tab_strip.y + layout.project_tab_strip.h - kDivider,
                          layout.project_tab_strip.w, kDivider),
                 theme_.border);
  DrawFilledRect(renderer, layout.tab_strip, theme_.chrome_background);
  DrawFilledRect(renderer,
                 MakeRect(layout.tab_strip.x, layout.tab_strip.y + layout.tab_strip.h - kDivider,
                          layout.tab_strip.w, kDivider),
                 theme_.border);
  DrawFilledRect(renderer, layout.breadcrumb, theme_.chrome_background);
  DrawFilledRect(renderer,
                 MakeRect(layout.breadcrumb.x, layout.breadcrumb.y + layout.breadcrumb.h - kDivider,
                          layout.breadcrumb.w, kDivider),
                 theme_.border);

  if (surface_.sidebar_visible) {
    DrawFilledRect(renderer, layout.sidebar, theme_.surface_background);
    DrawFilledRect(renderer,
                   MakeRect(layout.sidebar.x + layout.sidebar.w, layout.sidebar.y, kDivider,
                            layout.sidebar.h),
                   surface_.drag_target == DragTarget::SidebarDivider ? theme_.accent : theme_.border);
    const SDL_FRect sidebar_header =
        MakeRect(layout.sidebar.x, layout.sidebar.y, layout.sidebar.w, kSidebarHeaderHeight);
    DrawFilledRect(renderer, sidebar_header, theme_.chrome_background);
    DrawFilledRect(renderer,
                   MakeRect(sidebar_header.x, sidebar_header.y + sidebar_header.h - kDivider,
                            sidebar_header.w, kDivider),
                   theme_.border);
  }

  if (BottomPanelVisible()) {
    DrawFilledRect(renderer, layout.bottom_panel, theme_.surface_background);
    DrawFilledRect(renderer,
                   MakeRect(layout.bottom_panel.x, layout.bottom_panel.y, layout.bottom_panel.w,
                            kDivider),
                   surface_.drag_target == DragTarget::BottomPanelDivider ? theme_.accent : theme_.border);
    const SDL_FRect panel_header = MakeRect(layout.bottom_panel.x, layout.bottom_panel.y,
                                            layout.bottom_panel.w, kBottomPanelHeaderHeight);
    DrawFilledRect(renderer, panel_header, theme_.chrome_background);
    DrawFilledRect(renderer,
                   MakeRect(panel_header.x, panel_header.y + panel_header.h - kDivider,
                            panel_header.w, kDivider),
                   theme_.border);
  }

  if (ActiveTabIsCompare()) {
    RenderCompareSurface(renderer, layout.editor_surface);
  } else if (ActiveTabIsMerge()) {
    RenderMergeSurface(renderer, layout.editor_surface);
  } else {
    const auto panes = ComputeEditorPaneLayouts(layout.editor_surface);
    if (panes.empty() && text_viewport_.is_placeholder()) {
      active_editor_pane_rect = layout.editor_surface;
      editor_view_renderer_.Render(renderer, text_renderer_, theme_, text_viewport_,
                                   layout.editor_surface, draw_editor_caret, "", std::nullopt,
                                   std::nullopt);
    }
    auto* editor_tab = ActiveEditorTab();
    for (const EditorPaneLayout& pane : panes) {
      editor::TextViewport* viewport =
          pane.active ? &text_viewport_
                      : (editor_tab != nullptr ? FindEditorView(*editor_tab, pane.leaf_id)
                                               : nullptr);
      if (viewport == nullptr) {
        continue;
      }
      if (pane.active) {
        active_editor_pane_rect = pane.rect;
      }
      const auto blame_overlay = pane.active ? BuildEditorBlameOverlay(*viewport, pane.rect)
                                             : std::nullopt;
      if (pane.active) {
        visible_editor_blame_overlay_ = blame_overlay;
      }
      editor_view_renderer_.Render(renderer, text_renderer_, theme_, *viewport, pane.rect,
                                   pane.active && draw_editor_caret,
                                   pane.active && (surface_.overlay_mode == OverlayMode::BufferSearch ||
                                                   surface_.overlay_mode == OverlayMode::BufferReplace)
                                       ? overlay_workflow_.buffer_search.query
                                       : "",
                                   pane.active ? ActiveBufferSearchMatch() : std::nullopt,
                                   blame_overlay);
    }
  }
  if (last_mouse_position_valid_) {
    UpdateEditorBlameHover(last_mouse_x_, last_mouse_y_);
  }

  const auto draw_text_on =
      [&](float x, float y, SDL_Color foreground, SDL_Color background, std::string_view text) {
        text_renderer_.DrawStringOn(renderer, x, y, foreground, background, text);
      };
  const auto draw_text = [&](float x, float y, SDL_Color foreground, std::string_view text) {
    text_renderer_.DrawString(renderer, x, y, foreground, text);
  };
  struct TextInputVisual {
    TextInputSurface surface = TextInputSurface::None;
    SDL_FRect area{};
    float text_x = 0.0f;
    float text_y = 0.0f;
    float cursor_x = 0.0f;
    SDL_Color foreground{};
    SDL_Color background{};
  };
  const auto active_text_input_visual = [&]() -> std::optional<TextInputVisual> {
    const TextInputSurface surface = CurrentTextInputSurface();
    const float line_height = text_renderer_.LineHeight();
    const float char_width = std::max(1.0f, text_renderer_.CharWidth());

    switch (surface) {
      case TextInputSurface::Editor: {
        if (ActiveTabIsCompare()) {
          CompareTabState* compare_tab = ActiveCompareTab();
          if (compare_tab == nullptr || !compare_tab->right_editable || !compare_tab->right_view_active) {
            return std::nullopt;
          }
          const CompareSurfaceLayout surface_layout =
              ComputeCompareSurfaceLayout(layout.editor_surface, *compare_tab);
          const std::size_t model_row =
              CompareRowIndexForRightLine(*compare_tab, compare_tab->right_viewport.cursor_line());
          const float cursor_x =
              surface_layout.right_x + surface_layout.gutter_width +
              static_cast<float>(compare_tab->right_viewport.cursor_visual_column() -
                                 compare_tab->horizontal_scroll) *
                  char_width;
          const float cursor_y =
              surface_layout.rows_y +
              static_cast<float>(model_row > static_cast<std::size_t>(std::max(0, compare_tab->scroll_row))
                                     ? model_row - static_cast<std::size_t>(std::max(0, compare_tab->scroll_row))
                                     : 0) *
                  surface_layout.line_height;
          return TextInputVisual{
              .surface = surface,
              .area = MakeRect(cursor_x, cursor_y - 1.0f, char_width, surface_layout.line_height),
              .text_x = cursor_x,
              .text_y = cursor_y,
              .cursor_x = cursor_x,
              .foreground = theme_.text_primary,
              .background = theme_.editor_background,
          };
        }
        if (ActiveTabIsMerge()) {
          MergeTabState* merge_tab = ActiveMergeTab();
          if (merge_tab == nullptr) {
            return std::nullopt;
          }
          const MergeSurfaceLayout surface_layout =
              ComputeMergeSurfaceLayout(layout.editor_surface, *merge_tab);
          const float bottom_reserved = surface_layout.show_horizontal ? kScrollbarThickness + kScrollbarInset
                                                                      : 0.0f;
          const float content_height = std::max(0.0f, layout.editor_surface.h - bottom_reserved);
          const SDL_FRect result_rect = MakeRect(
              surface_layout.center_x, surface_layout.rows_y - 8.0f,
              surface_layout.gutter_width + surface_layout.center_width,
              std::max(0.0f, layout.editor_surface.y + content_height -
                                   (surface_layout.rows_y - 8.0f)));
          const editor::EditorViewMetrics metrics =
              editor::EditorViewRenderer::ComputeMetrics(text_renderer_, merge_tab->result_viewport,
                                                         result_rect);
          const float cursor_x =
              metrics.text_x + static_cast<float>(merge_tab->result_viewport.cursor_visual_column() -
                                                  merge_tab->result_viewport.horizontal_scroll()) *
                                   char_width;
          const float cursor_y =
              metrics.first_line_y +
              static_cast<float>(merge_tab->result_viewport.cursor_line() -
                                 merge_tab->result_viewport.scroll_line()) *
                  metrics.line_height;
          return TextInputVisual{
              .surface = surface,
              .area = MakeRect(cursor_x, cursor_y - 1.0f, char_width, metrics.line_height),
              .text_x = cursor_x,
              .text_y = cursor_y,
              .cursor_x = cursor_x,
              .foreground = theme_.text_primary,
              .background = theme_.editor_background,
          };
        }
        if (!active_editor_pane_rect.has_value()) {
          return std::nullopt;
        }
        const editor::EditorViewMetrics metrics =
            editor::EditorViewRenderer::ComputeMetrics(text_renderer_, text_viewport_,
                                                       *active_editor_pane_rect);
        const float cursor_x =
            metrics.text_x +
            static_cast<float>(text_viewport_.cursor_visual_column() - text_viewport_.horizontal_scroll()) *
                char_width;
        const float cursor_y =
            metrics.first_line_y +
            static_cast<float>(text_viewport_.cursor_line() - text_viewport_.scroll_line()) *
                metrics.line_height;
        return TextInputVisual{
            .surface = surface,
            .area = MakeRect(cursor_x, cursor_y - 1.0f, char_width, metrics.line_height),
            .text_x = cursor_x,
            .text_y = cursor_y,
            .cursor_x = cursor_x,
            .foreground = theme_.text_primary,
            .background = theme_.row_highlight,
        };
      }
      case TextInputSurface::Command: {
        const SDL_FRect prompt_rect = BottomPanelCommandPromptRect(layout);
        const float text_x = prompt_rect.x + 6.0f;
        const float text_y = prompt_rect.y + 4.0f;
        const float cursor_x = text_x + text_renderer_.MeasureWidth("> " + command_.input);
        return TextInputVisual{
            .surface = surface,
            .area = MakeRect(text_x, text_y, std::max(1.0f, prompt_rect.w - 12.0f), line_height),
            .text_x = text_x,
            .text_y = text_y,
            .cursor_x = cursor_x,
            .foreground = theme_.text_primary,
            .background = theme_.chrome_active,
        };
      }
      case TextInputSurface::PromptInput: {
        const SDL_FRect dialog = ComputePromptSurfaceRect(layout.full);
        const SDL_FRect input_rect = ComputePromptSurfaceInputRect(dialog);
        const float text_x = input_rect.x + 6.0f;
        const float text_y = input_rect.y + 4.0f;
        const float cursor_x = text_x + text_renderer_.MeasureWidth(prompts_.surface.input);
        return TextInputVisual{
            .surface = surface,
            .area = MakeRect(text_x, text_y, std::max(1.0f, input_rect.w - 12.0f), line_height),
            .text_x = text_x,
            .text_y = text_y,
            .cursor_x = cursor_x,
            .foreground = theme_.text_primary,
            .background = theme_.surface_background,
        };
      }
      case TextInputSurface::FileFinder:
      case TextInputSurface::BufferSearch:
      case TextInputSurface::BufferReplaceSearch:
      case TextInputSurface::BufferReplaceReplace:
      case TextInputSurface::ProjectSearchOverlay:
      case TextInputSurface::CommitPicker: {
        if (!surface_.overlay_visible) {
          return std::nullopt;
        }
        const SDL_FRect overlay = ComputeOverlayRect(layout.editor_area);
        const float inset = 18.0f;
        float text_x = overlay.x + inset;
        float text_y = overlay.y + 44.0f;
        std::string prefix = "> ";
        switch (surface) {
          case TextInputSurface::BufferSearch:
            prefix += overlay_workflow_.buffer_search.query;
            break;
          case TextInputSurface::BufferReplaceSearch:
            prefix = "find: " + overlay_workflow_.buffer_search.query;
            break;
          case TextInputSurface::BufferReplaceReplace:
            text_y = overlay.y + 62.0f;
            prefix = "replace: " + overlay_workflow_.buffer_search.replace_text;
            break;
          case TextInputSurface::ProjectSearchOverlay:
            prefix += overlay_workflow_.project_search.query;
            break;
          case TextInputSurface::CommitPicker:
            text_y = overlay.y + 62.0f;
            prefix += overlay_workflow_.compare_picker.query;
            break;
          case TextInputSurface::FileFinder:
          default:
            prefix += file_finder_.query();
            break;
        }
        const float cursor_x = text_x + text_renderer_.MeasureWidth(prefix);
        return TextInputVisual{
            .surface = surface,
            .area = MakeRect(text_x, text_y, std::max(1.0f, overlay.w - inset * 2.0f), line_height),
            .text_x = text_x,
            .text_y = text_y,
            .cursor_x = cursor_x,
            .foreground = theme_.text_secondary,
            .background = theme_.overlay_background,
        };
      }
      case TextInputSurface::SidebarSearchQuery:
      case TextInputSurface::SidebarSearchReplace: {
        if (!surface_.sidebar_visible || surface_.sidebar_mode != SidebarMode::Search || !overlay_workflow_.project_search.editing) {
          return std::nullopt;
        }
        const float text_x = layout.sidebar.x + kSidebarInset;
        const float text_y =
            layout.sidebar.y + (surface == TextInputSurface::SidebarSearchQuery ? 38.0f : 54.0f);
        const std::string prefix =
            surface == TextInputSurface::SidebarSearchQuery
                ? "search> " + overlay_workflow_.project_search.edit_buffer
                : "replace> " + overlay_workflow_.project_search.edit_buffer;
        const float cursor_x = text_x + text_renderer_.MeasureWidth(prefix);
        return TextInputVisual{
            .surface = surface,
            .area = MakeRect(text_x, text_y, std::max(1.0f, layout.sidebar.w - kSidebarInset * 2.0f),
                             line_height),
            .text_x = text_x,
            .text_y = text_y,
            .cursor_x = cursor_x,
            .foreground = theme_.text_primary,
            .background = theme_.surface_background,
        };
      }
      case TextInputSurface::Terminal:
      case TextInputSurface::None:
      default:
        return std::nullopt;
    }
  }();
  const auto update_text_input_area = [&](const std::optional<TextInputVisual>& visual) {
    if (render_window == nullptr) {
      return;
    }

    if (!visual.has_value()) {
      SDL_SetTextInputArea(render_window, nullptr, 0);
      return;
    }

    float window_x0 = visual->area.x;
    float window_y0 = visual->area.y;
    float window_x1 = visual->area.x + visual->area.w;
    float window_y1 = visual->area.y + visual->area.h;
    float cursor_window_x = visual->cursor_x;
    float cursor_window_y = visual->area.y;
    SDL_RenderCoordinatesToWindow(renderer, visual->area.x, visual->area.y, &window_x0, &window_y0);
    SDL_RenderCoordinatesToWindow(renderer, visual->area.x + visual->area.w,
                                  visual->area.y + visual->area.h, &window_x1, &window_y1);
    SDL_RenderCoordinatesToWindow(renderer, visual->cursor_x, visual->area.y, &cursor_window_x,
                                  &cursor_window_y);

    const SDL_Rect area = SDL_Rect{
        static_cast<int>(std::floor(std::min(window_x0, window_x1))),
        static_cast<int>(std::floor(std::min(window_y0, window_y1))),
        std::max(1, static_cast<int>(std::ceil(std::fabs(window_x1 - window_x0)))),
        std::max(1, static_cast<int>(std::ceil(std::fabs(window_y1 - window_y0)))),
    };
    const int cursor = std::max(
        0, static_cast<int>(std::round(cursor_window_x - static_cast<float>(area.x))));
    SDL_SetTextInputArea(render_window, &area, cursor);
  };

  if (const auto popup = ActiveEditorBlamePopupLayout(); popup.has_value()) {
    const editor::EditorBlameLine* blame_line = VisibleEditorBlameLine(popup->line_index);
    if (blame_line != nullptr) {
      DrawFilledRect(renderer, popup->rect, theme_.overlay_background);
      DrawRect(renderer, popup->rect, theme_.border);
      const float text_x = popup->rect.x + 12.0f;
      const float text_width = std::max(0.0f, popup->rect.w - 24.0f);
      const auto summary_lines =
          WrapEditorBlamePopupText(blame_line->summary, text_width, 4);
      float text_y = popup->rect.y + 12.0f;
      draw_text_on(text_x, text_y, theme_.text_primary, theme_.overlay_background,
                   text_renderer_.TruncateToWidth(blame_line->author, text_width));
      text_y += text_renderer_.LineHeight() + 2.0f;
      draw_text_on(text_x, text_y, theme_.text_secondary, theme_.overlay_background,
                   text_renderer_.TruncateToWidth(blame_line->date, text_width));
      if (!summary_lines.empty()) {
        text_y += text_renderer_.LineHeight() + 8.0f;
        for (std::size_t i = 0; i < summary_lines.size(); ++i) {
          draw_text_on(text_x, text_y, theme_.text_primary, theme_.overlay_background,
                       summary_lines[i]);
          text_y += text_renderer_.LineHeight();
          if (i + 1 < summary_lines.size()) {
            text_y += 2.0f;
          }
        }
      }

      const bool copy_hovered =
          last_mouse_position_valid_ && EditorBlamePopupCopyShaHovered(last_mouse_x_, last_mouse_y_);
      DrawFilledRect(renderer, popup->copy_sha_rect,
                     copy_hovered ? theme_.row_highlight : theme_.surface_raised);
      DrawRect(renderer, popup->copy_sha_rect, copy_hovered ? theme_.accent : theme_.border);
      draw_text_on(popup->copy_sha_rect.x + 9.0f, popup->copy_sha_rect.y + 4.0f,
                   copy_hovered ? theme_.text_primary : theme_.text_secondary,
                   copy_hovered ? theme_.row_highlight : theme_.surface_raised, "Copy SHA");
    }
  }
  const auto render_text_composition = [&](const std::optional<TextInputVisual>& visual) {
    if (!visual.has_value() || text_composition_.text.empty() ||
        text_composition_.surface != visual->surface) {
      return;
    }

    const std::string_view composition = text_composition_.text;
    const std::size_t total_codepoints = Utf8CodepointCount(composition);
    const std::size_t selection_start_codepoints =
        text_composition_.start < 0
            ? total_codepoints
            : std::min<std::size_t>(static_cast<std::size_t>(text_composition_.start),
                                    total_codepoints);
    const std::size_t selection_end_codepoints =
        text_composition_.length <= 0
            ? selection_start_codepoints
            : std::min(total_codepoints,
                       selection_start_codepoints +
                           static_cast<std::size_t>(text_composition_.length));
    const std::size_t selection_start =
        Utf8ByteOffsetForCodepointCount(composition, selection_start_codepoints);
    const std::size_t selection_end =
        Utf8ByteOffsetForCodepointCount(composition, selection_end_codepoints);
    const std::string_view prefix = composition.substr(0, selection_start);
    const std::string_view selected =
        composition.substr(selection_start, selection_end - selection_start);
    const std::string_view suffix = composition.substr(selection_end);
    const float prefix_width = text_renderer_.MeasureWidth(prefix);
    const float selected_width = text_renderer_.MeasureWidth(selected);
    const float total_width = text_renderer_.MeasureWidth(composition);

    if (!selected.empty()) {
      DrawFilledRect(renderer,
                     MakeRect(visual->cursor_x + prefix_width, visual->text_y - 1.0f, selected_width,
                              text_renderer_.LineHeight()),
                     theme_.selection_fill);
    }

    float segment_x = visual->cursor_x;
    if (!prefix.empty()) {
      text_renderer_.DrawStringOn(renderer, segment_x, visual->text_y, theme_.accent,
                                  visual->background, prefix);
      segment_x += prefix_width;
    }
    if (!selected.empty()) {
      text_renderer_.DrawStringOn(renderer, segment_x, visual->text_y, theme_.text_primary,
                                  theme_.selection_fill, selected);
      segment_x += selected_width;
    }
    if (!suffix.empty()) {
      text_renderer_.DrawStringOn(renderer, segment_x, visual->text_y, theme_.accent,
                                  visual->background, suffix);
    }

    DrawFilledRect(renderer,
                   MakeRect(visual->cursor_x, visual->text_y + text_renderer_.LineHeight() - 1.0f,
                            total_width, 1.0f),
                   theme_.accent);
    DrawFilledRect(renderer,
                   MakeRect(visual->cursor_x + prefix_width + selected_width, visual->text_y - 1.0f,
                            1.5f, text_renderer_.LineHeight()),
                   theme_.accent);
  };
  const auto draw_vertical_scrollbar =
      [&](const SDL_FRect& area, float total_units, float visible_units, float scroll_units,
          bool active = false, bool reserve_horizontal = false) {
        if (const auto geometry = MakeVerticalScrollbarGeometry(area, total_units, visible_units,
                                                                scroll_units, reserve_horizontal);
            geometry.has_value()) {
          DrawScrollbar(renderer, theme_, geometry->track, geometry->thumb, active);
        }
      };
  const auto draw_horizontal_scrollbar =
      [&](const SDL_FRect& area, float total_units, float visible_units, float scroll_units,
          bool active = false, bool reserve_vertical = false) {
        if (const auto geometry = MakeHorizontalScrollbarGeometry(area, total_units, visible_units,
                                                                  scroll_units, reserve_vertical);
            geometry.has_value()) {
          DrawScrollbar(renderer, theme_, geometry->track, geometry->thumb, active);
        }
      };
  const auto terminal_styles_equal = [](const terminal::TerminalStyle& lhs,
                                        const terminal::TerminalStyle& rhs) {
    const auto colors_equal = [](const std::optional<SDL_Color>& left,
                                 const std::optional<SDL_Color>& right) {
      return left.has_value() == right.has_value() &&
             (!left.has_value() ||
              (left->r == right->r && left->g == right->g && left->b == right->b &&
               left->a == right->a));
    };
    return colors_equal(lhs.foreground, rhs.foreground) &&
           colors_equal(lhs.background, rhs.background) && lhs.bold == rhs.bold;
  };
  const auto draw_terminal_line =
      [&](float x, float y, float width, const terminal::TerminalLine& line, std::size_t row_index) {
    if (width <= 0.0f || line.cells.empty()) {
      return;
    }

    const float char_width = std::max(1.0f, text_renderer_.CharWidth());
    const std::size_t max_chars =
        std::max<std::size_t>(1, static_cast<std::size_t>(std::floor(width / char_width)));
    std::size_t drawn_chars = 0;
    std::size_t segment_start = 0;
    std::string segment;
    terminal::TerminalStyle segment_style;
    bool has_segment = false;
    bool segment_selected = false;

    const auto flush_segment = [&]() {
      if (!has_segment || segment.empty()) {
        return;
      }
      const SDL_Color foreground =
          segment_selected ? theme_.text_primary
                           : segment_style.foreground.value_or(theme_.text_muted);
      const SDL_Color background =
          segment_selected ? theme_.row_highlight
                           : segment_style.background.value_or(theme_.surface_background);
      const float segment_x = x + static_cast<float>(segment_start) * char_width;
      text_renderer_.DrawStringOn(renderer, segment_x, y, foreground, background, segment);
      segment.clear();
      has_segment = false;
    };

    for (std::size_t column = 0; column < line.cells.size(); ++column) {
      if (drawn_chars >= max_chars) {
        break;
      }
      const auto& cell = line.cells[column];
      const bool selected = TerminalCellSelected(row_index, column);
      if (!has_segment) {
        segment_start = column;
        segment_style = cell.style;
        segment_selected = selected;
        has_segment = true;
      } else if (!terminal_styles_equal(segment_style, cell.style) ||
                 segment_selected != selected) {
        flush_segment();
        segment_start = column;
        segment_style = cell.style;
        segment_selected = selected;
        has_segment = true;
      }
      segment.push_back(cell.character);
      ++drawn_chars;
    }
    flush_segment();
  };
  const auto draw_vcentered_text_on = [&](const SDL_FRect& rect,
                                          float left_padding,
                                          SDL_Color foreground,
                                          SDL_Color background,
                                          std::string_view text) {
    (void) background;
    const float y = rect.y + std::floor(std::max(0.0f, rect.h - text_renderer_.LineHeight()) * 0.5f);
    draw_text(rect.x + left_padding, y, foreground, text);
  };
  const auto draw_centered_text_on = [&](const SDL_FRect& rect,
                                         SDL_Color foreground,
                                         SDL_Color background,
                                         std::string_view text) {
    (void) background;
    const float text_width = text_renderer_.MeasureWidth(text);
    const float x = rect.x + std::floor(std::max(0.0f, rect.w - text_width) * 0.5f);
    const float y = rect.y + std::floor(std::max(0.0f, rect.h - text_renderer_.LineHeight()) * 0.5f);
    draw_text(x, y, foreground, text);
  };
  const auto draw_tab_close_button = [&](const SDL_FRect& rect,
                                         SDL_Color color,
                                         SDL_Color hover_color) {
    const bool hovered = last_mouse_position_valid_ && Contains(rect, last_mouse_x_, last_mouse_y_);
    DrawCloseGlyph(renderer, rect, hovered ? hover_color : color);
  };

  const auto visible_menu_items = ComputeVisibleMenuBarItems(layout.menu_bar);
  const auto window_buttons = ComputeVisibleWindowControlButtons(layout.menu_bar);
  for (const VisibleMenuBarItem& item : visible_menu_items) {
    const MenuSpec* menu = FindMenuSpec(item.id);
    if (menu == nullptr) {
      continue;
    }
    const SDL_Color background = item.active ? theme_.chrome_active : theme_.chrome_background;
    DrawFilledRect(renderer, item.rect, background);
    if (item.active) {
      DrawFilledRect(renderer,
                     MakeRect(item.rect.x, item.rect.y + item.rect.h - 2.0f, item.rect.w, 2.0f),
                     theme_.accent);
    }
    draw_vcentered_text_on(item.rect, 10.0f,
                           item.active ? theme_.text_primary : theme_.text_secondary, background,
                           menu->label);
  }

  if (custom_window_chrome_enabled_) {
    const std::string title = "microide";
    const float title_width = text_renderer_.MeasureWidth(title);
    const float left_limit =
        visible_menu_items.empty() ? layout.menu_bar.x + 12.0f
                                   : visible_menu_items.back().rect.x +
                                         visible_menu_items.back().rect.w + 16.0f;
    const float right_limit =
        window_buttons.empty() ? layout.menu_bar.x + layout.menu_bar.w - 12.0f
                               : window_buttons.front().rect.x - 16.0f;
    const float title_x =
        std::floor(layout.menu_bar.x + (layout.menu_bar.w - title_width) * 0.5f);
    if (title_x >= left_limit && title_x + title_width <= right_limit) {
      draw_vcentered_text_on(MakeRect(title_x, layout.menu_bar.y, title_width, layout.menu_bar.h),
                             0.0f, theme_.text_muted, theme_.chrome_background, title);
    }
  }

  for (const VisibleWindowControlButton& button : window_buttons) {
    SDL_Color background = button.hovered ? theme_.row_highlight : theme_.chrome_background;
    SDL_Color glyph = button.hovered ? theme_.text_primary : theme_.text_secondary;
    if (button.id == WindowControlButtonId::Close && button.hovered) {
      background = theme_.diff_deleted;
      glyph = theme_.text_primary;
    }

    DrawFilledRect(renderer, button.rect, background);
    DrawWindowControlGlyph(renderer, button.rect, button.id, glyph, window_maximized_);
  }

  if (ActiveTabIsCompare()) {
    CompareTabState* compare_tab = ActiveCompareTab();
    if (compare_tab != nullptr) {
      const CompareSurfaceLayout surface_layout =
          ComputeCompareSurfaceLayout(layout.editor_surface, *compare_tab);
      ClampCompareScrollRow(*compare_tab, surface_layout.visible_rows);
      ClampCompareHorizontalScroll(*compare_tab, surface_layout.visible_columns);
      if (const auto geometry =
              MakeVerticalScrollbarGeometry(layout.editor_surface,
                                            static_cast<float>(compare_tab->model.rows.size()),
                                            static_cast<float>(surface_layout.visible_rows),
                                            static_cast<float>(compare_tab->scroll_row),
                                            surface_layout.show_horizontal);
          geometry.has_value()) {
        const SDL_FRect marker_lane = MakeRect(
            std::max(layout.editor_surface.x,
                     geometry->track.x - kCompareMarkerLaneGap - kCompareMarkerLaneWidth),
            geometry->track.y, kCompareMarkerLaneWidth, geometry->track.h);
        const SDL_FRect marker_inner_lane =
            MakeRect(marker_lane.x + 1.0f, marker_lane.y + 1.0f,
                     std::max(0.0f, marker_lane.w - 2.0f),
                     std::max(0.0f, marker_lane.h - 2.0f));
        DrawFilledRect(renderer, marker_lane, theme_.surface_raised);
        DrawRect(renderer, marker_lane, theme_.border);
        DrawCompareScrollbarMarkers(renderer, theme_, marker_inner_lane, compare_tab->model);
        DrawScrollbarTrack(renderer, theme_, geometry->track);
        DrawScrollbarThumb(renderer, theme_, geometry->thumb,
                           surface_.drag_target == DragTarget::CompareVerticalScrollbar);
      }
      if (surface_layout.show_horizontal) {
        draw_horizontal_scrollbar(
            layout.editor_surface, static_cast<float>(compare_tab->max_visual_columns),
            static_cast<float>(surface_layout.visible_columns),
            static_cast<float>(compare_tab->horizontal_scroll),
            surface_.drag_target == DragTarget::CompareHorizontalScrollbar, surface_layout.show_vertical);
      }
    }
  } else if (ActiveTabIsMerge()) {
    MergeTabState* merge_tab = ActiveMergeTab();
    if (merge_tab != nullptr) {
      const MergeSurfaceLayout surface_layout =
          ComputeMergeSurfaceLayout(layout.editor_surface, *merge_tab);
      ClampMergeScrollRow(*merge_tab, surface_layout.visible_rows);
      ClampMergeHorizontalScroll(*merge_tab, surface_layout.visible_columns);
      const std::size_t line_count =
          std::max({merge_tab->model.incoming_lines.size(), merge_tab->result_viewport.line_count(),
                    merge_tab->model.current_lines.size(), std::size_t{1}});
      if (const auto geometry =
              MakeVerticalScrollbarGeometry(layout.editor_surface, static_cast<float>(line_count),
                                            static_cast<float>(surface_layout.visible_rows),
                                            static_cast<float>(merge_tab->scroll_row),
                                            surface_layout.show_horizontal);
          geometry.has_value()) {
        const SDL_FRect marker_lane = MakeRect(
            std::max(layout.editor_surface.x,
                     geometry->track.x - kCompareMarkerLaneGap - kCompareMarkerLaneWidth),
            geometry->track.y, kCompareMarkerLaneWidth, geometry->track.h);
        const SDL_FRect marker_inner_lane =
            MakeRect(marker_lane.x + 1.0f, marker_lane.y + 1.0f,
                     std::max(0.0f, marker_lane.w - 2.0f),
                     std::max(0.0f, marker_lane.h - 2.0f));
        std::vector<MergeScrollbarMarkerInput> inputs;
        inputs.reserve(merge_tab->conflicts.size());
        for (const auto& conflict : merge_tab->conflicts) {
          const int start_row = static_cast<int>(std::min(
              {conflict.incoming_start_line, conflict.start_line, conflict.current_start_line}));
          const int end_row = static_cast<int>(std::max(
              {std::max(conflict.incoming_end_line, conflict.incoming_start_line + 1),
               std::max(conflict.end_line, conflict.start_line + 1),
               std::max(conflict.current_end_line, conflict.current_start_line + 1)}));
          inputs.push_back(MergeScrollbarMarkerInput{
              .start_row = start_row,
              .end_row = end_row,
              .choice = conflict.last_choice,
              .valid = conflict.valid,
          });
        }
        DrawFilledRect(renderer, marker_lane, theme_.surface_raised);
        DrawRect(renderer, marker_lane, theme_.border);
        DrawMergeScrollbarMarkers(renderer, theme_, marker_inner_lane, line_count, inputs);
        DrawScrollbarTrack(renderer, theme_, geometry->track);
        DrawScrollbarThumb(renderer, theme_, geometry->thumb,
                           surface_.drag_target == DragTarget::CompareVerticalScrollbar);
      }
      if (surface_layout.show_horizontal) {
        draw_horizontal_scrollbar(
            layout.editor_surface, static_cast<float>(merge_tab->max_visual_columns),
            static_cast<float>(surface_layout.visible_columns),
            static_cast<float>(merge_tab->horizontal_scroll),
            surface_.drag_target == DragTarget::CompareHorizontalScrollbar, surface_layout.show_vertical);
      }
    }
  } else {
    const auto panes = ComputeEditorPaneLayouts(layout.editor_surface);
    auto* editor_tab = ActiveEditorTab();
    for (const EditorPaneLayout& pane : panes) {
      editor::TextViewport* viewport =
          pane.active ? &text_viewport_
                      : (editor_tab != nullptr ? FindEditorView(*editor_tab, pane.leaf_id)
                                               : nullptr);
      if (viewport == nullptr || viewport->is_placeholder()) {
        continue;
      }

      const editor::EditorViewMetrics metrics =
          editor::EditorViewRenderer::ComputeMetrics(text_renderer_, *viewport, pane.rect);
      viewport->SetViewportSize(metrics.visible_rows, metrics.visible_columns);
      const std::size_t total_columns =
          std::max<std::size_t>(viewport->visible_columns(), MaxVisualColumns(*viewport));
      const bool show_vertical = viewport->line_count() > viewport->visible_lines();
      const bool show_horizontal = total_columns > viewport->visible_columns();
      if (show_vertical) {
        draw_vertical_scrollbar(pane.rect, static_cast<float>(viewport->line_count()),
                                static_cast<float>(viewport->visible_lines()),
                                static_cast<float>(viewport->scroll_line()),
                                pane.active && surface_.drag_target == DragTarget::EditorVerticalScrollbar,
                                show_horizontal);
      }
      if (show_horizontal) {
        draw_horizontal_scrollbar(
            pane.rect, static_cast<float>(total_columns),
            static_cast<float>(viewport->visible_columns()),
            static_cast<float>(viewport->horizontal_scroll()),
            pane.active && surface_.drag_target == DragTarget::EditorHorizontalScrollbar, show_vertical);
      }
    }
    for (const EditorSplitDividerLayout& divider :
         ComputeEditorSplitDividerLayouts(layout.editor_surface)) {
      const bool divider_active =
          surface_.drag_target == DragTarget::EditorSplitDivider &&
          divider.divider_index == surface_.drag_editor_split_divider_index &&
          divider.node_path == surface_.drag_editor_split_path;
      DrawFilledRect(renderer, divider.rect, divider_active ? theme_.accent : theme_.border);
    }
  }

  for (const VisibleProjectTab& tab : ComputeVisibleProjectTabs(layout.project_tab_strip)) {
    DrawFilledRect(renderer, tab.rect, tab.active ? theme_.chrome_active : theme_.surface_raised);
    if (tab.active) {
      DrawFilledRect(renderer, MakeRect(tab.rect.x, tab.rect.y, tab.rect.w, 2.0f), theme_.accent);
    }
    draw_vcentered_text_on(tab.rect, 10.0f, tab.active ? theme_.text_primary : theme_.text_secondary,
                           tab.active ? theme_.chrome_active : theme_.surface_raised,
                           TruncateLabel(tab.display_title, tab.rect.w - 46.0f));
    draw_tab_close_button(tab.close_rect,
                          tab.active ? theme_.text_secondary : theme_.text_disabled,
                          tab.active ? theme_.text_primary : theme_.text_secondary);
  }

  if (!project_root_.empty() && open_tabs_.empty()) {
    const SDL_FRect placeholder_tab =
        MakeRect(layout.tab_strip.x + 12.0f, layout.tab_strip.y + 2.0f, 220.0f,
                 std::max(22.0f, layout.tab_strip.h - 2.0f));
    DrawFilledRect(renderer, placeholder_tab, theme_.chrome_active);
    DrawFilledRect(renderer, MakeRect(placeholder_tab.x, placeholder_tab.y, placeholder_tab.w, 2.0f),
                   theme_.accent);
    draw_vcentered_text_on(placeholder_tab, 10.0f, theme_.text_primary, theme_.chrome_active,
                           "welcome");
  } else if (!project_root_.empty()) {
    for (const VisibleTab& tab : ComputeVisibleTabs(layout.tab_strip)) {
      DrawFilledRect(renderer, tab.rect, tab.active ? theme_.chrome_active : theme_.surface_raised);
      if (tab.active) {
        DrawFilledRect(renderer, MakeRect(tab.rect.x, tab.rect.y, tab.rect.w, 2.0f),
                       theme_.accent);
      }
      draw_vcentered_text_on(tab.rect, 10.0f, tab.active ? theme_.text_primary : theme_.text_secondary,
                             tab.active ? theme_.chrome_active : theme_.surface_raised,
                             TruncateLabel(tab.display_title, tab.rect.w - 46.0f));
      draw_tab_close_button(tab.close_rect,
                            tab.active ? theme_.text_secondary : theme_.text_disabled,
                            tab.active ? theme_.text_primary : theme_.text_secondary);
    }
  }

  const std::string hovered_tab_tooltip = HoveredTabTooltipLabel(layout.tab_strip);
  const float breadcrumb_text_x = layout.breadcrumb.x + 12.0f;
  const float breadcrumb_text_width =
      std::max(0.0f, layout.breadcrumb.x + layout.breadcrumb.w - 12.0f - breadcrumb_text_x);
  draw_vcentered_text_on(
      MakeRect(breadcrumb_text_x, layout.breadcrumb.y, breadcrumb_text_width, layout.breadcrumb.h),
      0.0f, theme_.text_primary, theme_.chrome_background,
      TruncateLabel(BreadcrumbLabel(), breadcrumb_text_width));
  if (!hovered_tab_tooltip.empty()) {
    const float max_tooltip_width = std::max(160.0f, layout.full.w - 24.0f);
    const std::string tooltip_text =
        text_renderer_.TruncateToWidth(hovered_tab_tooltip, max_tooltip_width - 16.0f);
    const float tooltip_width =
        std::min(max_tooltip_width, text_renderer_.MeasureWidth(tooltip_text) + 16.0f);
    const float tooltip_height = text_renderer_.LineHeight() + 10.0f;
    const float tooltip_x =
        std::clamp(last_mouse_x_ + 12.0f, layout.full.x + 8.0f,
                   layout.full.x + layout.full.w - tooltip_width - 8.0f);
    const SDL_FRect tooltip_rect = MakeRect(tooltip_x, layout.tab_strip.y + layout.tab_strip.h + 6.0f,
                                            tooltip_width, tooltip_height);
    DrawFilledRect(renderer, tooltip_rect, theme_.surface_raised);
    DrawRect(renderer, tooltip_rect, theme_.border);
    draw_vcentered_text_on(tooltip_rect, 8.0f, theme_.text_primary, theme_.surface_raised,
                           tooltip_text);
  }

  if (surface_.sidebar_visible) {
    const SDL_FRect sidebar_mode_rect = SidebarModeControlRect(layout.sidebar);
    const bool sidebar_mode_hovered =
        last_mouse_position_valid_ && Contains(sidebar_mode_rect, last_mouse_x_, last_mouse_y_);
    const bool sidebar_mode_open =
        surface_.menu_bar_open && surface_.active_menu_id == MenuId::SidebarMode && surface_.active_menu_anchor_rect.has_value();
    DrawFilledRect(renderer, sidebar_mode_rect,
                   sidebar_mode_open || sidebar_mode_hovered ? theme_.row_highlight
                                                            : theme_.surface_raised);
    DrawRect(renderer, sidebar_mode_rect,
             sidebar_mode_open ? theme_.accent
                               : sidebar_mode_hovered ? theme_.text_secondary : theme_.border);
    draw_vcentered_text_on(sidebar_mode_rect, 8.0f,
                           sidebar_mode_open || sidebar_mode_hovered ? theme_.text_primary
                                                                     : theme_.text_secondary,
                           sidebar_mode_open || sidebar_mode_hovered ? theme_.row_highlight
                                                                     : theme_.surface_raised,
                           SidebarModeControlLabel());
    DrawChevron(renderer, sidebar_mode_rect.x + sidebar_mode_rect.w - 18.0f,
                sidebar_mode_rect.y + sidebar_mode_rect.h * 0.5f, true,
                sidebar_mode_open || sidebar_mode_hovered ? theme_.text_primary : theme_.text_muted);

    if (surface_.sidebar_mode == SidebarMode::Search) {
      const std::string active_query =
          overlay_workflow_.project_search.editing && overlay_workflow_.project_search.edit_field == ProjectSearchEditField::Query
              ? overlay_workflow_.project_search.edit_buffer
              : overlay_workflow_.project_search.query;
      const std::string active_replace =
          overlay_workflow_.project_search.editing && overlay_workflow_.project_search.edit_field == ProjectSearchEditField::Replace
              ? overlay_workflow_.project_search.edit_buffer
              : overlay_workflow_.project_search.replace_text;
      draw_text_on(layout.sidebar.x + kSidebarInset, layout.sidebar.y + 38.0f,
                   overlay_workflow_.project_search.editing &&
                           overlay_workflow_.project_search.edit_field == ProjectSearchEditField::Query
                       ? theme_.text_primary
                       : theme_.text_secondary,
                   theme_.surface_background,
                   TruncateLabel("search> " + active_query,
                                 layout.sidebar.w - kSidebarInset * 2.0f));
      draw_text_on(layout.sidebar.x + kSidebarInset, layout.sidebar.y + 54.0f,
                   overlay_workflow_.project_search.editing &&
                           overlay_workflow_.project_search.edit_field == ProjectSearchEditField::Replace
                       ? theme_.text_primary
                       : theme_.text_secondary,
                   theme_.surface_background,
                   TruncateLabel("replace> " + active_replace,
                                 layout.sidebar.w - kSidebarInset * 2.0f));
      const auto draw_search_button = [&](const SDL_FRect& rect,
                                          std::string_view label,
                                          bool active) {
        const bool hovered =
            last_mouse_position_valid_ && Contains(rect, last_mouse_x_, last_mouse_y_);
        const SDL_Color background =
            active ? (hovered ? theme_.row_highlight : theme_.chrome_active)
                   : (hovered ? theme_.row_highlight : theme_.surface_raised);
        const SDL_Color border = active ? theme_.accent : (hovered ? theme_.text_secondary
                                                                   : theme_.border);
        const SDL_Color text = active || hovered ? theme_.text_primary : theme_.text_secondary;
        DrawFilledRect(renderer, rect, background);
        DrawRect(renderer, rect, border);
        draw_centered_text_on(rect, text, background, label);
      };

      draw_search_button(ProjectSearchModeButtonRect(layout.sidebar), ProjectSearchModeButtonLabel(),
                         overlay_workflow_.project_search.options.pattern_mode ==
                             project::ProjectSearchPatternMode::Regex);
      draw_search_button(ProjectSearchCaseButtonRect(layout.sidebar), ProjectSearchCaseButtonLabel(),
                         overlay_workflow_.project_search.options.case_mode !=
                             project::ProjectSearchCaseMode::Smart);
      draw_search_button(ProjectSearchHiddenButtonRect(layout.sidebar),
                         ProjectSearchHiddenButtonLabel(), overlay_workflow_.project_search.options.show_hidden);

      const std::string match_actions =
          ProjectSearchCanReplaceAll()
              ? "/ query  = replace  r rerun  R replace all"
              : "/ query  = replace  r rerun  R needs literal mode";
      const std::string status_text =
          overlay_workflow_.project_search.editing
              ? (overlay_workflow_.project_search.edit_field == ProjectSearchEditField::Query
                     ? "Editing query  |  Enter apply  Esc cancel"
                     : "Editing replace  |  Enter apply  Esc cancel")
          : !overlay_workflow_.project_search.error.empty()
              ? "Error  |  / query  = replace  r rerun"
          : overlay_workflow_.project_search.running
              ? "Searching " + std::to_string(overlay_workflow_.project_search.results.size()) + " matches"
          : overlay_workflow_.project_search.results.empty()
              ? (overlay_workflow_.project_search.query.empty()
                     ? "/ query  = replace  |  buttons change mode, case, hidden"
                     : "No matches  |  " + match_actions)
          : overlay_workflow_.project_search.truncated
              ? "Showing first " + std::to_string(overlay_workflow_.project_search.results.size()) +
                    " matches  |  " + match_actions
              : std::to_string(overlay_workflow_.project_search.results.size()) + " matches  |  " + match_actions;
      draw_text_on(layout.sidebar.x + kSidebarInset, layout.sidebar.y + kProjectSearchStatusTop,
                   theme_.text_muted,
                   theme_.surface_background,
                   TruncateLabel(status_text, layout.sidebar.w - kSidebarInset * 2.0f));

      const float list_y = layout.sidebar.y + kProjectSearchResultsTop;
      const auto line_map = BuildProjectSearchLineMap();
      const int visible_rows =
          std::max(1, static_cast<int>((layout.sidebar.h - kProjectSearchResultsTop) / kSidebarRowHeight));
      const int max_scroll = std::max(0, static_cast<int>(line_map.size()) - visible_rows);
      const float row_width =
          std::max(0.0f, layout.sidebar.w - kSidebarInset * 2.0f -
                             (max_scroll > 0 ? kScrollbarThickness + 6.0f : 0.0f));
      int scroll_row = std::clamp(surface_.sidebar_scroll_row, 0, max_scroll);
      const int selected_line = ProjectSearchLineForResult(overlay_workflow_.project_search.selected_index);
      if (selected_line < scroll_row) {
        scroll_row = selected_line;
      } else if (selected_line >= scroll_row + visible_rows) {
        scroll_row = selected_line - visible_rows + 1;
      }
      surface_.sidebar_scroll_row = scroll_row;

      for (int row = 0; row < visible_rows; ++row) {
        const int line_index = scroll_row + row;
        if (line_index >= static_cast<int>(line_map.size())) {
          break;
        }

        SDL_FRect row_rect = MakeRect(
            layout.sidebar.x + kSidebarInset,
            list_y + static_cast<float>(row) * kSidebarRowHeight,
            row_width,
            kSidebarRowHeight - 2.0f);

        const int result_index = line_map[static_cast<std::size_t>(line_index)];
        if (result_index < 0) {
          const std::size_t next_result_index =
              static_cast<std::size_t>(std::min(line_index + 1, static_cast<int>(line_map.size()) - 1));
          const auto& file_result =
              overlay_workflow_.project_search.results[static_cast<std::size_t>(line_map[next_result_index])];
          draw_vcentered_text_on(row_rect, 4.0f, theme_.text_primary, theme_.surface_background,
                                 TruncateLabel(file_result.relative_path.string(), row_rect.w - 8.0f));
          continue;
        }

        const auto& result = overlay_workflow_.project_search.results[static_cast<std::size_t>(result_index)];
        const bool selected = static_cast<std::size_t>(result_index) == overlay_workflow_.project_search.selected_index;
        if (selected) {
          DrawFilledRect(renderer, row_rect, theme_.row_highlight);
        }

        const std::string snippet = CollapseWhitespace(result.preview);
        const std::string label =
            std::to_string(result.line + 1) + ":" + std::to_string(result.column + 1) + "  " + snippet;
        draw_vcentered_text_on(row_rect, 6.0f,
                               selected ? theme_.text_primary : theme_.text_secondary,
                               selected ? theme_.row_highlight : theme_.surface_background,
                               TruncateLabel(label, row_rect.w - 12.0f));
      }

      if (line_map.empty()) {
        const std::string placeholder = !overlay_workflow_.project_search.error.empty()
                                            ? "Error: " + overlay_workflow_.project_search.error
                                        : overlay_workflow_.project_search.running
                                            ? "Searching..."
                                        : overlay_workflow_.project_search.query.empty() ? "Project search is idle"
                                                                        : "No matches";
        draw_text_on(layout.sidebar.x + kSidebarInset, list_y + 4.0f, theme_.text_muted,
                     theme_.surface_background,
                     TruncateLabel(placeholder, layout.sidebar.w - kSidebarInset * 2.0f));
      }

      draw_vertical_scrollbar(
          MakeRect(layout.sidebar.x, list_y, layout.sidebar.w,
                   std::max(0.0f, layout.sidebar.y + layout.sidebar.h - list_y)),
          static_cast<float>(line_map.size()), static_cast<float>(visible_rows),
          static_cast<float>(scroll_row), surface_.drag_target == DragTarget::SidebarScrollbar);
    } else if (surface_.sidebar_mode == SidebarMode::Git) {
      const auto draw_action_button = [&](const SDL_FRect& button_rect,
                                          std::string_view label,
                                          bool enabled,
                                          bool destructive = false) {
        const bool hovered =
            enabled && last_mouse_position_valid_ && Contains(button_rect, last_mouse_x_, last_mouse_y_);
        const SDL_Color fill = hovered ? theme_.row_highlight : theme_.surface_raised;
        const SDL_Color border = !enabled   ? theme_.border
                                 : hovered ? (destructive ? theme_.diff_deleted : theme_.accent)
                                           : theme_.border;
        const SDL_Color text = !enabled   ? theme_.text_muted
                               : hovered ? theme_.text_primary
                                         : destructive ? theme_.diff_deleted : theme_.accent;
        DrawFilledRect(renderer, button_rect, fill);
        DrawRect(renderer, button_rect, border);
        draw_centered_text_on(button_rect, text, fill, label);
      };

      draw_action_button(GitSidebarStageAllButtonRect(layout.sidebar), "Stage All",
                         CanStageAllGitSidebarEntries());
      draw_action_button(GitSidebarDiscardAllButtonRect(layout.sidebar), "Discard All",
                         CanDiscardAllGitSidebarEntries(), true);
      const SDL_FRect refresh_rect = GitSidebarRefreshButtonRect(layout.sidebar);
      draw_action_button(refresh_rect, "Refresh", true);
      const auto lines = BuildGitSidebarLines();
      const float list_y = GitSidebarListTop(layout.sidebar);
      const float visible_units = GitSidebarVisibleUnits(layout.sidebar);
      const int visible_rows = std::max(1, static_cast<int>(std::floor(visible_units)));
      const int max_scroll = std::max(
          0, static_cast<int>(std::ceil(static_cast<float>(lines.size()) - visible_units)));
      const float row_width =
          std::max(0.0f, layout.sidebar.w - kSidebarInset * 2.0f -
                             (max_scroll > 0 ? kScrollbarThickness + 6.0f : 0.0f));
      int scroll_row = std::clamp(surface_.sidebar_scroll_row, 0, max_scroll);
      surface_.sidebar_scroll_row = scroll_row;

      for (int row = 0; row < visible_rows; ++row) {
        const int line_index = scroll_row + row;
        if (line_index >= static_cast<int>(lines.size())) {
          break;
        }

        const auto& line = lines[static_cast<std::size_t>(line_index)];
        SDL_FRect row_rect = MakeRect(layout.sidebar.x + kSidebarInset,
                                      list_y + static_cast<float>(row) * kSidebarRowHeight, row_width,
                                      kSidebarRowHeight - 2.0f);

        if (line.kind == GitSidebarLine::Kind::Header) {
          draw_vcentered_text_on(row_rect, 4.0f, theme_.text_muted, theme_.surface_background,
                                 TruncateLabel(line.label, row_rect.w - 8.0f));
          continue;
        }
        if (line.kind == GitSidebarLine::Kind::Empty || line.entry_index < 0) {
          draw_vcentered_text_on(row_rect, 4.0f, theme_.text_muted, theme_.surface_background,
                                 TruncateLabel(line.label, row_rect.w - 8.0f));
          continue;
        }

        const auto& entry = git_sidebar_.entries[static_cast<std::size_t>(line.entry_index)];
        const bool selected = static_cast<std::size_t>(line.entry_index) == git_sidebar_.selected_index;
        if (selected) {
          DrawFilledRect(renderer, row_rect, theme_.row_highlight);
          DrawFilledRect(renderer, MakeRect(row_rect.x, row_rect.y, 2.0f, row_rect.h),
                         theme_.accent);
        }

        const char git_marker = GitMarker(entry.status);
        const std::string marker_text = git_marker == ' ' ? "" : std::string(1, git_marker);
        const float marker_width = marker_text.empty() ? 0.0f : text_renderer_.MeasureWidth(marker_text);
        float right_edge = row_rect.x + row_rect.w - 8.0f;

        const auto draw_button = [&](const SDL_FRect& button_rect,
                                     std::string_view label,
                                     SDL_Color text_color) {
          DrawRect(renderer, button_rect, selected ? theme_.accent : theme_.border);
          draw_centered_text_on(button_rect, text_color,
                                selected ? theme_.row_highlight : theme_.surface_background,
                                label);
        };

        if (entry.section == GitSidebarEntry::Section::Modified) {
          const std::string_view stage_label = entry.staged ? "Unstage" : "Stage";
          const float stage_width =
              std::max(entry.staged ? 68.0f : 48.0f,
                       text_renderer_.MeasureWidth(stage_label) + 16.0f);
          const SDL_FRect stage_rect =
              MakeRect(right_edge - stage_width, row_rect.y + 1.0f, stage_width, row_rect.h - 2.0f);
          draw_button(stage_rect, stage_label, selected ? theme_.text_primary : theme_.accent);
          right_edge = stage_rect.x - 6.0f;
          const float discard_width =
              std::max(62.0f, text_renderer_.MeasureWidth("Discard") + 16.0f);
          const SDL_FRect discard_rect = MakeRect(right_edge - discard_width, row_rect.y + 1.0f,
                                                  discard_width, row_rect.h - 2.0f);
          draw_button(discard_rect, "Discard",
                      selected ? theme_.text_primary : theme_.diff_deleted);
          right_edge = discard_rect.x - 6.0f;
        }

        if (!marker_text.empty()) {
          draw_vcentered_text_on(
              MakeRect(right_edge - marker_width, row_rect.y, marker_width, row_rect.h), 0.0f,
              selected ? theme_.text_primary : GitMarkerColor(theme_, entry.status),
              selected ? theme_.row_highlight : theme_.surface_background, marker_text);
          right_edge -= marker_width + 8.0f;
        }

        const std::string label = entry.relative_path.string() + (entry.staged ? "  [staged]" : "");
        draw_vcentered_text_on(
            row_rect, 6.0f, selected ? theme_.text_primary : theme_.text_secondary,
            selected ? theme_.row_highlight : theme_.surface_background,
            TruncateLabel(label, std::max(20.0f, right_edge - row_rect.x - 6.0f)));
      }

      draw_vertical_scrollbar(
          MakeRect(layout.sidebar.x, list_y, layout.sidebar.w,
                   std::max(0.0f, layout.sidebar.y + layout.sidebar.h - list_y)),
          static_cast<float>(lines.size()), visible_units,
          static_cast<float>(scroll_row), surface_.drag_target == DragTarget::SidebarScrollbar);
    } else {
      const SDL_FRect refresh_rect = TreeSidebarRefreshButtonRect(layout.sidebar);
      const bool refresh_hovered =
          last_mouse_position_valid_ && Contains(refresh_rect, last_mouse_x_, last_mouse_y_);
      DrawFilledRect(renderer, refresh_rect,
                     refresh_hovered ? theme_.row_highlight : theme_.surface_raised);
      DrawRect(renderer, refresh_rect, refresh_hovered ? theme_.accent : theme_.border);
      draw_centered_text_on(refresh_rect, refresh_hovered ? theme_.text_primary : theme_.accent,
                            refresh_hovered ? theme_.row_highlight : theme_.surface_raised,
                            "Refresh");

      const std::string tree_root_label = ProjectLabel();
      const SDL_FRect sidebar_mode_rect = SidebarModeControlRect(layout.sidebar);
      const float root_label_left = sidebar_mode_rect.x + sidebar_mode_rect.w + 10.0f;
      const float root_label_right = refresh_rect.x - 10.0f;
      const float root_label_max_width = std::max(0.0f, root_label_right - root_label_left);
      const std::string root_label = TruncateLabel(tree_root_label, root_label_max_width);
      if (!root_label.empty()) {
        draw_centered_text_on(
            MakeRect(root_label_left, layout.sidebar.y + 4.0f, root_label_max_width, 18.0f),
            theme_.text_muted, theme_.chrome_background, root_label);
      }

      const auto& entries = directory_tree_.entries();
      const float list_y = layout.sidebar.y + kSidebarHeaderHeight + 6.0f;
      const int visible_rows =
          std::max(1, static_cast<int>((layout.sidebar.h - 36.0f) / kSidebarRowHeight));
      const int max_scroll = std::max(0, static_cast<int>(entries.size()) - visible_rows);
      const float row_width =
          std::max(0.0f, layout.sidebar.w - kSidebarInset * 2.0f -
                             (max_scroll > 0 ? kScrollbarThickness + 6.0f : 0.0f));
      int scroll_row = std::clamp(surface_.sidebar_scroll_row, 0, max_scroll);
      if (directory_tree_.selected_index() < static_cast<std::size_t>(scroll_row)) {
        scroll_row = static_cast<int>(directory_tree_.selected_index());
      } else if (directory_tree_.selected_index() >=
                 static_cast<std::size_t>(scroll_row + visible_rows)) {
        scroll_row = static_cast<int>(directory_tree_.selected_index()) - visible_rows + 1;
      }
      surface_.sidebar_scroll_row = scroll_row;

      for (int row = 0; row < visible_rows; ++row) {
        const int entry_index = scroll_row + row;
        if (entry_index >= static_cast<int>(entries.size())) {
          break;
        }

        const auto& entry = entries[entry_index];
        SDL_FRect row_rect = MakeRect(
            layout.sidebar.x + kSidebarInset,
            list_y + static_cast<float>(row) * kSidebarRowHeight,
            row_width,
            kSidebarRowHeight - 2.0f);

        const bool selected = static_cast<std::size_t>(entry_index) == directory_tree_.selected_index();
        if (selected) {
          DrawFilledRect(renderer, row_rect, theme_.row_highlight);
          DrawFilledRect(renderer, MakeRect(row_rect.x, row_rect.y, 2.0f, row_rect.h),
                         theme_.accent);
        }

        const float depth_offset = static_cast<float>(entry.depth) * kTreeIndentWidth;
        const float tree_x = row_rect.x + 6.0f + depth_offset;
        const float chevron_x = tree_x;
        const float label_x = tree_x + kTreeChevronSlotWidth + 4.0f;
        const float chevron_center_y = row_rect.y + row_rect.h * 0.5f;
        const char git_marker = GitMarker(entry.git_status);
        const bool has_git_marker = git_marker != ' ';
        const std::string git_marker_text = has_git_marker ? std::string(1, git_marker) : "";
        const float marker_width =
            has_git_marker ? text_renderer_.MeasureWidth(git_marker_text) : 0.0f;
        const float marker_x = row_rect.x + row_rect.w - marker_width - 8.0f;
        const float label_width =
            has_git_marker ? std::max(20.0f, marker_x - label_x - 8.0f)
                           : std::max(20.0f, row_rect.x + row_rect.w - label_x - 8.0f);

        if (entry.is_directory) {
          DrawChevron(renderer, chevron_x, chevron_center_y, entry.expanded,
                      selected ? theme_.text_primary : theme_.text_muted);
        }

        draw_vcentered_text_on(
            MakeRect(label_x, row_rect.y, label_width, row_rect.h), 0.0f,
            selected ? theme_.text_primary
                     : (entry.is_directory ? theme_.text_primary : theme_.text_secondary),
            selected ? theme_.row_highlight : theme_.surface_background,
            TruncateLabel(entry.label, label_width));
        if (has_git_marker) {
          draw_vcentered_text_on(
              MakeRect(marker_x, row_rect.y, marker_width, row_rect.h), 0.0f,
              selected ? theme_.text_primary : GitMarkerColor(theme_, entry.git_status),
              selected ? theme_.row_highlight : theme_.surface_background, git_marker_text);
        }
      }

      draw_vertical_scrollbar(
          MakeRect(layout.sidebar.x, list_y, layout.sidebar.w,
                   std::max(0.0f, layout.sidebar.y + layout.sidebar.h - list_y)),
          static_cast<float>(entries.size()), static_cast<float>(visible_rows),
          static_cast<float>(scroll_row), surface_.drag_target == DragTarget::SidebarScrollbar);
    }
  }

  if (surface_.overlay_visible) {
    DrawFilledRect(renderer, layout.editor_area, theme_.overlay_backdrop);
    const SDL_FRect overlay = ComputeOverlayRect(layout.editor_area);
    const SDL_FRect overlay_header = MakeRect(overlay.x, overlay.y, overlay.w, 30.0f);
    DrawFilledRect(renderer, overlay, theme_.overlay_background);
    DrawRect(renderer, overlay, theme_.border);
    DrawFilledRect(renderer, overlay_header, theme_.chrome_background);
    DrawFilledRect(renderer,
                   MakeRect(overlay_header.x, overlay_header.y + overlay_header.h - kDivider,
                            overlay_header.w, kDivider),
                   theme_.border);

    const float overlay_inset = 18.0f;
    const float overlay_row_height = 22.0f;
    ClampOverlayScrollRow(overlay);
    const float overlay_list_y = overlay.y + OverlayListStartOffset();
    const int overlay_visible_rows = OverlayVisibleRows(overlay);
    const int overlay_max_scroll =
        std::max(0, static_cast<int>(OverlayItemCount()) - overlay_visible_rows);
    const float overlay_row_width =
        overlay.w - overlay_inset * 2.0f -
        (overlay_max_scroll > 0 ? kScrollbarThickness + 6.0f : 0.0f);
      const auto draw_overlay_row =
          [&](int row_index, int selected_index, std::string_view label) {
            const bool selected = row_index == selected_index;
            SDL_FRect row = MakeRect(overlay.x + overlay_inset,
                                     overlay_list_y +
                                         static_cast<float>(row_index) * overlay_row_height,
                                     overlay_row_width, 18.0f);
            DrawFilledRect(renderer, row, selected ? theme_.row_highlight : theme_.surface_raised);
            draw_vcentered_text_on(row, 6.0f,
                                   selected ? theme_.text_primary : theme_.text_secondary,
                                   selected ? theme_.row_highlight : theme_.surface_raised,
                                   TruncateLabel(label, row.w - 12.0f));
          };

    if (surface_.overlay_mode == OverlayMode::BufferSearch) {
      draw_text_on(overlay.x + overlay_inset, overlay.y + 8.0f, theme_.text_primary,
                   theme_.chrome_background, "Search Buffer");
      draw_text_on(overlay.x + overlay_inset, overlay.y + 44.0f, theme_.text_secondary,
                   theme_.overlay_background, "> " + overlay_workflow_.buffer_search.query);
      const std::string summary =
          overlay_workflow_.buffer_search.matches.empty()
              ? "No matches"
              : std::to_string(overlay_workflow_.buffer_search.selected_index + 1) + " / " +
                    std::to_string(overlay_workflow_.buffer_search.matches.size()) + " matches";
      draw_text_on(overlay.x + overlay_inset, overlay.y + 62.0f, theme_.text_muted,
                   theme_.overlay_background, summary);
      for (int row = 0; row < overlay_visible_rows; ++row) {
        const int item_index = surface_.overlay_scroll_row + row;
        if (item_index >= static_cast<int>(overlay_workflow_.buffer_search.matches.size())) {
          break;
        }
        const auto& match = overlay_workflow_.buffer_search.matches[static_cast<std::size_t>(item_index)];
        const std::string label =
            "Ln " + std::to_string(match.start.line + 1) + ", Col " +
            std::to_string(match.start.column + 1) + "  " +
            TruncateLabel(text_viewport_.lines()[match.start.line], overlay.w - 150.0f);
        draw_overlay_row(row, static_cast<int>(overlay_workflow_.buffer_search.selected_index) - surface_.overlay_scroll_row,
                         label);
      }
    } else if (surface_.overlay_mode == OverlayMode::BufferReplace) {
      draw_text_on(overlay.x + overlay_inset, overlay.y + 8.0f, theme_.text_primary,
                   theme_.chrome_background, "Replace Buffer");
      draw_text_on(overlay.x + overlay_inset, overlay.y + 44.0f,
                   surface_.buffer_search_field == BufferSearchField::Search ? theme_.text_primary
                                                                     : theme_.text_secondary,
                   theme_.overlay_background, "find: " + overlay_workflow_.buffer_search.query);
      draw_text_on(overlay.x + overlay_inset, overlay.y + 62.0f,
                   surface_.buffer_search_field == BufferSearchField::Replace ? theme_.text_primary
                                                                      : theme_.text_secondary,
                   theme_.overlay_background, "replace: " + overlay_workflow_.buffer_search.replace_text);
      const std::string summary =
          overlay_workflow_.buffer_search.matches.empty()
              ? "No matches"
              : std::to_string(overlay_workflow_.buffer_search.selected_index + 1) + " / " +
                    std::to_string(overlay_workflow_.buffer_search.matches.size()) +
                    " matches  |  Enter replace  Ctrl+Enter replace all";
      draw_text_on(overlay.x + overlay_inset, overlay.y + 82.0f, theme_.text_muted,
                   theme_.overlay_background, TruncateLabel(summary, overlay.w - 36.0f));
      for (int row = 0; row < overlay_visible_rows; ++row) {
        const int item_index = surface_.overlay_scroll_row + row;
        if (item_index >= static_cast<int>(overlay_workflow_.buffer_search.matches.size())) {
          break;
        }
        const auto& match = overlay_workflow_.buffer_search.matches[static_cast<std::size_t>(item_index)];
        const std::string label =
            "Ln " + std::to_string(match.start.line + 1) + ", Col " +
            std::to_string(match.start.column + 1) + "  " +
            TruncateLabel(text_viewport_.lines()[match.start.line], overlay.w - 150.0f);
        draw_overlay_row(row, static_cast<int>(overlay_workflow_.buffer_search.selected_index) - surface_.overlay_scroll_row,
                         label);
      }
    } else if (surface_.overlay_mode == OverlayMode::ProjectSearch) {
      draw_text_on(overlay.x + overlay_inset, overlay.y + 8.0f, theme_.text_primary,
                   theme_.chrome_background, "Project Search");
      draw_text_on(overlay.x + overlay_inset, overlay.y + 44.0f, theme_.text_secondary,
                   theme_.overlay_background, "> " + overlay_workflow_.project_search.query);
      const std::string summary =
          overlay_workflow_.project_search.results.empty()
              ? "No results"
          : overlay_workflow_.project_search.truncated
              ? std::to_string(overlay_workflow_.project_search.selected_index + 1) + " / " +
                    std::to_string(overlay_workflow_.project_search.results.size()) + " shown (capped)"
              : std::to_string(overlay_workflow_.project_search.selected_index + 1) + " / " +
                    std::to_string(overlay_workflow_.project_search.results.size()) + " results";
      draw_text_on(overlay.x + overlay_inset, overlay.y + 62.0f, theme_.text_muted,
                   theme_.overlay_background, summary);
      for (int row = 0; row < overlay_visible_rows; ++row) {
        const int item_index = surface_.overlay_scroll_row + row;
        if (item_index >= static_cast<int>(overlay_workflow_.project_search.results.size())) {
          break;
        }
        const auto& result = overlay_workflow_.project_search.results[static_cast<std::size_t>(item_index)];
        const std::string label =
            result.relative_path.string() + ":" + std::to_string(result.line + 1) + ":" +
            std::to_string(result.column + 1) + "  " +
            TruncateLabel(result.preview, overlay.w - 220.0f);
        draw_overlay_row(row, static_cast<int>(overlay_workflow_.project_search.selected_index) - surface_.overlay_scroll_row,
                         label);
      }
    } else if (surface_.overlay_mode == OverlayMode::CommitPicker) {
      draw_text_on(overlay.x + overlay_inset, overlay.y + 8.0f, theme_.text_primary,
                   theme_.chrome_background, "Compare against commit");
      draw_text_on(overlay.x + overlay_inset, overlay.y + 44.0f, theme_.text_muted,
                   theme_.overlay_background, overlay_workflow_.compare_picker.path.filename().string());
      draw_text_on(overlay.x + overlay_inset, overlay.y + 62.0f, theme_.text_secondary,
                   theme_.overlay_background, "> " + overlay_workflow_.compare_picker.query);
      for (int row = 0; row < overlay_visible_rows; ++row) {
        const int item_index = surface_.overlay_scroll_row + row;
        if (item_index >= static_cast<int>(overlay_workflow_.compare_picker.matches.size())) {
          break;
        }
        const auto& commit = overlay_workflow_.compare_picker.matches[static_cast<std::size_t>(item_index)];
        draw_overlay_row(row, static_cast<int>(overlay_workflow_.compare_picker.selected_index) - surface_.overlay_scroll_row,
                         commit.short_hash + "  " + commit.subject);
      }
      if (overlay_workflow_.compare_picker.matches.empty()) {
        draw_text_on(overlay.x + overlay_inset, overlay.y + 92.0f, theme_.text_muted,
                     theme_.overlay_background, "No matching commits");
      }
    } else {
      draw_text_on(overlay.x + overlay_inset, overlay.y + 8.0f, theme_.text_primary,
                   theme_.chrome_background, "Find File");
      draw_text_on(overlay.x + overlay_inset, overlay.y + 44.0f, theme_.text_secondary,
                   theme_.overlay_background, "> " + file_finder_.query());

      const auto& results = file_finder_.results();
      for (int row = 0; row < overlay_visible_rows; ++row) {
        const int item_index = surface_.overlay_scroll_row + row;
        if (item_index >= static_cast<int>(results.size())) {
          break;
        }
        draw_overlay_row(row, static_cast<int>(file_finder_.selected_index()) - surface_.overlay_scroll_row,
                         results[static_cast<std::size_t>(item_index)].relative_path.string());
      }
      if (results.empty()) {
        draw_text_on(overlay.x + overlay_inset, overlay.y + 80.0f, theme_.text_muted,
                     theme_.overlay_background, "No matching files");
      }
    }

    draw_vertical_scrollbar(
        MakeRect(overlay.x, overlay_list_y, overlay.w,
                 std::max(0.0f, overlay.y + overlay.h - overlay_list_y - 8.0f)),
        static_cast<float>(OverlayItemCount()), static_cast<float>(overlay_visible_rows),
        static_cast<float>(surface_.overlay_scroll_row), surface_.drag_target == DragTarget::OverlayScrollbar);
  }

  if (BottomPanelVisible()) {
    const SDL_FRect panel_header =
        MakeRect(layout.bottom_panel.x, layout.bottom_panel.y, layout.bottom_panel.w,
                 kBottomPanelHeaderHeight);
    const bool terminal_panel = ActiveTerminalTab() != nullptr;
    if (terminal_panel) {
      for (const VisibleTerminalTab& tab : ComputeVisibleTerminalTabs(panel_header)) {
        const auto* terminal_tab =
            tab.index < terminal_tabs_.size() ? terminal_tabs_[tab.index].get() : nullptr;
        if (terminal_tab == nullptr) {
          continue;
        }

        const SDL_Color background = tab.active ? theme_.chrome_active : theme_.surface_raised;
        const SDL_Color foreground = tab.active ? theme_.text_primary : theme_.text_secondary;
        DrawFilledRect(renderer, tab.rect, background);
        if (tab.active) {
          DrawFilledRect(renderer, MakeRect(tab.rect.x, tab.rect.y, tab.rect.w, 2.0f),
                         theme_.accent);
        }
        draw_vcentered_text_on(tab.rect, 8.0f, foreground, background,
                               TruncateLabel(tab.display_title, tab.rect.w - 40.0f));
        draw_tab_close_button(tab.close_rect, foreground, theme_.text_primary);
      }
      const SDL_FRect new_tab_rect = BottomPanelTerminalNewTabRect(panel_header);
      DrawFilledRect(renderer, new_tab_rect, theme_.surface_raised);
      DrawRect(renderer, new_tab_rect, theme_.border);
      draw_centered_text_on(new_tab_rect, theme_.text_secondary, theme_.surface_raised, "+");
    } else {
      draw_vcentered_text_on(panel_header, 12.0f, theme_.text_secondary, theme_.chrome_background,
                             "Command");
    }

    const SDL_FRect panel_content = BottomPanelContentRect(layout, surface_.command_mode);
    const float logs_y = panel_content.y + 8.0f;
    const std::size_t panel_line_count = terminal_panel ? terminal_lines.size() : 0;
    const int visible_rows = BottomPanelVisibleRows(layout.bottom_panel.h);
    const int max_scroll = std::max(0, static_cast<int>(panel_line_count) - visible_rows);
    const int scroll_row = BottomPanelScrollRow(panel_line_count, visible_rows);
    SetBottomPanelScrollRow(scroll_row, panel_line_count, visible_rows);
    const float log_width =
        panel_content.w - 24.0f - (max_scroll > 0 ? kScrollbarThickness + 6.0f : 0.0f);
    for (int row = 0; row < visible_rows; ++row) {
      const int index = scroll_row + row;
      if (index >= static_cast<int>(panel_line_count)) {
        break;
      }
      const float line_y = logs_y + static_cast<float>(row) * text_renderer_.LineHeight();
      if (terminal_panel) {
        draw_terminal_line(panel_content.x + 12.0f, line_y, log_width,
                           terminal_lines[static_cast<std::size_t>(index)],
                           static_cast<std::size_t>(index));
      }
    }

    if (terminal_panel) {
      if (auto* active_terminal = ActiveTerminalTab(); active_terminal != nullptr &&
                                                   active_terminal->session.cursor_visible()) {
        const std::size_t cursor_row = active_terminal->session.cursor_row();
        const std::size_t cursor_column = active_terminal->session.cursor_column();
        if (cursor_row >= static_cast<std::size_t>(scroll_row) &&
            cursor_row < static_cast<std::size_t>(scroll_row + visible_rows) &&
            (surface_.focus != FocusTarget::Panel || CaretVisibleNow())) {
          const float char_width = std::max(1.0f, text_renderer_.CharWidth());
          const float cursor_x =
              panel_content.x + 12.0f + static_cast<float>(cursor_column) * char_width;
          const float cursor_y =
              logs_y + static_cast<float>(cursor_row - static_cast<std::size_t>(scroll_row)) *
                           text_renderer_.LineHeight();
          if (cursor_x < panel_content.x + panel_content.w - 2.0f) {
            DrawFilledRect(renderer,
                           MakeRect(cursor_x, cursor_y - 1.0f, 1.5f, text_renderer_.LineHeight()),
                           theme_.cursor);
          }
        }
      }
    }

    if (surface_.command_mode) {
      const SDL_FRect command_area = BottomPanelCommandAreaRect(layout);
      DrawFilledRect(renderer, command_area, theme_.surface_raised);
      DrawFilledRect(renderer, MakeRect(command_area.x, command_area.y, command_area.w, kDivider),
                     theme_.border);

      const float status_y = command_area.y + kBottomPanelCommandTopPadding;
      draw_text_on(command_area.x + 12.0f, status_y, theme_.text_muted, theme_.surface_raised,
                   TruncateLabel(CommandPromptStatusText(), command_area.w - 24.0f));

      SDL_FRect prompt_rect = BottomPanelCommandPromptRect(layout);
      DrawFilledRect(renderer, prompt_rect, theme_.chrome_active);
      draw_vcentered_text_on(prompt_rect, 6.0f, theme_.text_primary, theme_.chrome_active,
                             "> " + command_.input);
    }

    draw_vertical_scrollbar(
        panel_content,
        static_cast<float>(panel_line_count), static_cast<float>(visible_rows),
        static_cast<float>(scroll_row), surface_.drag_target == DragTarget::BottomPanelScrollbar);
  }

  if (surface_.menu_bar_open) {
    const auto draw_popup_menu =
        [&](MenuId menu_id, int active_item_index, const std::optional<SDL_FRect>& anchor_rect) {
          const MenuSpec* menu = FindMenuSpec(menu_id);
          if (menu == nullptr) {
            return;
          }
          const auto popup_rect =
              anchor_rect.has_value() ? ActiveSubmenuRect(layout.menu_bar)
                                      : ComputePopupMenuRect(layout.menu_bar, menu_id);
          if (!popup_rect.has_value()) {
            return;
          }

          DrawFilledRect(renderer, *popup_rect, theme_.overlay_background);
          DrawRect(renderer, *popup_rect, theme_.border);
          for (const VisiblePopupMenuItem& item :
               ComputeVisiblePopupMenuItems(menu->items, active_item_index, *popup_rect)) {
            if (item.separator) {
              DrawFilledRect(renderer,
                             MakeRect(item.rect.x + 8.0f, item.rect.y + item.rect.h * 0.5f,
                                      std::max(0.0f, item.rect.w - 16.0f), 1.0f),
                             theme_.border);
              continue;
            }

            const MenuItemSpec& spec = menu->items[item.index];
            const SDL_Color background =
                item.hovered && item.enabled ? theme_.row_highlight : theme_.overlay_background;
            const SDL_Color text_color = !item.enabled ? theme_.text_disabled
                                       : item.hovered ? theme_.text_primary
                                                      : theme_.text_secondary;
            const SDL_Color accel_color = !item.enabled ? theme_.text_disabled : theme_.text_muted;
            DrawFilledRect(renderer, item.rect, background);
            if (item.checked) {
              draw_centered_text_on(
                  MakeRect(item.rect.x + 8.0f, item.rect.y, 10.0f, item.rect.h),
                  item.enabled ? theme_.accent : theme_.text_disabled, background, "x");
            }
            const std::string accelerator = MenuItemAccelerator(spec);
            const float accelerator_width = text_renderer_.MeasureWidth(accelerator);
            const float label_width = std::max(0.0f, item.rect.w - 42.0f - accelerator_width);
            draw_vcentered_text_on(
                MakeRect(item.rect.x + 24.0f, item.rect.y, label_width, item.rect.h), 0.0f,
                text_color, background, TruncateLabel(MenuItemLabel(spec), label_width));
            if (!accelerator.empty()) {
              draw_vcentered_text_on(
                  MakeRect(item.rect.x + item.rect.w - accelerator_width - 10.0f, item.rect.y,
                           accelerator_width, item.rect.h),
                  0.0f, accel_color, background, accelerator);
            }
          }
        };
    draw_popup_menu(surface_.active_menu_id, surface_.active_menu_item_index, std::nullopt);
    if (surface_.active_submenu_id != MenuId::None) {
      draw_popup_menu(surface_.active_submenu_id, surface_.active_submenu_item_index, surface_.active_submenu_anchor_rect);
    }
  }

  if (surface_.tree_context_menu.open) {
    const auto items = TreeContextMenuItems(surface_.tree_context_menu.target);
    const auto popup_rect = ComputeTreeContextMenuRect();
    if (!items.empty() && popup_rect.has_value()) {
      DrawFilledRect(renderer, *popup_rect, theme_.overlay_background);
      DrawRect(renderer, *popup_rect, theme_.border);
      for (const VisiblePopupMenuItem& item :
           ComputeVisiblePopupMenuItems(items, surface_.tree_context_menu.active_item_index, *popup_rect)) {
        if (item.separator) {
          DrawFilledRect(renderer,
                         MakeRect(item.rect.x + 8.0f, item.rect.y + item.rect.h * 0.5f,
                                  std::max(0.0f, item.rect.w - 16.0f), 1.0f),
                         theme_.border);
          continue;
        }

        const MenuItemSpec& spec = items[item.index];
        const SDL_Color background =
            item.hovered && item.enabled ? theme_.row_highlight : theme_.overlay_background;
        const SDL_Color text_color = !item.enabled ? theme_.text_disabled
                                   : item.hovered ? theme_.text_primary
                                                  : theme_.text_secondary;
        const SDL_Color accel_color = !item.enabled ? theme_.text_disabled : theme_.text_muted;
        DrawFilledRect(renderer, item.rect, background);
        if (item.checked) {
          draw_centered_text_on(MakeRect(item.rect.x + 8.0f, item.rect.y, 10.0f, item.rect.h),
                                item.enabled ? theme_.accent : theme_.text_disabled, background,
                                "x");
        }
        const std::string accelerator = MenuItemAccelerator(spec);
        const float accelerator_width = text_renderer_.MeasureWidth(accelerator);
        const float label_width =
            std::max(0.0f, item.rect.w - 42.0f - accelerator_width);
        draw_vcentered_text_on(MakeRect(item.rect.x + 24.0f, item.rect.y, label_width, item.rect.h),
                               0.0f, text_color, background,
                               TruncateLabel(MenuItemLabel(spec), label_width));
        if (!accelerator.empty()) {
          draw_vcentered_text_on(
              MakeRect(item.rect.x + item.rect.w - accelerator_width - 10.0f, item.rect.y,
                       accelerator_width, item.rect.h),
              0.0f, accel_color, background, accelerator);
        }
      }
    }
  }

  if (prompts_.surface_visible) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    DrawFilledRect(renderer, layout.full, SDL_Color{0x05, 0x07, 0x0b, 0xcc});

    const SDL_FRect dialog = ComputePromptSurfaceRect(layout.full);
    const SDL_FRect header = MakeRect(dialog.x, dialog.y, dialog.w, 32.0f);
    const SDL_FRect message_rect = MakeRect(dialog.x + 16.0f, dialog.y + 50.0f, dialog.w - 32.0f, 36.0f);
    DrawFilledRect(renderer, dialog, theme_.overlay_background);
    DrawRect(renderer, dialog, theme_.border);
    DrawFilledRect(renderer, header, theme_.chrome_background);
    DrawFilledRect(renderer, MakeRect(header.x, header.y + header.h - 1.0f, header.w, 1.0f),
                   theme_.border);
    draw_vcentered_text_on(header, 16.0f, theme_.text_primary, theme_.chrome_background,
                           PromptSurfaceTitle());
    draw_text_on(message_rect.x, message_rect.y, theme_.text_muted, theme_.overlay_background,
                 TruncateLabel(PromptSurfaceMessage(), message_rect.w));

    if (prompts_.surface.kind == PromptSurfaceState::Kind::TextInput) {
      const SDL_FRect input_rect = ComputePromptSurfaceInputRect(dialog);
      DrawFilledRect(renderer, input_rect, theme_.surface_background);
      DrawRect(renderer, input_rect, theme_.border);
      draw_vcentered_text_on(input_rect, 6.0f, theme_.text_primary, theme_.surface_background,
                             TruncateLabel(prompts_.surface.input, input_rect.w - 12.0f));
    }

    const auto buttons = ComputePromptSurfaceButtonRects(dialog);
    const auto labels = PromptSurfaceActionLabels();
    for (std::size_t i = 0; i < buttons.size(); ++i) {
      const bool selected = prompts_.surface.selected_button == static_cast<int>(i);
      const SDL_Color background = selected ? theme_.chrome_active : theme_.surface_raised;
      DrawFilledRect(renderer, buttons[i], background);
      DrawRect(renderer, buttons[i], selected ? theme_.accent : theme_.border);
      draw_centered_text_on(buttons[i], theme_.text_primary, background, labels[i]);
    }
  }

  render_text_composition(active_text_input_visual);
  update_text_input_area(active_text_input_visual);

  if (prompts_.dirty_visible) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    DrawFilledRect(renderer, layout.full, SDL_Color{0x05, 0x07, 0x0b, 0xcc});

    const SDL_FRect dialog = ComputeDirtyPromptRect(layout.full);
    const SDL_FRect header = MakeRect(dialog.x, dialog.y, dialog.w, 32.0f);
    const SDL_FRect message_rect = MakeRect(dialog.x + 16.0f, dialog.y + 50.0f, dialog.w - 32.0f, 54.0f);
    DrawFilledRect(renderer, dialog, theme_.overlay_background);
    DrawRect(renderer, dialog, theme_.border);
    DrawFilledRect(renderer, header, theme_.chrome_background);
    DrawFilledRect(renderer,
                   MakeRect(header.x, header.y + header.h - 1.0f, header.w, 1.0f),
                   theme_.border);

    draw_vcentered_text_on(header, 12.0f, theme_.text_primary, theme_.chrome_background,
                           DirtyPromptTitle());
    draw_text_on(message_rect.x, message_rect.y, theme_.text_secondary, theme_.overlay_background,
                 TruncateLabel(DirtyPromptMessage(), message_rect.w));
    draw_text_on(message_rect.x, message_rect.y + 22.0f, theme_.text_muted, theme_.overlay_background,
                 "Enter confirm  Left/Right choose  Esc cancel");

    const auto buttons = ComputeDirtyPromptButtonRects(dialog);
    const auto labels = DirtyPromptActionLabels();
    for (std::size_t i = 0; i < buttons.size(); ++i) {
      const bool selected = prompts_.dirty.selected_action == static_cast<int>(i);
      DrawFilledRect(renderer, buttons[i],
                     selected ? theme_.chrome_active : theme_.surface_raised);
      DrawRect(renderer, buttons[i], selected ? theme_.accent : theme_.border);
      draw_centered_text_on(buttons[i],
                            selected ? theme_.text_primary : theme_.text_secondary,
                            selected ? theme_.chrome_active : theme_.surface_raised, labels[i]);
    }

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
  }
}


}  // namespace microide::workspace
