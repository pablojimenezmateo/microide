  static std::string ProjectTabBadgeText(WorkspaceShell& shell, std::size_t index) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    for (const WorkspaceShell::VisibleStripTab& tab :
         shell.tab_strip_chrome_.ComputeVisibleProjectTabs(layout.project_tab_strip)) {
      if (tab.index == index) {
        return tab.badge_text;
      }
    }
    return {};
  }
  static bool ProjectTabShowsBadge(WorkspaceShell& shell, std::size_t index) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    for (const WorkspaceShell::VisibleStripTab& tab :
         shell.tab_strip_chrome_.ComputeVisibleProjectTabs(layout.project_tab_strip)) {
      if (tab.index == index) {
        return tab.show_badge;
      }
    }
    return false;
  }
  static SDL_Color ProjectTabBadgeColor(WorkspaceShell& shell, std::size_t index) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    for (const WorkspaceShell::VisibleStripTab& tab :
         shell.tab_strip_chrome_.ComputeVisibleProjectTabs(layout.project_tab_strip)) {
      if (tab.index == index) {
        return tab.badge_color;
      }
    }
    return SDL_Color{};
  }
  static SDL_FRect EditorTabRect(WorkspaceShell& shell, std::size_t index) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    for (const WorkspaceShell::VisibleStripTab& tab : shell.tab_strip_chrome_.ComputeVisibleTabs(layout.tab_strip)) {
      if (tab.index == index) {
        return tab.rect;
      }
    }
    return {};
  }
  static std::vector<std::string> EditorTabDisplayTitles(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    std::vector<std::string> titles;
    for (const WorkspaceShell::VisibleStripTab& tab : shell.tab_strip_chrome_.ComputeVisibleTabs(layout.tab_strip)) {
      titles.push_back(tab.display_title);
    }
    return titles;
  }
  static WorkspaceShell::TabStripOverflowControls EditorTabOverflowControls(
      WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    const auto visible = shell.tab_strip_chrome_.ComputeVisibleTabs(layout.tab_strip);
    return shell.tab_strip_chrome_.ComputeTabOverflowControls(layout.tab_strip, visible);
  }
  static WorkspaceShell::TabStripOverflowControls ProjectTabOverflowControls(
      WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    const auto visible = shell.tab_strip_chrome_.ComputeVisibleProjectTabs(layout.project_tab_strip);
    return shell.tab_strip_chrome_.ComputeProjectTabOverflowControls(layout.project_tab_strip, visible);
  }
  static bool ScrollEditorTabStrip(WorkspaceShell& shell, int direction) {
    return shell.tab_strip_chrome_.ScrollEditorTabStrip(direction);
  }
  static bool ScrollProjectTabStrip(WorkspaceShell& shell, int direction) {
    return shell.tab_strip_chrome_.ScrollProjectTabStrip(direction);
  }
  static std::string TabDisplayTitle(WorkspaceShell& shell, std::size_t index) {
    return shell.TabDisplayTitle(index);
  }
  static std::string TabTooltipLabel(WorkspaceShell& shell, std::size_t index) {
    return shell.TabTooltipLabel(index);
  }
  static std::string HoveredTabTooltipLabel(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return shell.HoveredTabTooltipLabel(layout.tab_strip);
  }
  static std::optional<SDL_FRect> HoveredTabTooltipRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return shell.HoveredTabTooltipRect(layout);
  }
  static std::optional<SDL_FRect> HoveredProjectTabTooltipRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return shell.HoveredProjectTabTooltipRect(layout);
  }
  static std::vector<WorkspaceShell::VisibleStatusItem> VisibleStatusItems(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return shell.ComputeVisibleStatusItems(layout.breadcrumb);
  }
  static std::string HoveredStatusTooltipLabel(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return shell.HoveredStatusTooltip(layout.breadcrumb);
  }
  static std::string HoveredGitSidebarTooltipLabel(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return shell.HoveredGitSidebarTooltipLabel(layout.sidebar);
  }
  static std::optional<SDL_FRect> HoveredGitSidebarTooltipRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    const std::string label = shell.HoveredGitSidebarTooltipLabel(layout.sidebar);
    if (label.empty()) {
      return std::nullopt;
    }
    const auto tooltip = detail::BuildTooltipLayout(
        shell.text_renderer_, label, std::max(160.0f, layout.full.w - 24.0f));
    const float tooltip_x =
        std::clamp(shell.last_mouse_x_ + 12.0f, layout.full.x + 8.0f,
                   layout.full.x + layout.full.w - tooltip.rect.w - 8.0f);
    const float tooltip_y =
        shell.last_mouse_y_ - tooltip.rect.h - 10.0f >= layout.full.y + 8.0f
            ? shell.last_mouse_y_ - tooltip.rect.h - 10.0f
            : std::clamp(shell.last_mouse_y_ + 14.0f, layout.full.y + 8.0f,
                         layout.full.y + layout.full.h - tooltip.rect.h - 8.0f);
    return MakeRect(tooltip_x, tooltip_y, tooltip.rect.w, tooltip.rect.h);
  }
  static std::array<SDL_FRect, 3> GitSidebarTopActionRects(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return {shell.GitSidebarStageAllButtonRect(layout.sidebar),
            shell.GitSidebarDiscardAllButtonRect(layout.sidebar),
            shell.GitSidebarRefreshButtonRect(layout.sidebar)};
  }
  static std::optional<SDL_FRect> GitSidebarOutgoingBaseButtonRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return shell.GitSidebarOutgoingBaseButtonRect(layout.sidebar);
  }
  static std::array<SDL_FRect, 2> GitSidebarEntryActionRects(WorkspaceShell& shell,
                                                             std::size_t entry_index) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    const auto lines = shell.BuildGitSidebarLines();
    auto list_layout = shell.ComputeGitSidebarListLayout(layout.sidebar, lines.size());
    for (std::size_t i = 0; i < lines.size(); ++i) {
      if (lines[i].entry_index < 0 || static_cast<std::size_t>(lines[i].entry_index) != entry_index) {
        continue;
      }
      shell.context_.current_project_state.sidebar.scroll_row =
          RevealScrollableListIndex(list_layout, static_cast<int>(i));
      list_layout = shell.ComputeGitSidebarListLayout(layout.sidebar, lines.size());
      const int visible_row = static_cast<int>(i) - list_layout.scroll_row;
      const SDL_FRect row_rect = ScrollableListRowRect(list_layout, visible_row);
      const auto& entry = shell.context_.current_project_state.sidebar.git.entries[entry_index];
      const auto actions = shell.ComputeGitSidebarEntryActionLayout(row_rect, entry);
      return {actions.primary_rect.value_or(SDL_FRect{}),
              actions.discard_rect.value_or(SDL_FRect{})};
    }
    return {};
  }
  static SDL_FRect GitSidebarEntryRowRect(WorkspaceShell& shell, std::size_t entry_index) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    const auto lines = shell.BuildGitSidebarLines();
    auto list_layout = shell.ComputeGitSidebarListLayout(layout.sidebar, lines.size());
    for (std::size_t i = 0; i < lines.size(); ++i) {
      if (lines[i].entry_index < 0 || static_cast<std::size_t>(lines[i].entry_index) != entry_index) {
        continue;
      }
      shell.context_.current_project_state.sidebar.scroll_row =
          RevealScrollableListIndex(list_layout, static_cast<int>(i));
      list_layout = shell.ComputeGitSidebarListLayout(layout.sidebar, lines.size());
      return ScrollableListRowRect(list_layout, static_cast<int>(i) - list_layout.scroll_row);
    }
    return {};
  }
  static SDL_FRect ActiveEditorPaneRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    if (!shell.ActiveTabIsEditor()) {
      return {};
    }
    shell.SyncActiveEditorTab();
    auto* editor_tab = shell.ActiveEditorTab();
    if (editor_tab == nullptr) {
      return {};
    }
    shell.NormalizeEditorSplitTree(*editor_tab);
    const auto panes = shell.ComputeEditorPaneLayouts(layout.editor_surface);
    for (const auto& pane : panes) {
      if (pane.active) {
        return pane.rect;
      }
    }
    return layout.editor_surface;
  }
  static SDL_FRect InactiveEditorPaneRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    if (!shell.ActiveTabIsEditor()) {
      return {};
    }
    shell.SyncActiveEditorTab();
    auto* editor_tab = shell.ActiveEditorTab();
    if (editor_tab == nullptr) {
      return {};
    }
    shell.NormalizeEditorSplitTree(*editor_tab);
    const auto panes = shell.ComputeEditorPaneLayouts(layout.editor_surface);
    for (const auto& pane : panes) {
      if (!pane.active) {
        return pane.rect;
      }
    }
    return {};
  }
  static microide::editor::EditorViewMetrics ActiveEditorMetrics(WorkspaceShell& shell) {
    const SDL_FRect pane = ActiveEditorPaneRect(shell);
    return microide::editor::EditorViewRenderer::ComputeMetrics(shell.text_renderer_,
                                                                *shell.ActiveEditorViewport(),
                                                                pane);
  }
  static std::size_t ActiveEditorStickyScrollRows(WorkspaceShell& shell,
                                                  microide::editor::TextViewport& viewport,
                                                  const SDL_FRect& pane) {
    auto setting_enabled = [&](std::string_view id, bool default_value) -> bool {
      const auto value = shell.GetSettingValue(id);
      if (!value.has_value()) {
        return default_value;
      }
      return *value != "false" && *value != "0" && *value != "off";
    };
    const bool fold_enabled = setting_enabled("editor.fold.enabled", true);
    const bool sticky_setting = setting_enabled("editor.fold.sticky_scroll.enabled", true);
    const int sticky_max_depth =
        ParseStickyScrollMaxDepthSetting(shell.GetSettingValue("editor.fold.sticky_scroll.max_depth"));
    const microide::editor::EditorViewMetrics base_metrics =
        microide::editor::EditorViewRenderer::ComputeMetrics(shell.text_renderer_, viewport, pane, 0);
    viewport.SetViewportSize(base_metrics.visible_rows, base_metrics.visible_columns);
    microide::editor::FoldingModel* folding_model =
        fold_enabled ? shell.EnsureActiveFoldingModelFresh() : nullptr;
    const bool sticky_active = fold_enabled && sticky_setting && folding_model != nullptr;
    std::vector<std::size_t> sticky_lines;
    ComputeStickyScrollLinesUncached(viewport, folding_model, sticky_active, sticky_max_depth,
                                     sticky_lines);
    return sticky_lines.size();
  }
  static microide::editor::EditorViewMetrics ActiveEditorRenderMetrics(WorkspaceShell& shell) {
    const SDL_FRect pane = ActiveEditorPaneRect(shell);
    auto* viewport = shell.ActiveEditorViewport();
    if (viewport == nullptr) {
      return {};
    }
    const std::size_t sticky_rows = ActiveEditorStickyScrollRows(shell, *viewport, pane);
    const microide::editor::EditorViewMetrics metrics =
        microide::editor::EditorViewRenderer::ComputeMetrics(shell.text_renderer_, *viewport, pane,
                                                             sticky_rows);
    viewport->SetViewportSize(metrics.visible_rows, metrics.visible_columns);
    return metrics;
  }
  static std::optional<SDL_FRect> ActiveEditorLineRangeRect(WorkspaceShell& shell,
                                                            std::size_t start_line,
                                                            std::size_t end_line) {
    return shell.CurrentEditorLineRangeRect(start_line, end_line);
  }  static std::optional<SDL_FRect> ActiveCompareRowRangeRect(WorkspaceShell& shell,
                                                            std::size_t start_row,
                                                            std::size_t end_row) {
    return shell.CurrentCompareRowRangeRect(start_row, end_row);
  }  static std::optional<SDL_FRect> ActiveMergeConflictRect(WorkspaceShell& shell,
                                                          std::size_t conflict_index) {
    return shell.CurrentMergeConflictRect(conflict_index);
  }  static float TextCharWidth(WorkspaceShell& shell) { return shell.text_renderer_.CharWidth(); }
  static float TextLineHeight(WorkspaceShell& shell) { return shell.text_renderer_.LineHeight(); }
  static std::optional<microide::editor::EditorBlameOverlay> ActiveEditorBlameOverlay(
      WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    if (!shell.ActiveTabIsEditor()) {
      return std::nullopt;
    }
    auto* editor_tab = shell.ActiveEditorTab();
    if (editor_tab == nullptr) {
      return std::nullopt;
    }
    shell.NormalizeEditorSplitTree(*editor_tab);
    microide::editor::TextViewport* viewport = shell.ActiveEditorViewport();
    if (viewport == nullptr) {
      return std::nullopt;
    }
    const auto panes = shell.ComputeEditorPaneLayouts(layout.editor_surface);
    for (const auto& pane : panes) {
      if (pane.active) {
        return shell.editor_blame_overlay_service_.BuildEditorOverlay(
            shell.context_.current_project_state.root, shell.text_renderer_, shell.git_blame_service_,
            *viewport, pane.rect, 520.0f, ActiveEditorStickyScrollRows(shell, *viewport, pane.rect));
      }
    }
    return viewport->is_placeholder()
               ? shell.editor_blame_overlay_service_.BuildEditorOverlay(
                     shell.context_.current_project_state.root, shell.text_renderer_, shell.git_blame_service_,
                     *viewport, layout.editor_surface, 520.0f, 0)
               : std::nullopt;
  }
  static std::optional<microide::editor::EditorBlameOverlay> ActiveCompareBlameOverlay(
      WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    if (!shell.ActiveTabIsCompare()) {
      return std::nullopt;
    }
    auto& compare = ActiveCompare(shell);
    const auto surface = shell.ComputeCompareSurfaceLayout(layout.editor_surface, compare);
    return shell.editor_blame_overlay_service_.BuildCompareOverlay(
        shell.context_.current_project_state.root, shell.text_renderer_, shell.git_blame_service_,
        compare,
        EditorBlameOverlayService::CompareOverlayLayout{
            .pane_rect = MakeRect(surface.right_x, layout.editor_surface.y,
                                  surface.gutter_width + surface.right_width,
                                  std::max(0.0f, layout.editor_surface.h - (surface.show_horizontal ? 12.0f : 0.0f))),
            .right_x = surface.right_x,
            .gutter_width = surface.gutter_width,
            .right_width = surface.right_width,
            .rows_y = surface.rows_y,
            .line_height = surface.line_height,
            .visible_rows = surface.visible_rows,
            .visible_columns = surface.visible_columns,
        },
        [&shell, &compare](std::size_t line_index) {
          return shell.CompareRowIndexForRightLine(compare, line_index);
        });
  }
  static std::optional<microide::editor::EditorBlameOverlay> ActiveMergeBlameOverlay(
      WorkspaceShell& shell) {
    if (!shell.ActiveTabIsMerge()) {
      return std::nullopt;
    }
    auto& merge = ActiveMerge(shell);
    return shell.editor_blame_overlay_service_.BuildEditorOverlay(
        shell.context_.current_project_state.root, shell.text_renderer_, shell.git_blame_service_,
        merge.result_viewport, ActiveMergeResultRect(shell), 280.0f, 0);
  }
  static void SetVisibleEditorBlameOverlay(
      WorkspaceShell& shell,
      std::optional<microide::editor::EditorBlameOverlay> overlay) {
    shell.editor_blame_overlay_service_.SetVisibleOverlay(std::move(overlay));
  }
  static std::optional<SDL_FRect> ActiveEditorHoverPopupRect(WorkspaceShell& shell) {
    const auto popup = shell.ActiveEditorHoverPopupLayout();
    return popup.has_value() ? std::make_optional(popup->rect) : std::nullopt;
  }
  static std::optional<std::string> ActiveEditorDiagnosticHoverMessage(WorkspaceShell& shell) {
    const auto popup = shell.ActiveEditorHoverPopupLayout();
    return popup.has_value() && popup->diagnostic.has_value()
               ? std::make_optional(popup->diagnostic->message)
               : std::nullopt;
  }
  static std::optional<std::string> ActiveEditorPluginHoverContent(WorkspaceShell& shell) {
    const auto popup = shell.ActiveEditorHoverPopupLayout();
    return popup.has_value() && popup->plugin_hover.has_value()
               ? std::make_optional(popup->plugin_hover->content)
               : std::nullopt;
  }
  static std::optional<SDL_FRect> ActiveEditorBlamePopupRect(WorkspaceShell& shell) {
    const auto popup = shell.ActiveEditorBlamePopupLayout();
    return popup.has_value() ? std::make_optional(popup->rect) : std::nullopt;
  }
  static std::optional<SDL_FRect> ActiveEditorBlamePopupCopyShaRect(WorkspaceShell& shell) {
    const auto popup = shell.ActiveEditorBlamePopupLayout();
    return popup.has_value() ? std::make_optional(popup->copy_sha_rect) : std::nullopt;
  }
  static SDL_FRect ActiveTerminalTabRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    const SDL_FRect panel_header =
        MakeRect(layout.bottom_panel.x, layout.bottom_panel.y, layout.bottom_panel.w, 28.0f);
    const std::vector<WorkspaceShell::VisibleStripTab> visible_tabs =
        shell.tab_strip_service_.ComputeVisibleTerminalTabs(
            shell.context_.current_project_state, panel_header,
            shell.layout_mode_service_.CurrentMode(),
            [&shell](std::string_view text) { return shell.text_renderer_.MeasureWidth(text); });
    for (const WorkspaceShell::VisibleStripTab& tab : visible_tabs) {
      if (tab.index == shell.context_.current_project_state.active_terminal_tab_index) {
        return tab.rect;
      }
    }
    return {};
  }
  static std::vector<std::string> BottomPanelTabDisplayTitles(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    const SDL_FRect panel_header =
        MakeRect(layout.bottom_panel.x, layout.bottom_panel.y, layout.bottom_panel.w, 28.0f);
    std::vector<std::string> labels;
    const std::vector<WorkspaceShell::VisibleStripTab> visible_tabs =
        shell.tab_strip_service_.ComputeVisibleBottomPanelTabs(
            shell.context_.current_project_state, panel_header,
            shell.layout_mode_service_.CurrentMode(),
            [&shell](std::string_view text) { return shell.text_renderer_.MeasureWidth(text); },
            shell.output_channels_.Channels());
    for (const WorkspaceShell::VisibleStripTab& tab : visible_tabs) {
      labels.push_back(tab.display_title);
    }
    return labels;
  }
  static std::optional<SDL_FRect> BottomPanelTabRectByTitle(WorkspaceShell& shell,
                                                             std::string_view title) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    const SDL_FRect panel_header =
        MakeRect(layout.bottom_panel.x, layout.bottom_panel.y, layout.bottom_panel.w, 28.0f);
    const std::vector<WorkspaceShell::VisibleStripTab> visible_tabs =
        shell.tab_strip_service_.ComputeVisibleBottomPanelTabs(
            shell.context_.current_project_state, panel_header,
            shell.layout_mode_service_.CurrentMode(),
            [&shell](std::string_view text) { return shell.text_renderer_.MeasureWidth(text); },
            shell.output_channels_.Channels());
    for (const WorkspaceShell::VisibleStripTab& tab : visible_tabs) {
      if (tab.display_title == title) {
        return tab.rect;
      }
    }
    return std::nullopt;
  }
  static SDL_FRect TerminalTabRect(WorkspaceShell& shell, std::size_t index) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    const SDL_FRect panel_header =
        MakeRect(layout.bottom_panel.x, layout.bottom_panel.y, layout.bottom_panel.w, 28.0f);
    const std::vector<WorkspaceShell::VisibleStripTab> visible_tabs =
        shell.tab_strip_service_.ComputeVisibleTerminalTabs(
            shell.context_.current_project_state, panel_header,
            shell.layout_mode_service_.CurrentMode(),
            [&shell](std::string_view text) { return shell.text_renderer_.MeasureWidth(text); });
    for (const WorkspaceShell::VisibleStripTab& tab : visible_tabs) {
      if (tab.index == index) {
        return tab.rect;
      }
    }
    return {};
  }
