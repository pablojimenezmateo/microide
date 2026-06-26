#include "workspace/WorkspaceShellRenderPrimitives.h"

#include <algorithm>
#include <cmath>
#include <optional>

#include "editor/TextLayout.h"
#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"
#include "workspace/RenderViewModelBuilder.h"
#include "workspace/SettingFlags.h"
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

void WorkspaceShell::DrawSurfaceFocusRing(SDL_Renderer* renderer, const SDL_FRect& region) const {
  // A 1px accent outline just inside the region marks which surface owns the keyboard.
  // Each surface draws its own ring from its render path so partial redraws repaint it
  // consistently with full redraws; HandleEvent repaints both surfaces when focus moves.
  if (renderer == nullptr || region.w <= 2.0f || region.h <= 2.0f) {
    return;
  }
  OutlineRect(renderer,
              MakeRect(region.x + 1.0f, region.y + 1.0f, region.w - 2.0f, region.h - 2.0f),
              theme_.accent);
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

  clip_frame_overlay_view_models_valid_ = false;
  prepare_cached_sidebar_vm_.reset();
  prepare_cached_bottom_panel_vm_.reset();
  prepare_cached_debug_pane_vm_.reset();
  prepare_cached_text_input_vm_.reset();

  util::PerformanceTrace::Scope trace_scope("WorkspaceShell::PrepareFrameOnce");
  util::AddPerformanceCounter(util::PerfCounterId::FramePrepareCalls);
  ConsumePendingProjectOpenDialogResult();
  ConsumeProjectSearchUpdates();
  text_renderer_.EnsureInitialized(renderer, presentation_scale_x_, presentation_scale_y_);
  window_presentation_.logical_width = width;
  window_presentation_.logical_height = height;
  const RenderViewModelBuilder view_models(context_);
  prepare_cached_sidebar_vm_.emplace(view_models.BuildSidebarSurface());
  prepare_cached_bottom_panel_vm_.emplace(view_models.BuildBottomPanelSurface());
  // The debug pane can only ever be visible when the debugger is enabled, so skip
  // building its view model entirely in the common debug-off case. RenderDebugPane
  // tolerates the empty optional.
  if (DebugEnabled()) {
    prepare_cached_debug_pane_vm_.emplace(view_models.BuildDebugPaneSurface());
  }
  const SidebarSurfaceViewModel& sidebar_vm = *prepare_cached_sidebar_vm_;
  const BottomPanelSurfaceViewModel& panel_vm = *prepare_cached_bottom_panel_vm_;
  ProjectWorkspaceState& project_state = *sidebar_vm.project_state;
  ApplyLiveSettings();
  prepare_cached_text_input_vm_.emplace(view_models.BuildTextInputSurface());
  const float clamped_sidebar_width =
      ClampSidebarWidth(project_state.sidebar.width, static_cast<float>(width));
  const float clamped_panel_height =
      ClampBottomPanelHeight(project_state.panel.height, static_cast<float>(height));
  const float resolved_sidebar_width = sidebar_vm.visible ? clamped_sidebar_width : 0.0f;
  const float clamped_right_pane_width = ClampRightPaneWidth(
      project_state.debug_pane.width, static_cast<float>(width), resolved_sidebar_width);
  if (clamped_sidebar_width != project_state.sidebar.width ||
      clamped_panel_height != project_state.panel.height ||
      clamped_right_pane_width != project_state.debug_pane.width) {
    layout_dirty_ = true;
  }
  project_state.sidebar.width = clamped_sidebar_width;
  project_state.panel.height = clamped_panel_height;
  project_state.debug_pane.width = clamped_right_pane_width;

  WorkspaceLayout layout;
  bool workspace_layout_recomputed = false;
  if (layout_dirty_ || !prepared_frame_layout_.has_value()) {
    layout = ComputeLayout(static_cast<float>(width), static_cast<float>(height), sidebar_vm.visible,
                           panel_vm.command_mode || panel_vm.content != PanelContentKind::None,
                           project_state.sidebar.width, project_state.panel.height,
                           layout_mode_service_.SnapshotInputs(),
                           layout_mode_service_.StatusBarVisible(),
                           project_state.debug_pane.visible, project_state.debug_pane.width);
    layout_mode_service_.SetCurrentMode(layout.layout_mode);
    ++prepare_frame_layout_compute_count_;
    layout_dirty_ = false;
    workspace_layout_recomputed = true;
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
    const SDL_FRect& panel = layout.bottom_panel;
    const auto& cached = last_terminal_panel_rect_;
    const bool unchanged = cached.has_value() && cached->x == panel.x && cached->y == panel.y &&
                           cached->w == panel.w && cached->h == panel.h;
    if (!unchanged) {
      ResizeTerminalToPanel(panel);
      last_terminal_panel_rect_ = panel;
    }
  } else {
    last_terminal_panel_rect_.reset();
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
    UpdateMouseCursor(mouse_x, mouse_y, !MenuSurfaceCapturingMouse(), workspace_layout_recomputed);
  }
  return FrameToken{prepared_frame_id_, visible_line_range};
}

