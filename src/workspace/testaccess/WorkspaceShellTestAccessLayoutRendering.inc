  static bool ProjectTabStripVisible(WorkspaceShell& shell) {
    return shell.ProjectTabStripVisible();
  }
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
  static SDL_FRect GroupTabStripRect(WorkspaceShell& shell, std::size_t group_index) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    const auto group_rects = shell.ComputeEditorGroupRectsForState(layout);
    return group_index < group_rects.groups.size() ? group_rects.groups[group_index].tab_strip
                                                    : SDL_FRect{};
  }
  // The editor PANE of a group -- the buffer area under its strip, which is what
  // the drag-to-split drop zones are carved out of.
  static SDL_FRect GroupEditorSurfaceRect(WorkspaceShell& shell, std::size_t group_index) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    const auto group_rects = shell.ComputeEditorGroupRectsForState(layout);
    return group_index < group_rects.groups.size() ? group_rects.groups[group_index].editor_surface
                                                   : SDL_FRect{};
  }
  // Every split divider the editor area currently shows, in visual order.
  static microide::util::InlineVector<microide::workspace::EditorSplitDividerRect,
                                      microide::workspace::kMaxEditorGroups - 1>
  EditorSplitDividerRects(WorkspaceShell& shell) {
    return shell.ComputeEditorGroupRectsForState(CurrentLayout(shell)).dividers;
  }
  static SDL_FRect GroupEditorTabRect(WorkspaceShell& shell, std::size_t group_index,
                                      std::size_t tab_index) {
    const SDL_FRect strip = GroupTabStripRect(shell, group_index);
    for (const WorkspaceShell::VisibleStripTab& tab :
         shell.tab_strip_chrome_.ComputeVisibleTabsForGroup(group_index, strip)) {
      if (tab.index == tab_index) {
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
    const auto& visible = shell.tab_strip_chrome_.ComputeVisibleTabs(layout.tab_strip);
    return shell.tab_strip_chrome_.ComputeTabOverflowControls(layout.tab_strip, visible);
  }
  static WorkspaceShell::TabStripOverflowControls ProjectTabOverflowControls(
      WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    const auto& visible =
        shell.tab_strip_chrome_.ComputeVisibleProjectTabs(layout.project_tab_strip);
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
  // The shell resolves one tooltip for the whole window (HoverTooltipSurface.cpp);
  // tests assert on that single answer, which also pins that no other provider
  // claimed the point.
  static std::string HoveredTooltipLabel(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    const auto tooltip = shell.HoveredTooltip(layout);
    return tooltip.has_value() ? tooltip->text : std::string{};
  }
  static std::optional<SDL_FRect> HoveredTooltipRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    const auto tooltip = shell.HoveredTooltip(layout);
    return tooltip.has_value() ? std::optional<SDL_FRect>(tooltip->rect) : std::nullopt;
  }
  static std::vector<WorkspaceShell::VisibleStatusItem> VisibleStatusItems(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return shell.ComputeVisibleStatusItems(layout.breadcrumb);
  }
  static std::vector<std::string> WrapHoverPopupText(const WorkspaceShell& shell,
                                                     std::string_view text,
                                                     float max_width,
                                                     std::size_t max_lines) {
    return shell.WrapEditorHoverPopupText(text, max_width, max_lines);
  }
  static std::array<SDL_FRect, 3> GitSidebarTopActionRects(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return {git_sidebar_header::StageAllButtonRect(layout.sidebar),
            git_sidebar_header::DiscardAllButtonRect(layout.sidebar),
            git_sidebar_header::RefreshButtonRect(layout.sidebar)};
  }
  static SDL_FRect GitSidebarBranchButtonRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return git_sidebar_header::BranchButtonRect(layout.sidebar);
  }
  static SDL_FRect GitSidebarSyncButtonRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return git_sidebar_header::SyncButtonRect(layout.sidebar);
  }
  static std::optional<SDL_FRect> GitSidebarOutgoingBaseButtonRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return shell.GitSidebarOutgoingBaseButtonRect(layout.sidebar);
  }
  static std::vector<std::string> GitSidebarLineLabels(WorkspaceShell& shell) {
    std::vector<std::string> labels;
    for (const auto& line : shell.BuildGitSidebarLines()) {
      labels.push_back(line.display_label);
    }
    return labels;
  }
  static SDL_FRect GitSidebarEntryRowRect(WorkspaceShell& shell, std::size_t entry_index) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    const auto& lines = shell.BuildGitSidebarLines();
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
    const auto panes = shell.ComputeEditorPaneLayouts(layout);
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
    const auto panes = shell.ComputeEditorPaneLayouts(layout);
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
  // The viewport a hit-test resolves for a given pane (split group), exercised by
  // hover/cursor/right-click paths. Returns the path each pane's group renders so a
  // test can assert the inactive pane resolves its own group, not the focused one.
  static std::vector<std::string> PaneViewportPaths(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    const auto panes = shell.ComputeEditorPaneLayouts(layout);
    std::vector<std::string> paths;
    paths.reserve(panes.size());
    for (const auto& pane : panes) {
      const microide::editor::TextViewport* viewport = shell.ViewportForPane(pane);
      paths.push_back(viewport != nullptr ? viewport->path().generic_string() : std::string{});
    }
    return paths;
  }
  static microide::editor::TextViewport* InactiveEditorPaneViewport(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    const auto panes = shell.ComputeEditorPaneLayouts(layout);
    for (const auto& pane : panes) {
      if (!pane.active) {
        return shell.ViewportForPane(pane);
      }
    }
    return nullptr;
  }
  static microide::editor::EditorViewMetrics InactiveEditorPaneMetrics(WorkspaceShell& shell) {
    const SDL_FRect pane = InactiveEditorPaneRect(shell);
    microide::editor::TextViewport* viewport = InactiveEditorPaneViewport(shell);
    if (viewport == nullptr) {
      return {};
    }
    return microide::editor::EditorViewRenderer::ComputeMetrics(shell.text_renderer_, *viewport,
                                                                pane);
  }
  static std::size_t ActiveEditorStickyScrollRows(WorkspaceShell& shell,
                                                  microide::editor::TextViewport& viewport,
                                                  const SDL_FRect& pane) {
    auto setting_enabled = [&](std::string_view id, bool default_value) -> bool {
      return SettingFlagEnabled(shell.GetSettingValue(id), default_value);
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
  }
  static std::optional<SDL_FRect> ActiveCompareRowRangeRect(WorkspaceShell& shell,
                                                            std::size_t start_row,
                                                            std::size_t end_row) {
    return shell.CurrentCompareRowRangeRect(start_row, end_row);
  }
  static std::optional<SDL_FRect> ActiveMergeConflictRect(WorkspaceShell& shell,
                                                          std::size_t conflict_index) {
    return shell.CurrentMergeConflictRect(conflict_index);
  }
  static float TextCharWidth(WorkspaceShell& shell) { return shell.text_renderer_.CharWidth(); }
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
    microide::editor::TextViewport* viewport = shell.ActiveEditorViewport();
    if (viewport == nullptr) {
      return std::nullopt;
    }
    const auto panes = shell.ComputeEditorPaneLayouts(layout);
    for (const auto& pane : panes) {
      if (pane.active) {
        return shell.editor_blame_overlay_service_.BuildEditorOverlay(
            shell.context_.current_project_state.root, shell.text_renderer_, shell.git_blame_service_,
            *viewport, pane.rect, 520.0f, ActiveEditorStickyScrollRows(shell, *viewport, pane.rect),
            shell.LineNumbersEnabled());
      }
    }
    return viewport->is_placeholder()
               ? shell.editor_blame_overlay_service_.BuildEditorOverlay(
                     shell.context_.current_project_state.root, shell.text_renderer_, shell.git_blame_service_,
                     *viewport, layout.editor_surface, 520.0f, 0, shell.LineNumbersEnabled())
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
        [&compare](std::size_t line_index, std::size_t line_visual_columns)
            -> std::optional<EditorBlameOverlayService::CompareBlameAnchor> {
          const CompareRightLineAnchor anchor =
              CompareRightLineBlameAnchor(compare, line_index, line_visual_columns);
          if (!anchor.valid) {
            return std::nullopt;
          }
          return EditorBlameOverlayService::CompareBlameAnchor{
              .visual_row = anchor.visual_row,
              .end_column = anchor.end_column,
          };
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
        merge.result_viewport, ActiveMergeResultRect(shell), 280.0f, 0, shell.LineNumbersEnabled());
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
  static SDL_FRect BottomPanelHeaderRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return MakeRect(layout.bottom_panel.x, layout.bottom_panel.y, layout.bottom_panel.w,
                    kWorkspaceBottomPanelHeaderHeight);
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
  static std::optional<SDL_FRect> WindowControlButtonRect(
      WorkspaceShell& shell,
      WorkspaceShell::WindowControlButtonId id) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    const auto buttons = shell.ComputeVisibleWindowControlButtons(layout.menu_bar);
    for (const auto& button : buttons) {
      if (button.id == id) {
        return button.rect;
      }
    }
    return std::nullopt;
  }
  static std::optional<SDL_FRect> MinimizeButtonRect(WorkspaceShell& shell) {
    return WindowControlButtonRect(shell, WorkspaceShell::WindowControlButtonId::Minimize);
  }
  static std::optional<SDL_FRect> MaximizeButtonRect(WorkspaceShell& shell) {
    return WindowControlButtonRect(shell, WorkspaceShell::WindowControlButtonId::Maximize);
  }
  static std::optional<SDL_FRect> CloseButtonRect(WorkspaceShell& shell) {
    return WindowControlButtonRect(shell, WorkspaceShell::WindowControlButtonId::Close);
  }
  static bool CachedCursorIs(const WorkspaceShell& shell, WorkspaceShell::CursorKind expected) {
    return shell.cursor_kind_ == expected;
  }
  static int CachedCursorValue(const WorkspaceShell& shell) {
    return static_cast<int>(shell.cursor_kind_);
  }
  static bool MousePositionValid(const WorkspaceShell& shell) {
    return shell.last_mouse_position_valid_;
  }
  // Consumes any pending render invalidation and reports whether one was queued.
  // Used to prove an event-time cursor change requests a present (so an idle
  // compositor recomposites and re-latches the hardware cursor plane).
  static bool ConsumePendingRedraw(WorkspaceShell& shell) {
    return shell.ConsumePendingRenderInvalidation().HasAnyRedraw();
  }
  static bool CachedCursorIsDefault(const WorkspaceShell& shell) {
    return shell.cursor_kind_ == WorkspaceShell::CursorKind::Default;
  }
  static bool CachedCursorIsNsResize(const WorkspaceShell& shell) {
    return shell.cursor_kind_ == WorkspaceShell::CursorKind::NsResize;
  }
  static bool CachedCursorIsEwResize(const WorkspaceShell& shell) {
    return shell.cursor_kind_ == WorkspaceShell::CursorKind::EwResize;
  }
  static bool CachedCursorIsPointer(const WorkspaceShell& shell) {
    return shell.cursor_kind_ == WorkspaceShell::CursorKind::Pointer;
  }
  static bool CachedCursorIsText(const WorkspaceShell& shell) {
    return shell.cursor_kind_ == WorkspaceShell::CursorKind::Text;
  }
  static bool CursorKindAtIs(WorkspaceShell& shell,
                             float x,
                             float y,
                             WorkspaceShell::CursorKind expected) {
    return shell.CursorKindForPosition(x, y) == expected;
  }
  static bool CursorKindAtIsDefault(WorkspaceShell& shell, float x, float y) {
    return shell.CursorKindForPosition(x, y) == WorkspaceShell::CursorKind::Default;
  }
  static bool CursorKindAtIsText(WorkspaceShell& shell, float x, float y) {
    return shell.CursorKindForPosition(x, y) == WorkspaceShell::CursorKind::Text;
  }
  static bool CursorKindAtIsNsResize(WorkspaceShell& shell, float x, float y) {
    return shell.CursorKindForPosition(x, y) == WorkspaceShell::CursorKind::NsResize;
  }
  static bool CursorKindAtIsEwResize(WorkspaceShell& shell, float x, float y) {
    return shell.CursorKindForPosition(x, y) == WorkspaceShell::CursorKind::EwResize;
  }
  static bool CursorKindAtIsPointer(WorkspaceShell& shell, float x, float y) {
    return shell.CursorKindForPosition(x, y) == WorkspaceShell::CursorKind::Pointer;
  }
  static bool CursorKindAtIsNResize(WorkspaceShell& shell, float x, float y) {
    return shell.CursorKindForPosition(x, y) == WorkspaceShell::CursorKind::NResize;
  }
  static bool CursorKindAtIsEResize(WorkspaceShell& shell, float x, float y) {
    return shell.CursorKindForPosition(x, y) == WorkspaceShell::CursorKind::EResize;
  }
  static bool CursorKindAtIsSResize(WorkspaceShell& shell, float x, float y) {
    return shell.CursorKindForPosition(x, y) == WorkspaceShell::CursorKind::SResize;
  }
  static bool CursorKindAtIsWResize(WorkspaceShell& shell, float x, float y) {
    return shell.CursorKindForPosition(x, y) == WorkspaceShell::CursorKind::WResize;
  }
  static bool CursorKindAtIsNeResize(WorkspaceShell& shell, float x, float y) {
    return shell.CursorKindForPosition(x, y) == WorkspaceShell::CursorKind::NeResize;
  }
  static bool CursorKindAtIsSeResize(WorkspaceShell& shell, float x, float y) {
    return shell.CursorKindForPosition(x, y) == WorkspaceShell::CursorKind::SeResize;
  }
  static bool CursorKindAtIsSwResize(WorkspaceShell& shell, float x, float y) {
    return shell.CursorKindForPosition(x, y) == WorkspaceShell::CursorKind::SwResize;
  }
  static bool CursorKindAtIsNwResize(WorkspaceShell& shell, float x, float y) {
    return shell.CursorKindForPosition(x, y) == WorkspaceShell::CursorKind::NwResize;
  }
  static bool CachedCursorIsNResize(const WorkspaceShell& shell) {
    return shell.cursor_kind_ == WorkspaceShell::CursorKind::NResize;
  }
  static bool CachedCursorIsEResize(const WorkspaceShell& shell) {
    return shell.cursor_kind_ == WorkspaceShell::CursorKind::EResize;
  }
  static bool CachedCursorIsSResize(const WorkspaceShell& shell) {
    return shell.cursor_kind_ == WorkspaceShell::CursorKind::SResize;
  }
  static bool CachedCursorIsWResize(const WorkspaceShell& shell) {
    return shell.cursor_kind_ == WorkspaceShell::CursorKind::WResize;
  }
  static bool CachedCursorIsNeResize(const WorkspaceShell& shell) {
    return shell.cursor_kind_ == WorkspaceShell::CursorKind::NeResize;
  }
  static bool CachedCursorIsSeResize(const WorkspaceShell& shell) {
    return shell.cursor_kind_ == WorkspaceShell::CursorKind::SeResize;
  }
  static bool CachedCursorIsSwResize(const WorkspaceShell& shell) {
    return shell.cursor_kind_ == WorkspaceShell::CursorKind::SwResize;
  }
  static bool CachedCursorIsNwResize(const WorkspaceShell& shell) {
    return shell.cursor_kind_ == WorkspaceShell::CursorKind::NwResize;
  }
  static int CursorKindAtValue(WorkspaceShell& shell, float x, float y) {
    return static_cast<int>(shell.CursorKindForPosition(x, y));
  }
  static void UpdateMouseCursor(WorkspaceShell& shell, float x, float y) {
    shell.UpdateMouseCursor(x, y, false, false);
  }
  static void InvalidateCursorKindFingerprint(WorkspaceShell& shell) {
    shell.InvalidateCursorKindFingerprint();
  }
  static void CloseTerminalTab(WorkspaceShell& shell, std::size_t index) {
    shell.CloseTerminalTab(index);
  }
  static void MarkLayoutDirty(WorkspaceShell& shell) { shell.MarkLayoutDirty(); }
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
