#include "workspace/WorkspaceShellRenderPrimitives.h"

#include <algorithm>
#include <cmath>
#include <optional>

#include "editor/TextLayout.h"
#include "util/PerformanceTrace.h"
#include "workspace/RenderViewModelBuilder.h"
#include "workspace/WorkspaceTextInputCoordinator.h"

namespace microide::workspace {

using namespace detail;

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

WorkspaceShell::FrameToken WorkspaceShell::PrepareFrameOnce(SDL_Renderer* renderer,
                                                            int width,
                                                            int height) {
  if (renderer == nullptr || width <= 0 || height <= 0) {
    return FrameToken{};
  }

  util::PerformanceTrace::Scope trace_scope("WorkspaceShell::PrepareFrameOnce");
  ConsumePendingProjectOpenDialogResult();
  ConsumeProjectSearchUpdates();
  text_renderer_.EnsureInitialized(renderer, presentation_scale_x_, presentation_scale_y_);
  window_presentation_.logical_width = width;
  window_presentation_.logical_height = height;
  const SidebarSurfaceViewModel sidebar_vm = RenderViewModelBuilder(context_).BuildSidebarSurface();
  const BottomPanelSurfaceViewModel panel_vm =
      RenderViewModelBuilder(context_).BuildBottomPanelSurface();
  ProjectWorkspaceState& project_state = *sidebar_vm.project_state;
  ApplyLiveSettings();
  const float clamped_sidebar_width =
      ClampSidebarWidth(project_state.sidebar.width, static_cast<float>(width));
  const float clamped_panel_height =
      ClampBottomPanelHeight(project_state.panel.height, static_cast<float>(height));
  if (clamped_sidebar_width != project_state.sidebar.width ||
      clamped_panel_height != project_state.panel.height) {
    layout_dirty_ = true;
  }
  project_state.sidebar.width = clamped_sidebar_width;
  project_state.panel.height = clamped_panel_height;

  WorkspaceLayout layout;
  if (layout_dirty_ || !prepared_frame_layout_.has_value()) {
    layout = ComputeLayout(static_cast<float>(width), static_cast<float>(height), sidebar_vm.visible,
                           panel_vm.command_mode || panel_vm.content != PanelContentKind::None,
                           project_state.sidebar.width, project_state.panel.height,
                           layout_mode_service_.SnapshotInputs(),
                           layout_mode_service_.StatusBarVisible());
    layout_mode_service_.SetCurrentMode(layout.layout_mode);
    ++prepare_frame_layout_compute_count_;
    layout_dirty_ = false;
  } else {
    layout = *prepared_frame_layout_;
  }
  RefreshStatusBar();
  RefreshSettingsOverlayCatalog();
  SDL_Window* render_window = SDL_GetRenderWindow(renderer);
  MakeTextInputCoordinator().SyncTextInputSurface(render_window);
  if (ActiveTabIsEditor()) {
    if (auto* editor_tab = ActiveEditorTab(); editor_tab != nullptr) {
      NormalizeEditorSplitTree(*editor_tab);
    }
  }
  if (panel_vm.content == PanelContentKind::Terminal && ActiveTerminalTab() != nullptr) {
    ResizeTerminalToPanel(layout.bottom_panel);
  }
  prepared_frame_layout_ = layout;
  prepared_frame_draw_editor_caret_ =
      CaretVisibleNow() &&
      !(context_.text_input.active_surface == TextInputSurface::Editor &&
        !context_.text_input.composition.text.empty());
  FrameToken::VisibleLineRange visible_line_range{};
  if (auto* viewport = ActiveEditorViewport(); viewport != nullptr && !viewport->is_placeholder()) {
    visible_line_range.viewport = viewport;
    visible_line_range.start_line = viewport->VisualRowLineIndex(viewport->scroll_line());
    const std::size_t last_visible_row =
        viewport->visible_lines() == 0
            ? viewport->scroll_line()
            : viewport->scroll_line() + viewport->visible_lines() - 1;
    visible_line_range.end_line =
        std::min(viewport->lines().size(), viewport->VisualRowLineIndex(last_visible_row) + 1);
  }
  ++prepared_frame_id_;
  float mouse_x = last_mouse_x_;
  float mouse_y = last_mouse_y_;
  if (!last_mouse_position_valid_) {
    SDL_GetMouseState(&mouse_x, &mouse_y);
    SDL_RenderCoordinatesFromWindow(renderer, mouse_x, mouse_y, &mouse_x, &mouse_y);
  }
  {
    util::PerformanceTrace::Scope scope("WorkspaceShell::PrepareFrameOnce::UpdateMouseCursor");
    UpdateMouseCursor(mouse_x, mouse_y);
  }
  return FrameToken{prepared_frame_id_, visible_line_range};
}

void WorkspaceShell::PrepareRenderFrame(SDL_Renderer* renderer, int width, int height) {
  (void)PrepareFrameOnce(renderer, width, height);
}

void WorkspaceShell::RenderFrameBase(SDL_Renderer* renderer, const WorkspaceLayout& layout) const {
  const FrameSurfaceViewModel frame_vm = RenderViewModelBuilder(context_).BuildFrameSurface(layout);
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

  if (frame_vm.sidebar_visible) {
    DrawFilledRect(renderer, layout.sidebar, theme_.surface_background);
    DrawFilledRect(renderer,
                   MakeRect(layout.sidebar.x + layout.sidebar.w, layout.sidebar.y,
                            kWorkspaceDividerThickness, layout.sidebar.h),
                   context_.interaction_state.drag_target == DragTarget::SidebarDivider ? theme_.accent
                                                                      : theme_.border);
    const SDL_FRect sidebar_header = MakeRect(layout.sidebar.x, layout.sidebar.y, layout.sidebar.w,
                                              kWorkspaceHeaderHeight);
    DrawFilledRect(renderer, sidebar_header, theme_.chrome_background);
    DrawFilledRect(renderer,
                   MakeRect(sidebar_header.x,
                            sidebar_header.y + sidebar_header.h - kWorkspaceDividerThickness,
                            sidebar_header.w, kWorkspaceDividerThickness),
                   theme_.border);
  }

  if (frame_vm.bottom_panel_visible) {
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
    const FrameToken& frame_token,
    bool draw_editor_caret,
    std::optional<SDL_FRect>* active_editor_pane_rect) {
  (void)frame_token;
  const FrameSurfaceViewModel frame_vm = RenderViewModelBuilder(context_).BuildFrameSurface(layout);
  ProjectWorkspaceState& project_state = *frame_vm.project_state;
  const OverlaySurfaceViewModel overlay_vm = RenderViewModelBuilder(context_).BuildOverlaySurface();
  CompareTabState* active_compare_tab = nullptr;
  MergeTabState* active_merge_tab = nullptr;
  if (frame_vm.compare_surface.has_value() &&
      project_state.active_tab_index < project_state.open_tabs.size()) {
    TabEntry& active_tab = project_state.open_tabs[project_state.active_tab_index];
    if (frame_vm.compare_surface->kind == TabEntry::Kind::Compare && active_tab.compare.has_value()) {
      active_compare_tab = &active_tab.compare.value();
    } else if (frame_vm.compare_surface->kind == TabEntry::Kind::Merge &&
               active_tab.merge.has_value()) {
      active_merge_tab = &active_tab.merge.value();
    }
  }
  const bool render_editor_surface = active_compare_tab == nullptr && active_merge_tab == nullptr;
  const std::vector<EditorPaneLayout> editor_panes =
      render_editor_surface ? ComputeEditorPaneLayouts(layout.editor_surface)
                            : std::vector<EditorPaneLayout>{};
  if (active_compare_tab != nullptr) {
    const bool draw_compare_caret =
        project_state.surface.focus == FocusTarget::Editor && active_compare_tab->right_view_active &&
        CaretVisibleNow() &&
        !(overlay_vm.current_surface == TextInputSurface::Editor &&
          !context_.text_input.composition.text.empty());
    RenderCompareSurface(renderer, layout.editor_surface, *active_compare_tab, draw_compare_caret,
                         project_state.diagnostics_store);
  } else if (active_merge_tab != nullptr) {
    RenderMergeSurface(renderer, layout.editor_surface);
  } else {
    const auto diagnostics_for_viewport =
        [this, &project_state](const editor::TextViewport& viewport)
        -> std::span<const editor::PublishedDiagnostic> {
      if (viewport.path().empty() || viewport.dirty()) {
        return {};
      }
      const auto* diagnostics = project_state.diagnostics_store.FindByPath(viewport.path());
      return diagnostics != nullptr ? std::span<const editor::PublishedDiagnostic>(*diagnostics)
                                    : std::span<const editor::PublishedDiagnostic>{};
    };
    const auto draw_review_comment_markers =
        [this, renderer](const editor::TextViewport& viewport, const SDL_FRect& pane_rect) {
          if (renderer == nullptr || viewport.path().empty() || viewport.is_placeholder()) {
            return;
          }

          const std::string uri = viewport.path().generic_string();
          const editor::EditorViewMetrics metrics =
              editor::EditorViewRenderer::ComputeMetrics(text_renderer_, viewport, pane_rect);
          for (std::size_t row = 0; row < viewport.visible_lines(); ++row) {
            const std::size_t visual_row_index = viewport.scroll_line() + row;
            if (visual_row_index >= viewport.visual_line_count()) {
              break;
            }
            const auto row_meta = viewport.WrappedVisualRowLayout(visual_row_index);
            if (viewport.soft_wrap() && row_meta.visual_start != 0) {
              continue;
            }
            const std::size_t line_index = row_meta.line_index;
            const int one_based_line = static_cast<int>(line_index + 1);
            if (!review_comments_registry_.HasThreads(uri, one_based_line) &&
                !review_comments_registry_.HasComments(uri, one_based_line)) {
              continue;
            }
            const float y =
                metrics.first_line_y + static_cast<float>(row) * metrics.line_height;
            const SDL_FRect marker_rect = SDL_FRect{
                pane_rect.x + metrics.gutter_width - 6.0f,
                y + std::max(1.0f, (metrics.line_height - 6.0f) * 0.5f),
                3.0f,
                6.0f,
            };
            DrawFilledRect(renderer, marker_rect, theme_.accent);
          }
        };
    const auto setting_enabled = [this](std::string_view id, bool default_value) {
      const auto value = GetSettingValue(id);
      if (!value.has_value()) {
        return default_value;
      }
      return *value != "false" && *value != "0" && *value != "off";
    };
    const bool bracket_match_highlight_enabled =
        setting_enabled("editor.brackets.match_highlight.enabled", true);
    const bool indent_guides_enabled =
        setting_enabled("editor.view.indent_guides.enabled", true);
    const bool render_whitespace_enabled =
        setting_enabled("editor.view.render_whitespace", false);
    const bool fold_enabled = setting_enabled("editor.fold.enabled", true);
    const editor::FoldingModel* active_folding_model =
        fold_enabled ? EnsureActiveFoldingModelFresh() : nullptr;
    if (!fold_enabled) {
      if (auto* editor_tab = ActiveEditorTab(); editor_tab != nullptr) {
        for (auto& view : editor_tab->views) {
          view.viewport.SetFoldingModel(nullptr);
        }
      }
    }
    const std::vector<EditorPaneLayout>& panes = editor_panes;
    editor::TextViewport* active_viewport = ActiveEditorViewport();
    if (panes.empty() && active_viewport != nullptr && active_viewport->is_placeholder()) {
      if (active_editor_pane_rect != nullptr) {
        *active_editor_pane_rect = layout.editor_surface;
      }
      editor_view_renderer_.Render(renderer, text_renderer_, theme_, *active_viewport,
                                   layout.editor_surface, draw_editor_caret, "", std::nullopt,
                                   std::nullopt, {}, bracket_match_highlight_enabled,
                                   indent_guides_enabled, render_whitespace_enabled,
                                   active_folding_model);
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
                                           (overlay_vm.mode == OverlayMode::BufferSearch ||
                                            overlay_vm.mode == OverlayMode::BufferReplace)
                                       ? overlay_vm.buffer_search_query_text
                                       : "",
                                   pane.active ? ActiveBufferSearchMatch() : std::nullopt,
                                   blame_overlay, diagnostics_for_viewport(*viewport),
                                   pane.active && bracket_match_highlight_enabled,
                                   indent_guides_enabled, render_whitespace_enabled,
                                   active_folding_model);
      draw_review_comment_markers(*viewport, pane.rect);

    }
  }

  if (active_compare_tab != nullptr) {
    RenderCompareScrollbars(renderer, layout.editor_surface, *active_compare_tab);
  } else if (active_merge_tab != nullptr) {
    RenderMergeScrollbars(renderer, layout.editor_surface);
  } else {
    const std::vector<EditorPaneLayout>& panes = editor_panes;
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