void WorkspaceShell::PrepareRenderFrame(SDL_Renderer* renderer, int width, int height) {
  (void)PrepareFrameOnce(renderer, width, height);
}

void WorkspaceShell::EnsureClipFrameAndOverlayViewModels(const WorkspaceLayout& layout) const {
  if (clip_frame_overlay_view_models_valid_ &&
      clip_frame_overlay_view_models_frame_id_ == prepared_frame_id_ &&
      clip_frame_overlay_view_models_layout_ == layout) {
    return;
  }
  clip_cached_frame_vm_.emplace(RenderViewModelBuilder(context_).BuildFrameSurface(layout));
  util::AddPerformanceCounter(util::PerfCounterId::RenderViewModelBuildFrameSurfaceCalls, 1);
  clip_cached_overlay_vm_.emplace(RenderViewModelBuilder(context_).BuildOverlaySurface());
  util::AddPerformanceCounter(util::PerfCounterId::RenderViewModelBuildOverlaySurfaceCalls, 1);
  clip_frame_overlay_view_models_layout_ = layout;
  clip_frame_overlay_view_models_frame_id_ = prepared_frame_id_;
  clip_frame_overlay_view_models_valid_ = true;
}

void WorkspaceShell::RootViewRenderFrameBase(SDL_Renderer* renderer,
                                             const WorkspaceLayout& layout) const {
  EnsureClipFrameAndOverlayViewModels(layout);
  RenderFrameBase(renderer, layout, *clip_cached_frame_vm_);
}

void WorkspaceShell::RootViewRenderActiveWorkspaceSurface(
    SDL_Renderer* renderer,
    const WorkspaceLayout& layout,
    const FrameToken& frame_token,
    bool draw_editor_caret,
    std::optional<SDL_FRect>* active_editor_pane_rect) {
  EnsureClipFrameAndOverlayViewModels(layout);
  RenderActiveWorkspaceSurface(renderer, layout, frame_token, draw_editor_caret, active_editor_pane_rect,
                               *clip_cached_frame_vm_, *clip_cached_overlay_vm_);
}

void WorkspaceShell::RootViewRenderOverlaySurface(SDL_Renderer* renderer,
                                                  const WorkspaceLayout& layout) {
  EnsureClipFrameAndOverlayViewModels(layout);
  RenderOverlaySurface(renderer, layout, *clip_cached_overlay_vm_);
}

