    const WorkspaceLayout layout = CurrentLayout(shell);
    for (const WorkspaceShell::VisibleStripTab& tab :
         shell.ComputeVisibleProjectTabs(layout.project_tab_strip)) {
      if (tab.index == index) {
        return tab.chat_status;
      }
    }
    return WorkspaceShell::VisibleStripTab::ChatStatus::None;
  }
  static std::string ProjectTabBadgeText(WorkspaceShell& shell, std::size_t index) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    for (const WorkspaceShell::VisibleStripTab& tab :
         shell.ComputeVisibleProjectTabs(layout.project_tab_strip)) {
      if (tab.index == index) {
        return tab.badge_text;
      }
    }
    return {};
  }
  static bool ProjectTabShowsBadge(WorkspaceShell& shell, std::size_t index) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    for (const WorkspaceShell::VisibleStripTab& tab :
         shell.ComputeVisibleProjectTabs(layout.project_tab_strip)) {
      if (tab.index == index) {
        return tab.show_badge;
      }
    }
    return false;
  }
  static SDL_FRect EditorTabRect(WorkspaceShell& shell, std::size_t index) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    for (const WorkspaceShell::VisibleStripTab& tab : shell.ComputeVisibleTabs(layout.tab_strip)) {
      if (tab.index == index) {
        return tab.rect;
      }
    }
    return {};
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
  static std::array<SDL_FRect, 2> GitSidebarEntryActionRects(WorkspaceShell& shell,
                                                             std::size_t entry_index) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    const auto lines = shell.BuildGitSidebarLines();
    const auto list_layout = shell.ComputeGitSidebarListLayout(layout.sidebar, lines.size());
    for (std::size_t i = 0; i < lines.size(); ++i) {
      if (lines[i].entry_index < 0 || static_cast<std::size_t>(lines[i].entry_index) != entry_index) {
        continue;
      }
      const SDL_FRect row_rect = ScrollableListRowRect(
          list_layout, static_cast<int>(i) - list_layout.scroll_row);
      const auto actions = shell.ComputeGitSidebarEntryActionLayout(
          row_rect, shell.context_.current_project_state.sidebar.git.entries[entry_index]);
      return {actions.primary_rect.value_or(SDL_FRect{}),
              actions.discard_rect.value_or(SDL_FRect{})};
    }
    return {};
  }
  static SDL_FRect GitSidebarEntryRowRect(WorkspaceShell& shell, std::size_t entry_index) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    const auto lines = shell.BuildGitSidebarLines();
    const auto list_layout = shell.ComputeGitSidebarListLayout(layout.sidebar, lines.size());
    for (std::size_t i = 0; i < lines.size(); ++i) {
      if (lines[i].entry_index < 0 || static_cast<std::size_t>(lines[i].entry_index) != entry_index) {
        continue;
      }
      return ScrollableListRowRect(
          list_layout, static_cast<int>(i) - list_layout.scroll_row);
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
        return shell.BuildEditorBlameOverlay(*viewport, pane.rect);
      }
    }
    return viewport->is_placeholder()
               ? shell.BuildEditorBlameOverlay(*viewport, layout.editor_surface)
               : std::nullopt;
  }
  static std::optional<microide::editor::EditorBlameOverlay> ActiveCompareBlameOverlay(
      WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    if (!shell.ActiveTabIsCompare()) {
      return std::nullopt;
    }
    auto& compare = ActiveCompare(shell);
    return shell.BuildCompareBlameOverlay(compare, shell.ComputeCompareSurfaceLayout(layout.editor_surface, compare),
                                          layout.editor_surface);
  }
  static std::optional<microide::editor::EditorBlameOverlay> ActiveMergeBlameOverlay(
      WorkspaceShell& shell) {
    if (!shell.ActiveTabIsMerge()) {
      return std::nullopt;
    }
    auto& merge = ActiveMerge(shell);
    return shell.BuildEditorBlameOverlay(merge.result_viewport, ActiveMergeResultRect(shell), 280.0f);
  }
  static void SetVisibleEditorBlameOverlay(
      WorkspaceShell& shell,
      std::optional<microide::editor::EditorBlameOverlay> overlay) {
    shell.visible_editor_blame_overlay_ = std::move(overlay);
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
    for (const WorkspaceShell::VisibleStripTab& tab : shell.ComputeVisibleTerminalTabs(panel_header)) {
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
    for (const WorkspaceShell::VisibleStripTab& tab :
         shell.ComputeVisibleBottomPanelTabs(panel_header)) {
      labels.push_back(tab.display_title);
    }
    return labels;
  }
  static std::optional<SDL_FRect> BottomPanelTabRectByTitle(WorkspaceShell& shell,
                                                             std::string_view title) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    const SDL_FRect panel_header =
        MakeRect(layout.bottom_panel.x, layout.bottom_panel.y, layout.bottom_panel.w, 28.0f);
    for (const WorkspaceShell::VisibleStripTab& tab :
         shell.ComputeVisibleBottomPanelTabs(panel_header)) {
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
    for (const WorkspaceShell::VisibleStripTab& tab : shell.ComputeVisibleTerminalTabs(panel_header)) {
      if (tab.index == index) {
        return tab.rect;
      }
    }
    return {};
  }
