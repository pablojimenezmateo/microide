#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <cmath>
#include <optional>

#include "editor/TextLayout.h"
#include "util/PerformanceTrace.h"
#include "workspace/WorkspaceTextInputCoordinator.h"

namespace microide::workspace {

namespace {

constexpr float kSidebarHeaderHeight = 26.0f;

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

}  // namespace

void WorkspaceShell::ResizeTerminalToPanel(const SDL_FRect& panel_rect) {
  auto* terminal_tab = ActiveTerminalTab();
  if (terminal_tab == nullptr) {
    return;
  }

  util::PerformanceTrace::Scope trace_scope("WorkspaceShell::ResizeTerminalToPanel");
  const int rows = BottomPanelVisibleRows(panel_rect.h);
  const float usable_width =
      std::max(16.0f, panel_rect.w - 24.0f - kWorkspaceScrollbarThickness - 6.0f);
  const int columns = std::max(
      1, static_cast<int>(std::floor(usable_width / std::max(1.0f, text_renderer_.CharWidth()))));
  if (terminal_tab->session.rows() == static_cast<std::size_t>(rows) &&
      terminal_tab->session.columns() == static_cast<std::size_t>(columns)) {
    return;
  }
  terminal_tab->session.Resize(static_cast<std::size_t>(rows), static_cast<std::size_t>(columns));
  post_render_full_redraws_remaining_ = std::max(post_render_full_redraws_remaining_, 2);
}

void WorkspaceShell::DrawFilledRect(SDL_Renderer* renderer,
                                    const SDL_FRect& rect,
                                    SDL_Color color) const {
  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
  SDL_RenderFillRect(renderer, &rect);
}

void WorkspaceShell::DrawRect(SDL_Renderer* renderer,
                              const SDL_FRect& rect,
                              SDL_Color color) const {
  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
  SDL_RenderRect(renderer, &rect);
}

std::string WorkspaceShell::TruncateLabel(std::string_view text, float max_width) const {
  return text_renderer_.TruncateToWidth(text, max_width);
}

void WorkspaceShell::PrepareRenderFrame(SDL_Renderer* renderer, int width, int height) {
  if (renderer == nullptr || width <= 0 || height <= 0) {
    return;
  }

  util::PerformanceTrace::Scope trace_scope("WorkspaceShell::PrepareRenderFrame");
  ConsumePendingProjectOpenDialogResult();
  ConsumeProjectSearchUpdates();
  text_renderer_.EnsureInitialized(renderer, presentation_scale_x_, presentation_scale_y_);
  window_presentation_.logical_width = width;
  window_presentation_.logical_height = height;
  context_.current_project_state.sidebar.width = ClampSidebarWidth(context_.current_project_state.sidebar.width, static_cast<float>(width));
  context_.current_project_state.panel.height =
      ClampBottomPanelHeight(context_.current_project_state.panel.height, static_cast<float>(height));

  const WorkspaceLayout layout =
      ComputeLayout(static_cast<float>(width), static_cast<float>(height), context_.current_project_state.sidebar.visible,
                    BottomPanelVisible(), context_.current_project_state.sidebar.width, context_.current_project_state.panel.height);
  SDL_Window* render_window = SDL_GetRenderWindow(renderer);
  MakeTextInputCoordinator().SyncTextInputSurface(render_window);
  if (ActiveTabIsEditor()) {
    if (auto* editor_tab = ActiveEditorTab(); editor_tab != nullptr) {
      NormalizeEditorSplitTree(*editor_tab);
    }
  }
  if (BottomPanelShowsTerminal() && ActiveTerminalTab() != nullptr) {
    ResizeTerminalToPanel(layout.bottom_panel);
  }
  float mouse_x = last_mouse_x_;
  float mouse_y = last_mouse_y_;
  if (!last_mouse_position_valid_) {
    SDL_GetMouseState(&mouse_x, &mouse_y);
    SDL_RenderCoordinatesFromWindow(renderer, mouse_x, mouse_y, &mouse_x, &mouse_y);
  }
  UpdateMouseCursor(mouse_x, mouse_y);
}

void WorkspaceShell::RenderFrameBase(SDL_Renderer* renderer, const WorkspaceLayout& layout) const {
  DrawFilledRect(renderer, layout.full, theme_.window_background);
  DrawFilledRect(renderer, layout.menu_bar, theme_.chrome_background);
  DrawFilledRect(renderer,
                 MakeRect(layout.menu_bar.x,
                          layout.menu_bar.y + layout.menu_bar.h - kWorkspaceDividerThickness,
                          layout.menu_bar.w, kWorkspaceDividerThickness),
                 theme_.border);
  DrawFilledRect(renderer, layout.project_tab_strip, theme_.chrome_background);
  DrawFilledRect(renderer,
                 MakeRect(layout.project_tab_strip.x,
                          layout.project_tab_strip.y + layout.project_tab_strip.h -
                              kWorkspaceDividerThickness,
                          layout.project_tab_strip.w, kWorkspaceDividerThickness),
                 theme_.border);
  DrawFilledRect(renderer, layout.tab_strip, theme_.chrome_background);
  DrawFilledRect(renderer,
                 MakeRect(layout.tab_strip.x,
                          layout.tab_strip.y + layout.tab_strip.h - kWorkspaceDividerThickness,
                          layout.tab_strip.w, kWorkspaceDividerThickness),
                 theme_.border);
  DrawFilledRect(renderer, layout.breadcrumb, theme_.chrome_background);
  DrawFilledRect(renderer,
                 MakeRect(layout.breadcrumb.x,
                          layout.breadcrumb.y + layout.breadcrumb.h - kWorkspaceDividerThickness,
                          layout.breadcrumb.w, kWorkspaceDividerThickness),
                 theme_.border);

  if (context_.current_project_state.sidebar.visible) {
    DrawFilledRect(renderer, layout.sidebar, theme_.surface_background);
    DrawFilledRect(renderer,
                   MakeRect(layout.sidebar.x + layout.sidebar.w, layout.sidebar.y,
                            kWorkspaceDividerThickness, layout.sidebar.h),
                   context_.interaction_state.drag_target == DragTarget::SidebarDivider ? theme_.accent
                                                                      : theme_.border);
    const SDL_FRect sidebar_header =
        MakeRect(layout.sidebar.x, layout.sidebar.y, layout.sidebar.w, kSidebarHeaderHeight);
    DrawFilledRect(renderer, sidebar_header, theme_.chrome_background);
    DrawFilledRect(renderer,
                   MakeRect(sidebar_header.x,
                            sidebar_header.y + sidebar_header.h - kWorkspaceDividerThickness,
                            sidebar_header.w, kWorkspaceDividerThickness),
                   theme_.border);
  }

  if (BottomPanelVisible()) {
    DrawFilledRect(renderer, layout.bottom_panel, theme_.surface_background);
    DrawFilledRect(renderer,
                   MakeRect(layout.bottom_panel.x, layout.bottom_panel.y, layout.bottom_panel.w,
                            kWorkspaceDividerThickness),
                   context_.interaction_state.drag_target == DragTarget::BottomPanelDivider ? theme_.accent
                                                                          : theme_.border);
    const SDL_FRect panel_header = MakeRect(layout.bottom_panel.x, layout.bottom_panel.y,
                                            layout.bottom_panel.w,
                                            kWorkspaceBottomPanelHeaderHeight);
    DrawFilledRect(renderer, panel_header, theme_.chrome_background);
    DrawFilledRect(renderer,
                   MakeRect(panel_header.x,
                            panel_header.y + panel_header.h - kWorkspaceDividerThickness,
                            panel_header.w, kWorkspaceDividerThickness),
                   theme_.border);
  }
}

void WorkspaceShell::RenderActiveWorkspaceSurface(
    SDL_Renderer* renderer,
    const WorkspaceLayout& layout,
    bool draw_editor_caret,
    std::optional<SDL_FRect>* active_editor_pane_rect) {
  if (ActiveTabIsCompare()) {
    RenderCompareSurface(renderer, layout.editor_surface);
  } else if (ActiveTabIsMerge()) {
    RenderMergeSurface(renderer, layout.editor_surface);
  } else {
    const auto diagnostics_for_viewport =
        [this](const editor::TextViewport& viewport)
        -> std::span<const editor::PublishedDiagnostic> {
      if (viewport.path().empty() || viewport.dirty()) {
        return {};
      }
      const auto* diagnostics = context_.current_project_state.diagnostics_store.FindByPath(viewport.path());
      return diagnostics != nullptr ? std::span<const editor::PublishedDiagnostic>(*diagnostics)
                                    : std::span<const editor::PublishedDiagnostic>{};
    };
    const auto draw_review_comment_markers =
        [this, renderer](const editor::TextViewport& viewport, const SDL_FRect& pane_rect) {
          if (renderer == nullptr || viewport.path().empty() || viewport.is_placeholder()) {
            return;
          }

          const std::string uri = viewport.path().generic_string();
          std::vector<int> marked_lines;
          for (const ReviewThread& thread : review_comments_registry_.GetThreads(uri)) {
            if (thread.line > 0 &&
                std::find(marked_lines.begin(), marked_lines.end(), thread.line) == marked_lines.end()) {
              marked_lines.push_back(thread.line);
            }
          }
          if (marked_lines.empty()) {
            for (std::size_t line_index = viewport.scroll_line();
                 line_index < std::min(viewport.lines().size(),
                                       viewport.scroll_line() + viewport.visible_lines());
                 ++line_index) {
              if (!review_comments_registry_
                       .GetComments(uri, static_cast<int>(line_index + 1))
                       .empty()) {
                marked_lines.push_back(static_cast<int>(line_index + 1));
              }
            }
          }
          if (marked_lines.empty()) {
            return;
          }

          const editor::EditorViewMetrics metrics =
              editor::EditorViewRenderer::ComputeMetrics(text_renderer_, viewport, pane_rect);
          for (std::size_t line_index = viewport.scroll_line();
               line_index < std::min(viewport.lines().size(),
                                     viewport.scroll_line() + viewport.visible_lines());
               ++line_index) {
            const int one_based_line = static_cast<int>(line_index + 1);
            if (std::find(marked_lines.begin(), marked_lines.end(), one_based_line) ==
                marked_lines.end()) {
              continue;
            }
            const float y =
                metrics.first_line_y +
                static_cast<float>(line_index - viewport.scroll_line()) * metrics.line_height;
            const SDL_FRect marker_rect = SDL_FRect{
                pane_rect.x + metrics.gutter_width - 6.0f,
                y + std::max(1.0f, (metrics.line_height - 6.0f) * 0.5f),
                3.0f,
                6.0f,
            };
            DrawFilledRect(renderer, marker_rect, theme_.accent);
          }
        };
    const auto panes = ComputeEditorPaneLayouts(layout.editor_surface);
    editor::TextViewport* active_viewport = ActiveEditorViewport();
    if (panes.empty() && active_viewport != nullptr && active_viewport->is_placeholder()) {
      if (active_editor_pane_rect != nullptr) {
        *active_editor_pane_rect = layout.editor_surface;
      }
      editor_view_renderer_.Render(renderer, text_renderer_, theme_, *active_viewport,
                                   layout.editor_surface, draw_editor_caret, "", std::nullopt,
                                   std::nullopt, {});
    }
    auto* editor_tab = ActiveEditorTab();
    for (const EditorPaneLayout& pane : panes) {
      editor::TextViewport* viewport =
          pane.active ? ActiveEditorViewport()
                      : (editor_tab != nullptr ? FindEditorView(*editor_tab, pane.leaf_id)
                                               : nullptr);
      if (viewport == nullptr) {
        continue;
      }
      if (pane.active && active_editor_pane_rect != nullptr) {
        *active_editor_pane_rect = pane.rect;
      }
      const auto blame_overlay =
          pane.active ? BuildEditorBlameOverlay(*viewport, pane.rect) : std::nullopt;
      if (pane.active) {
        visible_editor_blame_overlay_ = blame_overlay;
      }
      editor_view_renderer_.Render(renderer, text_renderer_, theme_, *viewport, pane.rect,
                                   pane.active && draw_editor_caret,
                                   pane.active &&
                                           (context_.current_project_state.overlay.mode == OverlayMode::BufferSearch ||
                                            context_.current_project_state.overlay.mode == OverlayMode::BufferReplace)
                                       ? context_.current_project_state.overlay.workflow.buffer_search.query.text
                                       : "",
                                   pane.active ? ActiveBufferSearchMatch() : std::nullopt,
                                   blame_overlay, diagnostics_for_viewport(*viewport));
      draw_review_comment_markers(*viewport, pane.rect);

      if (pane.active && context_.current_project_state.inline_completion.visible &&
          !context_.current_project_state.inline_completion.text.empty()) {
        const std::size_t line_index = context_.current_project_state.inline_completion.start_line;
        const auto& lines = viewport->lines();
        if (line_index < lines.size() && line_index >= viewport->scroll_line() &&
            line_index < viewport->scroll_line() + viewport->visible_lines()) {
          const editor::EditorViewMetrics metrics =
              editor::EditorViewRenderer::ComputeMetrics(text_renderer_, *viewport, pane.rect);
          const std::size_t visual_column = editor::TextLayout::VisualColumnForTextColumn(
              lines[line_index], context_.current_project_state.inline_completion.start_column,
              viewport->tab_size());
          if (visual_column >= viewport->horizontal_scroll() &&
              visual_column < viewport->horizontal_scroll() + viewport->visible_columns()) {
            const float draw_x =
                metrics.text_x +
                static_cast<float>(visual_column - viewport->horizontal_scroll()) *
                    text_renderer_.CharWidth();
            const float draw_y =
                metrics.first_line_y +
                static_cast<float>(line_index - viewport->scroll_line()) * metrics.line_height;
            std::string ghost_text = context_.current_project_state.inline_completion.text;
            if (const std::size_t newline = ghost_text.find('\n'); newline != std::string::npos) {
              ghost_text.erase(newline);
            }
            text_renderer_.DrawStringOn(
                renderer, draw_x, draw_y, theme_.text_disabled,
                line_index == viewport->cursor_line() ? theme_.row_highlight
                                                      : theme_.editor_background,
                ghost_text);
          }
        }
      }
    }
  }

  if (ActiveTabIsCompare()) {
    RenderCompareScrollbars(renderer, layout.editor_surface);
  } else if (ActiveTabIsMerge()) {
    RenderMergeScrollbars(renderer, layout.editor_surface);
  } else {
    const auto panes = ComputeEditorPaneLayouts(layout.editor_surface);
    auto* editor_tab = ActiveEditorTab();
    for (const EditorPaneLayout& pane : panes) {
      editor::TextViewport* viewport =
          pane.active ? ActiveEditorViewport()
                      : (editor_tab != nullptr ? FindEditorView(*editor_tab, pane.leaf_id)
                                               : nullptr);
      if (viewport == nullptr || viewport->is_placeholder()) {
        continue;
      }

      const editor::EditorViewMetrics metrics =
          editor::EditorViewRenderer::ComputeMetrics(text_renderer_, *viewport, pane.rect);
      viewport->SetViewportSize(metrics.visible_rows, metrics.visible_columns);
      const auto scroll_layout = ComputeEditorScrollLayout(pane.rect, *viewport, metrics);
      if (scroll_layout.vertical_scrollbar.has_value()) {
        DrawScrollbar(renderer, theme_, scroll_layout.vertical_scrollbar->track,
                      scroll_layout.vertical_scrollbar->thumb,
                      pane.active &&
                         context_.interaction_state.drag_target == DragTarget::EditorVerticalScrollbar);
      }
      if (scroll_layout.horizontal_scrollbar.has_value()) {
        DrawScrollbar(renderer, theme_, scroll_layout.horizontal_scrollbar->track,
                      scroll_layout.horizontal_scrollbar->thumb,
                      pane.active &&
                         context_.interaction_state.drag_target == DragTarget::EditorHorizontalScrollbar);
      }
    }
    for (const EditorSplitDividerLayout& divider :
         ComputeEditorSplitDividerLayouts(layout.editor_surface)) {
      const bool divider_active =
          context_.interaction_state.drag_target == DragTarget::EditorSplitDivider &&
          divider.divider_index == context_.interaction_state.drag_editor_split_divider_index &&
          divider.node_path == context_.interaction_state.drag_editor_split_path;
      DrawFilledRect(renderer, divider.rect, divider_active ? theme_.accent : theme_.border);
    }
  }
}

}  // namespace microide::workspace