void WorkspaceShell::RenderFrameBase(SDL_Renderer* renderer,
                                   const WorkspaceLayout& layout,
                                   const FrameSurfaceViewModel& frame_vm) const {
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
    std::optional<SDL_FRect>* active_editor_pane_rect,
    const FrameSurfaceViewModel& frame_vm,
    const OverlaySurfaceViewModel& overlay_vm) {
  (void)frame_token;
  ProjectWorkspaceState& project_state = *frame_vm.project_state;
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
    RenderCompareSurface(renderer, layout.editor_surface, *active_compare_tab, project_state.root,
                         draw_compare_caret, project_state.diagnostics_store);
  } else if (active_merge_tab != nullptr) {
    const bool draw_merge_caret =
        project_state.surface.focus == FocusTarget::Editor && CaretVisibleNow();
    RenderMergeSurface(renderer, layout.editor_surface, project_state.root, draw_merge_caret,
                       project_state.diagnostics_store);
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
    const auto decorations_for_viewport =
        [&project_state](const editor::TextViewport& viewport) -> const editor::FileDecorations* {
      if (viewport.path().empty() || viewport.dirty()) {
        return nullptr;
      }
      const auto* pres = project_state.plugin_presentation_if_present();
      return pres != nullptr ? pres->decorations.FindByPath(viewport.path()) : nullptr;
    };
    const auto draw_review_comment_markers =
        [this, renderer](const editor::TextViewport& viewport,
                         const SDL_FRect& pane_rect,
                         const editor::EditorViewMetrics& editor_metrics) {
          if (renderer == nullptr || viewport.path().empty() || viewport.is_placeholder()) {
            return;
          }

          const std::string uri = viewport.path().generic_string();
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
                editor_metrics.first_line_y + static_cast<float>(row) * editor_metrics.line_height;
            const SDL_FRect marker_rect = SDL_FRect{
                pane_rect.x + editor_metrics.gutter_width - 6.0f,
                y + std::max(1.0f, (editor_metrics.line_height - 6.0f) * 0.5f),
                3.0f,
                6.0f,
            };
            DrawFilledRect(renderer, marker_rect, theme_.accent);
          }
        };
    const auto setting_enabled = [this](std::string_view id, bool default_value) {
      return SettingFlagEnabled(GetSettingValue(id), default_value);
    };
    const bool bracket_match_highlight_enabled =
        setting_enabled("editor.brackets.match_highlight.enabled", true);
    const bool indent_guides_enabled =
        setting_enabled("editor.view.indent_guides.enabled", true);
    const bool render_whitespace_enabled =
        setting_enabled("editor.view.render_whitespace", false);
    // Phase E1: inline plugin-surface insets. Experimental and off by default, so
    // the editor geometry stays byte-for-byte identical for everyone else.
    const editor::InsetGapFeatureFlags inset_flags{
        .inline_surfaces = setting_enabled("plugins.inline_surfaces", false),
        .code_lens_above = setting_enabled("plugins.code_lens_above", false),
        .ghost_text = setting_enabled("plugins.ghost_text", false),
    };
    const bool fold_enabled = setting_enabled("editor.fold.enabled", true);
    const bool occurrences_highlight_enabled_global =
        setting_enabled("editor.occurrences.enabled", true);
    const bool occurrences_case_sensitive =
        setting_enabled("editor.search.case_sensitive", false);
    // Breakpoint gutter dots only render when the debugger is enabled (default
    // off), keeping the editor byte-for-byte unchanged for non-debug users.
    const bool debug_enabled = DebugEnabled();
    const editor::BreakpointStore* breakpoint_store =
        debug_enabled ? &project_state.breakpoint_store : nullptr;
    const DebugExecutionView* debug_execution =
        debug_enabled ? &project_state.debug_execution : nullptr;
    const editor::FoldingModel* active_folding_model = nullptr;
    if (!fold_enabled) {
      if (auto* editor_tab = ActiveEditorTab(); editor_tab != nullptr) {
        for (auto& view : editor_tab->views) {
          view.viewport.SetFoldingModel(nullptr);
        }
      }
    }
    const RenderViewModelBuilder editor_render_builder(context_);
    const bool sticky_scroll_setting_enabled =
        setting_enabled("editor.fold.sticky_scroll.enabled", true);
    const int sticky_scroll_max_depth =
        ParseStickyScrollMaxDepthSetting(GetSettingValue("editor.fold.sticky_scroll.max_depth"));
    thread_local editor::EditorViewModel tls_editor_surface_vm;
    const std::vector<EditorPaneLayout>& panes = editor_panes;
    thread_local std::vector<editor::EditorViewMetrics> tls_pane_scroll_metrics;
    thread_local std::vector<unsigned char> tls_pane_scroll_metrics_valid;
    tls_pane_scroll_metrics.resize(panes.size());
    tls_pane_scroll_metrics_valid.assign(panes.size(), 0);
    editor::TextViewport* active_viewport = ActiveEditorViewport();
    if (panes.empty() && active_viewport != nullptr && active_viewport->is_placeholder()) {
      if (active_editor_pane_rect != nullptr) {
        *active_editor_pane_rect = layout.editor_surface;
      }
      editor::EditorViewMetrics metrics =
          editor::EditorViewRenderer::ComputeMetrics(text_renderer_, *active_viewport,
                                                     layout.editor_surface, 0);
      active_viewport->SetViewportSize(metrics.visible_rows, metrics.visible_columns);
      active_folding_model = fold_enabled ? EnsureActiveFoldingModelFresh() : nullptr;
      const bool sticky_active =
          fold_enabled && sticky_scroll_setting_enabled && active_folding_model != nullptr;
      editor_render_builder.BuildEditorViewModelInto(
          tls_editor_surface_vm, *active_viewport, metrics.visible_rows, active_folding_model,
          occurrences_highlight_enabled_global, occurrences_case_sensitive, sticky_active,
          sticky_scroll_max_depth, render_whitespace_enabled, debug_enabled, breakpoint_store,
          debug_execution, inset_flags, metrics.line_height);
      if (!tls_editor_surface_vm.sticky_lines.empty()) {
        metrics = editor::EditorViewRenderer::ComputeMetrics(
            text_renderer_, *active_viewport, layout.editor_surface,
            tls_editor_surface_vm.sticky_lines.size());
        active_viewport->SetViewportSize(metrics.visible_rows, metrics.visible_columns);
        editor_render_builder.BuildEditorViewModelInto(
            tls_editor_surface_vm, *active_viewport, metrics.visible_rows, active_folding_model,
            occurrences_highlight_enabled_global, occurrences_case_sensitive, sticky_active,
            sticky_scroll_max_depth, render_whitespace_enabled, debug_enabled, breakpoint_store,
            debug_execution, inset_flags, metrics.line_height);
      }
      // The welcome home surface (recents + curated shortcuts + hints) is built in the
      // view-model layer so this render TU never materializes its strings.
      thread_local editor::WelcomeViewModel tls_welcome_vm;
      tls_welcome_vm = editor_render_builder.BuildWelcomeView(recents_service_.RecentProjects());
      editor_view_renderer_.Render(renderer, text_renderer_, theme_, *active_viewport,
                                   layout.editor_surface, draw_editor_caret, "", std::nullopt,
                                   std::nullopt, {}, &tls_editor_surface_vm,
                                   bracket_match_highlight_enabled,
                                   indent_guides_enabled, render_whitespace_enabled,
                                   active_folding_model, &tls_welcome_vm);
    }
    auto* editor_tab = ActiveEditorTab();
    for (std::size_t pane_index = 0; pane_index < panes.size(); ++pane_index) {
      const EditorPaneLayout& pane = panes[pane_index];
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
      editor::EditorViewMetrics metrics =
          editor::EditorViewRenderer::ComputeMetrics(text_renderer_, *viewport, pane.rect, 0);
      viewport->SetViewportSize(metrics.visible_rows, metrics.visible_columns);
      if (pane.active) {
        active_folding_model = fold_enabled ? EnsureActiveFoldingModelFresh() : nullptr;
      }
      const editor::FoldingModel* folding_for_vm =
          fold_enabled ? active_folding_model : nullptr;
      const bool sticky_active =
          fold_enabled && sticky_scroll_setting_enabled && folding_for_vm != nullptr;
      const bool occurrences_for_pane =
          occurrences_highlight_enabled_global && pane.active;
      editor_render_builder.BuildEditorViewModelInto(
          tls_editor_surface_vm, *viewport, metrics.visible_rows, folding_for_vm,
          occurrences_for_pane, occurrences_case_sensitive, sticky_active, sticky_scroll_max_depth,
          render_whitespace_enabled, debug_enabled, breakpoint_store, debug_execution,
          inset_flags, metrics.line_height);
      if (!tls_editor_surface_vm.sticky_lines.empty()) {
        metrics = editor::EditorViewRenderer::ComputeMetrics(
            text_renderer_, *viewport, pane.rect, tls_editor_surface_vm.sticky_lines.size());
        viewport->SetViewportSize(metrics.visible_rows, metrics.visible_columns);
        editor_render_builder.BuildEditorViewModelInto(
            tls_editor_surface_vm, *viewport, metrics.visible_rows, folding_for_vm,
            occurrences_for_pane, occurrences_case_sensitive, sticky_active, sticky_scroll_max_depth,
            render_whitespace_enabled, debug_enabled, breakpoint_store, debug_execution,
            inset_flags, metrics.line_height);
      }
      tls_pane_scroll_metrics[pane_index] = metrics;
      tls_pane_scroll_metrics_valid[pane_index] = 1;
      const auto blame_overlay =
          pane.active
              ? editor_blame_overlay_service_.BuildEditorOverlay(
                    project_state.root, text_renderer_, git_blame_service_, *viewport,
                    pane.rect, 520.0f, tls_editor_surface_vm.sticky_lines.size())
                      : std::nullopt;
      if (pane.active) {
        editor_blame_overlay_service_.SetVisibleOverlay(blame_overlay);
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
                                   &tls_editor_surface_vm,
                                   pane.active && bracket_match_highlight_enabled,
                                   indent_guides_enabled, render_whitespace_enabled,
                                   active_folding_model, nullptr,
                                   decorations_for_viewport(*viewport));
      DrawEditorInsets(renderer, pane.rect, metrics, viewport->scroll_line(),
                       tls_editor_surface_vm);
      draw_review_comment_markers(*viewport, pane.rect, metrics);

    }
    for (std::size_t pane_index = 0; pane_index < panes.size(); ++pane_index) {
      const EditorPaneLayout& pane = panes[pane_index];
      editor::TextViewport* viewport =
          pane.active ? ActiveEditorViewport()
                      : (editor_tab != nullptr ? FindEditorView(*editor_tab, pane.leaf_id)
                                               : nullptr);
      if (viewport == nullptr || viewport->is_placeholder() ||
          pane_index >= tls_pane_scroll_metrics_valid.size() ||
          tls_pane_scroll_metrics_valid[pane_index] == 0) {
        continue;
      }
      const editor::EditorViewMetrics& metrics = tls_pane_scroll_metrics[pane_index];
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

  if (active_compare_tab != nullptr) {
    RenderCompareScrollbars(renderer, layout.editor_surface, *active_compare_tab);
  } else if (active_merge_tab != nullptr) {
    RenderMergeScrollbars(renderer, layout.editor_surface);
  }

  if (frame_vm.editor_banner.has_value()) {
    const EditorBannerViewModel& banner_vm = *frame_vm.editor_banner;
    const SDL_FRect strip = ComputeEditorBannerStripRect(layout.editor_surface);
    DrawFilledRect(renderer, strip, theme_.chrome_background);
    DrawFilledRect(renderer,
                   MakeRect(strip.x, strip.y + strip.h - kWorkspaceDividerThickness, strip.w,
                            kWorkspaceDividerThickness),
                   theme_.border);
    DrawFilledRect(renderer, MakeRect(strip.x, strip.y, 3.0f, strip.h), theme_.accent);

    const EditorBannerButtonLayout buttons =
        ComputeEditorBannerButtonRects(strip, banner_vm.has_actions);
    const float message_right = banner_vm.has_actions ? buttons.reload.x : buttons.dismiss.x;
    const SDL_FRect message_rect = MakeRect(strip.x + 12.0f, strip.y,
                                            std::max(0.0f, message_right - strip.x - 20.0f), strip.h);
    const SDL_Rect clip{static_cast<int>(message_rect.x), static_cast<int>(message_rect.y),
                        static_cast<int>(message_rect.w), static_cast<int>(message_rect.h)};
    SDL_SetRenderClipRect(renderer, &clip);
    DrawVCenteredTextOn(text_renderer_, renderer, message_rect, 0.0f, theme_.text_secondary,
                        theme_.chrome_background, banner_vm.message);
    SDL_SetRenderClipRect(renderer, nullptr);

    const auto draw_banner_button = [&](const SDL_FRect& rect, std::string_view label,
                                        ButtonTone tone) {
      if (rect.w <= 0.0f) {
        return;
      }
      DrawButtonCentered(text_renderer_, renderer, theme_, rect, label, tone,
                         ButtonVisualState{
                             .enabled = true,
                             .hovered = last_mouse_position_valid_ &&
                                        Contains(rect, last_mouse_x_, last_mouse_y_),
                             .active = false,
                         });
    };
    if (banner_vm.has_actions) {
      draw_banner_button(buttons.reload, "Reload", ButtonTone::Neutral);
      draw_banner_button(buttons.overwrite, "Overwrite", ButtonTone::Destructive);
      draw_banner_button(buttons.keep, "Keep", ButtonTone::Neutral);
    }
    draw_banner_button(buttons.dismiss, "x", ButtonTone::Neutral);
  }

  if (project_state.surface.focus == FocusTarget::Editor) {
    DrawSurfaceFocusRing(renderer, layout.editor_surface);
  }

  // Floating debug control bar, drawn over the editor whenever a session is live.
  // Stacks below the find widget (which renders later, in the overlay layer) when
  // both are visible, so draw order between them is irrelevant.
  if (render_editor_surface && DebugToolbarVisible()) {
    RenderDebugToolbar(renderer, layout, IsDebugSessionStopped(),
                       DebugToolbarAvoidBelowY(layout));
  }
}

}  // namespace microide::workspace
