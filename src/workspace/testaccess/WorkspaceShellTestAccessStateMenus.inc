  static bool RestoreSessionState(WorkspaceShell& shell) {
    return shell.MakePersistenceCoordinator().RestoreSessionState();
  }
  static bool RestoreUserConfig(WorkspaceShell& shell) {
    return shell.MakePersistenceCoordinator().RestoreUserConfig();
  }
  static void FlushPendingStateWrites(WorkspaceShell& shell) {
    shell.persistence_service_.FlushPendingWrites();
  }

  static bool RestoreConfigState(WorkspaceShell& shell) {
    return shell.MakePersistenceCoordinator().RestoreConfigState();
  }
  static void SaveSessionState(WorkspaceShell& shell) {
    shell.MakePersistenceCoordinator().SaveSessionState();
    FlushPendingStateWrites(shell);
  }
  static void FlushSessionStateForCrashSafety(WorkspaceShell& shell) {
    shell.FlushSessionStateForCrashSafety();
    FlushPendingStateWrites(shell);
  }
  static void ArmSessionFlushTimer(WorkspaceShell& shell) { shell.MaybeArmSessionFlushTimer(); }
  static bool SessionFlushArmed(WorkspaceShell& shell) { return shell.session_flush_.armed(); }
  static bool ActivateCurrentTabAfterStateLoad(WorkspaceShell& shell) {
    return shell.ActivateCurrentTabAfterStateLoad();
  }
  static bool RestoreWorkspaceSession(WorkspaceShell& shell) {
    return shell.MakePersistenceCoordinator().RestoreWorkspaceSession();
  }
  static void SaveWorkspaceSession(WorkspaceShell& shell) {
    shell.MakePersistenceCoordinator().SaveWorkspaceSession();
    FlushPendingStateWrites(shell);
  }
  static void SaveConfigState(WorkspaceShell& shell) {
    shell.MakePersistenceCoordinator().SaveConfigState();
    FlushPendingStateWrites(shell);
  }
  static void ConfirmDirtyPrompt(WorkspaceShell& shell, int selected_action) {
    shell.context_.prompts.dirty.selected_action = selected_action;
    shell.ConfirmDirtyPrompt();
    shell.file_io_.writes.Flush();
  }
  static void ShowDirtyPromptForQuit(WorkspaceShell& shell) {
    shell.ShowDirtyPromptForQuit();
  }
  static bool StageGitSidebarEntry(WorkspaceShell& shell, std::size_t entry_index) {
    return shell.StageGitSidebarEntry(entry_index);
  }
  static bool DirtyPromptVisible(const WorkspaceShell& shell) { return shell.context_.prompts.dirty_visible; }
  static std::size_t DirtyPromptCount(const WorkspaceShell& shell) {
    return shell.context_.prompts.dirty.dirty_count;
  }
  static std::string DirtyPromptMessage(const WorkspaceShell& shell) {
    return shell.DirtyPromptMessage();
  }
  static bool HasExternalChangeBanner(const WorkspaceShell& shell,
                                      const std::filesystem::path& path) {
    const std::filesystem::path normalized = path.lexically_normal();
    for (const EditorBannerState& banner : shell.context_.current_project_state.editor_banners) {
      if (banner.kind == EditorBannerState::Kind::ExternalChange && banner.path == normalized) {
        return true;
      }
    }
    return false;
  }
  static std::string DescribeEditorBanners(const WorkspaceShell& shell) {
    std::string description;
    for (const EditorBannerState& banner : shell.context_.current_project_state.editor_banners) {
      if (!description.empty()) {
        description += ", ";
      }
      description += banner.kind == EditorBannerState::Kind::ExternalChange ? "external:"
                     : banner.kind == EditorBannerState::Kind::OpenFailed   ? "open-failed:"
                     : banner.kind == EditorBannerState::Kind::ReadOnly     ? "read-only:"
                                                                            : "reloaded:";
      description += banner.path.string();
    }
    return description.empty() ? std::string("none") : description;
  }
  static bool HasEditorBanner(const WorkspaceShell& shell, const std::filesystem::path& path,
                              EditorBannerState::Kind kind) {
    const std::filesystem::path normalized = path.lexically_normal();
    for (const EditorBannerState& banner : shell.context_.current_project_state.editor_banners) {
      if (banner.kind == kind && banner.path == normalized) {
        return true;
      }
    }
    return false;
  }
  static void ActivateEditorBanner(WorkspaceShell& shell, EditorBannerAction action,
                                   const std::filesystem::path& path) {
    shell.ActivateEditorBannerAction(action, path);
  }
  static void ClearEditorBannersForTesting(WorkspaceShell& shell) {
    shell.context_.current_project_state.editor_banners.clear();
  }
  static bool HasReloadedNoticeBanner(const WorkspaceShell& shell,
                                      const std::filesystem::path& path) {
    const std::filesystem::path normalized = path.lexically_normal();
    for (const EditorBannerState& banner : shell.context_.current_project_state.editor_banners) {
      if (banner.kind == EditorBannerState::Kind::ReloadedNotice && banner.path == normalized) {
        return true;
      }
    }
    return false;
  }
  static std::size_t EditorBannerCount(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.editor_banners.size();
  }
  static void EditorBannerReload(WorkspaceShell& shell, const std::filesystem::path& path) {
    shell.ActivateEditorBannerAction(EditorBannerAction::Reload, path);
  }
  static void EditorBannerOverwrite(WorkspaceShell& shell, const std::filesystem::path& path) {
    shell.ActivateEditorBannerAction(EditorBannerAction::Overwrite, path);
  }
  static void EditorBannerCompare(WorkspaceShell& shell, const std::filesystem::path& path) {
    shell.ActivateEditorBannerAction(EditorBannerAction::Compare, path);
  }
  static void EditorBannerKeep(WorkspaceShell& shell, const std::filesystem::path& path) {
    shell.ActivateEditorBannerAction(EditorBannerAction::Keep, path);
  }
  static void EditorBannerRetry(WorkspaceShell& shell, const std::filesystem::path& path) {
    shell.ActivateEditorBannerAction(EditorBannerAction::Retry, path);
  }
  static const EditorBannerState* ActiveEditorBanner(const WorkspaceShell& shell) {
    return ActiveEditorBannerForTab(shell.context_.current_project_state);
  }
  static void OpenPromptSurfaceForTest(WorkspaceShell& shell,
                                       PromptSurfaceState::Action action,
                                       PromptSurfaceState::Kind kind,
                                       const std::filesystem::path& path,
                                       std::string detail, std::string input = {}) {
    shell.context_.prompts.surface_visible = true;
    shell.context_.prompts.surface.action = action;
    shell.context_.prompts.surface.kind = kind;
    shell.context_.prompts.surface.path = path;
    shell.context_.prompts.surface.detail = std::move(detail);
    shell.context_.prompts.surface.input.SetText(std::move(input));
    shell.context_.prompts.surface.button_count = 2;
    shell.context_.prompts.surface.selected_button = 0;
  }
  static std::size_t PromptSurfaceLabelCount(const WorkspaceShell& shell) {
    return shell.PromptSurfaceActionLabels().size();
  }
  static std::size_t PromptSurfaceButtonRectCount(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return ComputePromptSurfaceButtonRects(ComputePromptSurfaceRect(layout.full),
                                           shell.context_.prompts.surface.button_count)
        .size();
  }
  static void SetPromptSurfaceSelectedButton(WorkspaceShell& shell, int button) {
    shell.context_.prompts.surface.selected_button = button;
  }
  static void SetPromptSurfaceButtonCount(WorkspaceShell& shell, int count) {
    shell.context_.prompts.surface.button_count = count;
  }
  static void ShowDirtyPromptForTest(WorkspaceShell& shell, DirtyPromptState::Kind kind,
                                     std::size_t dirty_count) {
    shell.context_.prompts.dirty_visible = true;
    shell.context_.prompts.dirty.kind = kind;
    shell.context_.prompts.dirty.dirty_count = dirty_count;
    shell.context_.prompts.dirty.selected_action = 0;
  }
  static void SetDirtyPromptSelectedAction(WorkspaceShell& shell, int action) {
    shell.context_.prompts.dirty.selected_action = action;
  }
  static bool PromptSurfaceVisible(const WorkspaceShell& shell) {
    return shell.context_.prompts.surface_visible;
  }
  static SDL_FRect PromptSurfaceInputRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return ComputePromptSurfaceInputRect(ComputePromptSurfaceRect(layout.full));
  }
  static const std::string& PromptSurfaceInput(const WorkspaceShell& shell) {
    return shell.context_.prompts.surface.input.text();
  }
  static void SetPromptSurfaceInput(WorkspaceShell& shell, const std::string& text) {
    shell.context_.prompts.surface.input.SetText(text);
  }
  static std::size_t PromptSurfaceInputCaret(const WorkspaceShell& shell) {
    return shell.context_.prompts.surface.input.caret();
  }
  static std::optional<std::pair<std::size_t, std::size_t>> PromptSurfaceInputSelection(
      const WorkspaceShell& shell) {
    const auto selection = shell.context_.prompts.surface.input.Selection();
    if (!selection.has_value()) {
      return std::nullopt;
    }
    return std::make_pair(selection->start, selection->end);
  }
  static std::string PromptSurfaceTitle(const WorkspaceShell& shell) {
    return std::string(shell.PromptSurfaceTitle());
  }
  static std::string PromptSurfaceMessage(const WorkspaceShell& shell) {
    return shell.PromptSurfaceMessage();
  }
  static std::string PromptSurfaceDetail(const WorkspaceShell& shell) {
    return std::string(shell.PromptSurfaceDetail());
  }
  static const std::vector<WorkspaceShell::TabEntry>& OpenTabs(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.focused_group().open_tabs;
  }
  static const std::vector<ResolvedKeybinding>& ResolvedKeybindings(WorkspaceShell& shell) {
    return shell.ResolvedKeybindings();
  }
  static void SetDisabledKeybindingIds(WorkspaceShell& shell, std::vector<std::string> ids) {
    shell.context_.disabled_keybinding_ids = std::move(ids);
  }
  static std::size_t ActiveTabIndex(const WorkspaceShell& shell) { return shell.context_.current_project_state.focused_group().active_tab_index; }
  static const TextCompositionState& TextComposition(const WorkspaceShell& shell) {
    return shell.context_.text_input.composition;
  }
  static const TabDragState& TabDrag(const WorkspaceShell& shell) {
    return shell.context_.interaction_state.tab_drag;
  }
  static const TabSlideState& TabSlide(const WorkspaceShell& shell) {
    return shell.context_.interaction_state.tab_slide;
  }
  static bool ActiveTabIsCompare(const WorkspaceShell& shell) { return shell.ActiveTabIsCompare(); }
  static bool ActiveTabIsMerge(const WorkspaceShell& shell) { return shell.ActiveTabIsMerge(); }
  static std::size_t ActiveTerminalTabIndex(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.active_terminal_tab_index;
  }
  static int BottomPanelTabScrollIndex(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.panel.tab_scroll_index;
  }
  static std::vector<std::filesystem::path> ProjectRoots(const WorkspaceShell& shell) {
    std::vector<std::filesystem::path> roots;
    roots.reserve(shell.context_.project_catalog.entries.size());
    for (std::size_t i = 0; i < shell.context_.project_catalog.entries.size(); ++i) {
      roots.push_back(shell.ProjectCatalogRoot(i));
    }
    return roots;
  }
  static std::vector<std::string> TerminalLaunchLabels(WorkspaceShell& shell) {
    std::vector<std::string> labels;
    labels.reserve(shell.context_.current_project_state.terminal_tabs.size());
    for (const auto& terminal_tab : shell.context_.current_project_state.terminal_tabs) {
      const auto* pane = terminal_tab == nullptr ? nullptr : terminal_tab->active();
      labels.push_back(pane == nullptr ? std::string{} : pane->session.LaunchLabel());
    }
    return labels;
  }
  static std::vector<std::string> VisibleMenuBarLabels(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    std::vector<std::string> labels;
    for (const auto& item : shell.ComputeVisibleMenuBarItems(layout.menu_bar)) {
      if (const auto* menu = shell.FindMenuSpec(item.id); menu != nullptr) {
        labels.emplace_back(menu->label);
      }
    }
    return labels;
  }
  static std::vector<std::string> OverflowMenuBarLabels(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    std::vector<std::string> labels;
    for (const MenuId id : shell.ComputeOverflowMenuBarItems(layout.menu_bar)) {
      if (const auto* menu = shell.FindMenuSpec(id); menu != nullptr) {
        labels.emplace_back(menu->label);
      }
    }
    return labels;
  }
  static std::size_t LayOutMenuBar(WorkspaceShell& shell, const SDL_FRect& menu_bar) {
    return shell.ComputeVisibleMenuBarItems(menu_bar).size() +
           shell.ComputeOverflowMenuBarItems(menu_bar).size() +
           shell.ComputeVisibleWindowControlButtons(menu_bar).size();
  }
  static SDL_FRect MenuBarRect(WorkspaceShell& shell) { return CurrentLayout(shell).menu_bar; }
  static std::optional<SDL_FRect> MenuBarItemRect(WorkspaceShell& shell,
                                                  WorkspaceShell::MenuId id) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    for (const auto& item : shell.ComputeVisibleMenuBarItems(layout.menu_bar)) {
      if (item.id == id) {
        return item.rect;
      }
    }
    return std::nullopt;
  }
  static std::optional<SDL_FRect> HoveredInteractiveRect(WorkspaceShell& shell, float x, float y) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return shell.HoveredInteractiveRect(layout, x, y);
  }
  static std::optional<SDL_FRect> HoverTargetRect(WorkspaceShell& shell, float x, float y) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    if (const auto segment = shell.HoveredStatusBarSegmentRect(layout, x, y)) {
      return segment;
    }
    return shell.HoveredInteractiveRect(layout, x, y);
  }
  static std::optional<SDL_FRect> MenuOverflowChevronRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return shell.MenuOverflowChevronRect(layout.menu_bar);
  }
  static VisibleMenuBarItems MenuBarItemsForTest(WorkspaceShell& shell) {
    return shell.ComputeVisibleMenuBarItems(CurrentLayout(shell).menu_bar);
  }
  static MenuBarOverflowIds MenuBarOverflowIdsForTest(WorkspaceShell& shell) {
    return shell.ComputeOverflowMenuBarItems(CurrentLayout(shell).menu_bar);
  }
  static std::size_t MenuBarOverflowItemCount(WorkspaceShell& shell) {
    return shell.ComputeOverflowMenuBarItems(CurrentLayout(shell).menu_bar).size();
  }
  static void SetMousePositionForTest(WorkspaceShell& shell, float x, float y) {
    shell.last_mouse_x_ = x;
    shell.last_mouse_y_ = y;
    shell.last_mouse_position_valid_ = true;
  }
  static bool MenuOverflowPopupOpen(const WorkspaceShell& shell) {
    return shell.context_.menu_state.overflow_popup_open;
  }
  static int MenuOverflowPopupActiveIndex(const WorkspaceShell& shell) {
    return shell.context_.menu_state.overflow_popup_active_index;
  }
  static std::optional<SDL_FRect> MenuOverflowPopupRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    const auto chevron = shell.MenuOverflowChevronRect(layout.menu_bar);
    if (!shell.context_.menu_state.overflow_popup_open || !chevron.has_value()) {
      return std::nullopt;
    }
    return ComputeMenuOverflowPopupRect(*chevron, shell.ComputeOverflowMenuBarItems(layout.menu_bar).size());
  }
  static std::optional<SDL_FRect> MenuBarItemRect(WorkspaceShell& shell, std::string_view label) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    for (const auto& item : shell.ComputeVisibleMenuBarItems(layout.menu_bar)) {
      if (const auto* menu = shell.FindMenuSpec(item.id);
          menu != nullptr && menu->label == label) {
        return item.rect;
      }
    }
    return std::nullopt;
  }
  static std::vector<std::string> VisiblePopupMenuLabels(WorkspaceShell& shell,
                                                         WorkspaceShell::MenuId id) {
    std::vector<std::string> labels;
    const WorkspaceLayout layout = CurrentLayout(shell);
    const auto popup_rect = shell.ComputePopupMenuRect(layout.menu_bar, id);
    if (!popup_rect.has_value()) {
      return labels;
    }
    const auto items = shell.MenuItems(id);
    for (const auto& visible_item : shell.ComputeVisiblePopupMenuItems(id, *popup_rect)) {
      if (visible_item.separator) {
        continue;
      }
      labels.emplace_back(shell.MenuItemLabel(items[visible_item.index], false));
    }
    return labels;
  }
  static std::optional<SDL_FRect> PopupMenuItemRect(WorkspaceShell& shell,
                                                    WorkspaceShell::MenuId id,
                                                    std::string_view label) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    const auto popup_rect = shell.ComputePopupMenuRect(layout.menu_bar, id);
    if (!popup_rect.has_value()) {
      return std::nullopt;
    }
    const auto items = shell.MenuItems(id);
    for (const auto& visible_item : shell.ComputeVisiblePopupMenuItems(id, *popup_rect)) {
      if (visible_item.separator) {
        continue;
      }
      if (shell.MenuItemLabel(items[visible_item.index], false) == label) {
        return visible_item.rect;
      }
    }
    return std::nullopt;
  }
  static std::optional<bool> MenuItemCheckedByLabel(const WorkspaceShell& shell,
                                                    WorkspaceShell::MenuId id,
                                                    std::string_view label) {
    const auto items = shell.MenuItems(id);
    for (const auto& item : items) {
      if (item.separator) {
        continue;
      }
      if (shell.MenuItemLabel(item, false) == label) {
        return shell.IsMenuItemChecked(item);
      }
    }
    return std::nullopt;
  }
  static std::string PaintedMenuItemLabel(const WorkspaceShell& shell, WorkspaceShell::MenuId id,
                                          std::string_view label) {
    for (const auto& item : shell.MenuItems(id)) {
      if (!item.separator && shell.MenuItemLabel(item, false) == label) {
        return std::string(shell.MenuItemLabel(item));
      }
    }
    return {};
  }
  static std::vector<std::string> MenuItemLabels(const WorkspaceShell& shell,
                                                 WorkspaceShell::MenuId id) {
    std::vector<std::string> labels;
    for (const auto& item : shell.MenuItems(id)) {
      if (item.separator) {
        continue;
      }
      labels.emplace_back(shell.MenuItemLabel(item, false));
    }
    return labels;
  }
  static std::vector<std::string> SidebarModeMenuLabels(WorkspaceShell& shell) {
    return VisiblePopupMenuLabels(shell, WorkspaceShell::MenuId::SidebarMode);
  }
  static std::optional<SDL_FRect> SidebarModeMenuItemRect(WorkspaceShell& shell,
                                                          std::string_view label) {
    return PopupMenuItemRect(shell, WorkspaceShell::MenuId::SidebarMode, label);
  }
  static SDL_FRect SidebarModeButtonRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    const SidebarModeRowLayout row = shell.SidebarModeRow(layout.sidebar);
    return row.tab_count > 0 ? row.tabs[0].rect : SDL_FRect{};
  }
  static SDL_FRect SidebarModeTabRect(WorkspaceShell& shell, std::string_view view_id) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    const SidebarModeRowLayout row = shell.SidebarModeRow(layout.sidebar);
    for (int i = 0; i < row.tab_count; ++i) {
      if (row.tabs[static_cast<std::size_t>(i)].id == view_id) {
        return row.tabs[static_cast<std::size_t>(i)].rect;
      }
    }
    return SDL_FRect{};
  }
  static SDL_FRect SidebarModeOverflowRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    const SidebarModeRowLayout row = shell.SidebarModeRow(layout.sidebar);
    return row.has_overflow ? row.overflow_rect : SDL_FRect{};
  }
  static SDL_FRect SearchSidebarModeButtonRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return project_search_panel::ModeButtonRect(layout.sidebar);
  }
  static SDL_FRect SearchSidebarCaseButtonRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return project_search_panel::CaseButtonRect(layout.sidebar);
  }
  static SDL_FRect SearchSidebarScopeButtonRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return project_search_panel::ScopeButtonRect(layout.sidebar);
  }
  static SDL_FRect SearchSidebarIncludeFieldRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return project_search_panel::IncludeRect(
        layout.sidebar,
        shell.context_.current_project_state.overlay.workflow.project_search.scope_expanded);
  }
  static SDL_FRect SearchSidebarExcludeFieldRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return project_search_panel::ExcludeRect(
        layout.sidebar,
        shell.context_.current_project_state.overlay.workflow.project_search.scope_expanded);
  }
  static bool ProjectSearchScopeExpanded(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.overlay.workflow.project_search.scope_expanded;
  }
  static void SetProjectSearchScopeGlobs(WorkspaceShell& shell, std::string include,
                                         std::string exclude) {
    auto& search = shell.context_.current_project_state.overlay.workflow.project_search;
    search.include_globs.SetText(std::move(include));
    search.exclude_globs.SetText(std::move(exclude));
  }
  static ProjectSearchEditField ProjectSearchEditFieldValue(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.overlay.workflow.project_search.edit_field;
  }
  static bool ProjectSearchEditing(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.overlay.workflow.project_search.editing;
  }
  static const project::ProjectSearchOptions& ProjectSearchOptions(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.overlay.workflow.project_search.options;
  }
  static SDL_FRect TreeSidebarCollapseButtonRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return shell.TreeSidebarCollapseButtonRect(layout.sidebar);
  }
  static SDL_FRect TreeSidebarRefreshButtonRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return shell.TreeSidebarRefreshButtonRect(layout.sidebar);
  }
  static std::string StatusBarSegmentText(const WorkspaceShell& shell,
                                          StatusBarSegmentId id) {
    return shell.status_bar_service_.Segment(id).text;
  }
  static std::string StatusBarSegmentTooltip(const WorkspaceShell& shell,
                                             StatusBarSegmentId id) {
    return shell.status_bar_service_.Segment(id).tooltip;
  }
  static bool StatusBarSegmentVisible(const WorkspaceShell& shell,
                                      StatusBarSegmentId id) {
    return shell.status_bar_service_.Segment(id).visible;
  }
  static std::optional<SDL_FRect> StatusBarSegmentRect(WorkspaceShell& shell,
                                                       StatusBarSegmentId id) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    const StatusBarViewModel vm =
        RenderViewModelBuilder(shell.context_).BuildStatusBar(layout, shell.status_bar_service_);
    std::optional<SDL_FRect> found;
    ForEachStatusBarSegmentRect(
        vm, shell.text_renderer_,
        [&](const StatusBarSegmentViewModel& segment, const SDL_FRect& row) {
          if (!found.has_value() && segment.id == id) {
            found = row;
          }
        });
    return found;
  }
  static void SetStatusBarSegment(WorkspaceShell& shell, StatusBarSegmentId id, StatusBarSegmentValue value) {
    (void)shell.status_bar_service_.SetSegment(id, std::move(value));
  }
  static void RefreshStatusBar(WorkspaceShell& shell) { shell.RefreshStatusBar(); }
  static void SetStatusBarGitSnapshot(WorkspaceShell& shell,
                                      bool repo_available,
                                      std::string base_label,
                                      bool dirty_worktree) {
    auto& git = shell.context_.current_project_state.sidebar.git;
    git.repo_available = repo_available;
    git.branch_label = base_label;
    git.base_label = std::move(base_label);
    git.base_ref = git.base_label;
    git.entries.clear();
    if (repo_available && dirty_worktree) {
      GitSidebarEntry entry;
      entry.section = GitSidebarEntry::Section::Changed;
      entry.path = shell.context_.current_project_state.root / "status_bar_fixture.cpp";
      entry.relative_path = std::filesystem::path("status_bar_fixture.cpp");
      entry.status = project::GitFileStatus::Modified;
      git.entries.push_back(std::move(entry));
    }
  }
  static std::string BreadcrumbLabel(WorkspaceShell& shell) { return shell.BreadcrumbLabel(); }
  static std::string BreadcrumbLabelForGroup(WorkspaceShell& shell, std::size_t group_index) {
    return shell.BreadcrumbLabel(group_index);
  }
  static const std::vector<project::TreeEntry>& TreeEntries(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.directory_tree.entries();
  }
  static std::optional<SDL_FRect> TreeSidebarRowRect(WorkspaceShell& shell, std::size_t entry_index) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    const ScrollableListLayout list = shell.ComputeTreeSidebarListLayout(
        layout.sidebar, shell.context_.current_project_state.directory_tree.entries().size());
    const int visible_row = static_cast<int>(entry_index) - list.scroll_row;
    if (visible_row < 0 || visible_row >= list.visible_rows) {
      return std::nullopt;
    }
    return ScrollableListRowRect(list, visible_row);
  }
  static void ForceCursorReassert(WorkspaceShell& shell) { shell.ForceCursorReassert(); }
  static std::filesystem::path SelectedTreePath(const WorkspaceShell& shell) {
    return shell.SelectedTreePath();
  }
  static int SidebarScrollRow(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.sidebar.scroll_row;
  }
  static void SetSidebarScrollRow(WorkspaceShell& shell, int scroll_row) {
    shell.context_.current_project_state.sidebar.scroll_row = scroll_row;
  }
  static std::size_t ProjectCount(const WorkspaceShell& shell) { return shell.context_.project_catalog.entries.size(); }
  static std::size_t ActiveProjectIndex(const WorkspaceShell& shell) {
    return shell.context_.project_catalog.active_index;
  }
  static const std::filesystem::path& ProjectRoot(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.root;
  }
  static const std::vector<project::ProjectSearchResult>& ProjectSearchResults(
      const WorkspaceShell& shell) {
    return shell.context_.current_project_state.overlay.workflow.project_search.results;
  }
  static bool ProjectSearchRunning(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.overlay.workflow.project_search.running;
  }
  static std::vector<int> ProjectSearchLineMap(const WorkspaceShell& shell) {
    return shell.BuildProjectSearchLineMap();
  }
  static std::uint64_t ProjectSearchResultsRevision(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.overlay.workflow.project_search.results_revision;
  }
  static bool ProjectSearchWorkerFinished(const WorkspaceShell& shell) {
    return shell.project_search_runtime_.WorkerFinished();
  }
  static bool GitSidebarRefreshing(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.sidebar.git.refreshing;
  }
  static bool GitSidebarSnapshotStale(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.sidebar.git.snapshot_stale;
  }
  static bool ProjectSearchTruncated(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.overlay.workflow.project_search.truncated;
  }
  static bool ProjectSearchIndexIncomplete(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.overlay.workflow.project_search.index_incomplete;
  }
  static std::size_t ProjectSearchTotalFiles(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.overlay.workflow.project_search.total_files;
  }
  static bool FileIndexScanIncomplete(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.file_index.truncated();
  }
  static const std::string& ProjectSearchError(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.overlay.workflow.project_search.error;
  }
  static bool ProjectOpenDialogActive(const WorkspaceShell& shell) {
    return shell.project_dialog_state_.active;
  }
  static bool CommandPaletteOpen(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.overlay.visible &&
           shell.context_.current_project_state.overlay.mode == OverlayMode::CommandPalette;
  }
  static const std::string& CommandPaletteQuery(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.overlay.workflow.command_palette.query.text();
  }
  static const std::string& CommandFeedbackText(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.panel.feedback.text;
  }
  static const std::string& ProjectSearchQuery(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.overlay.workflow.project_search.query.text();
  }
  static const std::string& BufferSearchQuery(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.overlay.workflow.buffer_search.query.text();
  }
  static std::size_t BufferSearchMatchCount(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.overlay.workflow.buffer_search.matches.size();
  }
  static void SetBufferSearchQueryAndRefresh(WorkspaceShell& shell, std::string query) {
    shell.context_.current_project_state.overlay.workflow.buffer_search.query.SetText(
        std::move(query));
    shell.RefreshBufferSearch();
  }
  static std::size_t BufferSearchSelectedIndex(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.overlay.workflow.buffer_search.selected_index;
  }
  static void SetBufferSearchSelectedIndex(WorkspaceShell& shell, std::size_t index) {
    shell.context_.current_project_state.overlay.workflow.buffer_search.selected_index = index;
  }
  static bool BufferSearchRegexEnabled(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.overlay.workflow.buffer_search.regex;
  }
  static void SetBufferSearchRegexAndRefresh(WorkspaceShell& shell, bool enabled) {
    shell.context_.current_project_state.overlay.workflow.buffer_search.regex = enabled;
    shell.RefreshBufferSearch();
  }
  static void ToggleBufferSearchOption(WorkspaceShell& shell, BufferFindToggle toggle) {
    shell.ToggleBufferSearchOption(toggle);
  }
  static bool BufferSearchMatchCase(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.overlay.workflow.buffer_search.match_case;
  }
  static bool BufferSearchWholeWord(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.overlay.workflow.buffer_search.whole_word;
  }
  static void SetBufferReplaceText(WorkspaceShell& shell, std::string text) {
    shell.context_.current_project_state.overlay.workflow.buffer_search.replace_text.SetText(
        std::move(text));
  }
  static void ReplaceCurrentBufferSearchMatch(WorkspaceShell& shell) {
    shell.ReplaceCurrentBufferSearchMatch();
  }
  static void RefreshBufferSearchIfStale(WorkspaceShell& shell) { shell.RefreshBufferSearchIfStale(); }
  static void ReplaceAllBufferSearchMatches(WorkspaceShell& shell) {
    shell.ReplaceAllBufferSearchMatches();
  }
  static std::size_t ProjectSearchSelectedIndex(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.overlay.workflow.project_search.selected_index;
  }
  static void SetProjectSearchSelectedIndex(WorkspaceShell& shell, std::size_t index) {
    shell.context_.current_project_state.overlay.workflow.project_search.selected_index = index;
  }
  static void SelectAllBufferSearchQuery(WorkspaceShell& shell) {
    shell.context_.current_project_state.overlay.workflow.buffer_search.query.SelectAll();
  }
  static bool BufferSearchSurfaceFocused(const WorkspaceShell& shell) {
    return shell.CurrentTextInputSurface() == WorkspaceShell::TextInputSurface::BufferSearch;
  }
  static FindWidgetLayout FindWidgetControls(WorkspaceShell& shell, bool replace_mode) {
    return ComputeBufferFindWidgetLayout(shell.ActiveTabSurface(CurrentLayout(shell)), replace_mode);
  }
  static SDL_FRect EditorAreaRect(WorkspaceShell& shell) {
    return CurrentLayout(shell).editor_area;
  }
  static bool OverlayVisible(const WorkspaceShell& shell) { return shell.context_.current_project_state.overlay.visible; }
  static bool OverlayModeIsFileFinder(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.overlay.mode == WorkspaceShell::OverlayMode::FileFinder;
  }
  static bool TextInputSurfaceIsEditor(const WorkspaceShell& shell) {
    return shell.CurrentTextInputSurface() == WorkspaceShell::TextInputSurface::Editor;
  }
  static bool TextInputSurfaceIsFileFinder(const WorkspaceShell& shell) {
    return shell.CurrentTextInputSurface() == WorkspaceShell::TextInputSurface::FileFinder;
  }
  static std::size_t FileFinderResultCount(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.file_finder.results().size();
  }
  static std::optional<std::filesystem::path> FileFinderSelectedPath(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.file_finder.SelectedPath();
  }
  static bool FileIndexContainsPath(const WorkspaceShell& shell,
                                    const std::filesystem::path& relative_path) {
    const auto files = shell.context_.current_project_state.file_index.Snapshot();
    const std::filesystem::path normalized = relative_path.lexically_normal();
    for (const auto& file : files) {
      if (file.relative_path == normalized) {
        return true;
      }
    }
    return false;
  }
  static std::size_t FileIndexSize(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.file_index.Snapshot().size();
  }
  static bool TextInputSurfaceIsPromptInput(const WorkspaceShell& shell) {
    return shell.CurrentTextInputSurface() == WorkspaceShell::TextInputSurface::PromptInput;
  }
  static bool SidebarVisible(const WorkspaceShell& shell) { return shell.context_.current_project_state.sidebar.visible; }
  static WorkspaceShell::SidebarMode SidebarMode(const WorkspaceShell& shell) {
    return shell.ActiveSidebarMode();
  }
  static const std::string& SidebarViewId(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.sidebar.view_id;
  }  static const std::vector<plugin::PluginHost::SidebarItem>& PluginSidebarItems(
      const WorkspaceShell& shell) {
    return shell.context_.current_project_state.sidebar.plugin.items;
  }
  static const std::string& PluginSidebarError(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.sidebar.plugin.error;
  }
  static const std::string& PluginSidebarPlaceholder(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.sidebar.plugin.placeholder;
  }
  static float SidebarWidth(const WorkspaceShell& shell) { return shell.context_.current_project_state.sidebar.width; }
  static void SetSidebarWidth(WorkspaceShell& shell, float width) {
    shell.context_.current_project_state.sidebar.width = width;
  }
  static float BottomPanelHeight(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.panel.height;
  }
  static void SetBottomPanelHeight(WorkspaceShell& shell, float height) {
    shell.context_.current_project_state.panel.height = height;
  }
  static bool SoftTabsEnabled(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.editor_preferences.soft_tabs;
  }
  static int OverlayScrollRow(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.overlay.scroll_row;
  }
  static std::size_t OverlaySelectedIndex(const WorkspaceShell& shell) {
    return shell.OverlaySelectedIndex();
  }
  static std::size_t CommandPaletteMatchCount(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.overlay.workflow.command_palette.matches.size();
  }
  static std::size_t CommandPaletteItemCount(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.overlay.workflow.command_palette.items.size();
  }
  static std::string CommandPaletteMatchLabelAt(const WorkspaceShell& shell, std::size_t i) {
    const auto& palette = shell.context_.current_project_state.overlay.workflow.command_palette;
    if (i >= palette.matches.size() || palette.matches[i] >= palette.items.size()) {
      return {};
    }
    return palette.items[palette.matches[i]].primary_label;
  }
  static std::string CommandPaletteItemLabelAt(const WorkspaceShell& shell, std::size_t i) {
    const auto& palette = shell.context_.current_project_state.overlay.workflow.command_palette;
    return i < palette.items.size() ? palette.items[i].primary_label : std::string{};
  }
  static void SetCommandPaletteQueryAndRefresh(WorkspaceShell& shell, std::string_view text) {
    auto& palette = shell.context_.current_project_state.overlay.workflow.command_palette;
    palette.query.SetText(std::string(text));
    shell.RefreshCommandPalette();
  }
  static std::size_t EditorTabSize(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.editor_preferences.tab_size;
  }
  static std::size_t EditorIndentWidth(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.editor_preferences.indent_width;
  }
  static int EditorFontSize(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.editor_preferences.font_size;
  }
  static bool SoftWrapEnabled(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.editor_preferences.soft_wrap;
  }
  static bool SettingsOverlayVisible(const WorkspaceShell& shell) {
    return shell.settings_overlay_service_.Visible();
  }
  static bool SetSettingValue(WorkspaceShell& shell, std::string_view id, std::string value) {
    return shell.SetSettingValue(id, std::move(value));
  }
  static void ToggleSettingScope(WorkspaceShell& shell, std::string_view id) {
    shell.ToggleSettingScope(id);
  }
  static bool SetSettingValueTransient(WorkspaceShell& shell, std::string_view id,
                                       std::string value) {
    return shell.SetSettingValue(id, std::move(value), false);
  }
  static bool IsSettingTransient(const WorkspaceShell& shell, std::string_view id) {
    return shell.context_.transient_setting_keys.count(std::string(id)) != 0;
  }
  static void SetRawUserSetting(WorkspaceShell& shell, std::string_view id, std::string value) {
    settings_layer::Upsert(shell.context_.user_settings, id, std::move(value));
    shell.settings_store_.Reindex();
  }
  static std::optional<std::string> GetSettingValue(const WorkspaceShell& shell,
                                                    std::string_view id) {
    return shell.GetSettingValue(id);
  }
  static void SaveUserConfig(WorkspaceShell& shell) {
    shell.MakePersistenceCoordinator().SaveUserConfig();
    FlushPendingStateWrites(shell);
  }
  static void ApplyStartupSettingOverrides(WorkspaceShell& shell) {
    shell.ApplyStartupSettingOverrides();
  }
  static std::optional<std::string> ProjectStoredSettingValue(const WorkspaceShell& shell,
                                                              std::string_view id) {
    const auto& settings = shell.context_.current_project_state.settings;
    const auto it = std::find_if(settings.begin(), settings.end(),
                                 [id](const auto& entry) { return entry.first == id; });
    if (it == settings.end()) {
      return std::nullopt;
    }
    return it->second;
  }
  static void OpenSettingsOverlay(WorkspaceShell& shell) { shell.OpenSettingsOverlay(); }
  static void BeginSettingValueEdit(WorkspaceShell& shell, std::string_view id) {
    shell.BeginSettingValueEdit(id);
  }
  static bool SettingsEditingFonts(const WorkspaceShell& shell) {
    return shell.settings_overlay_service_.EditingFonts();
  }
  static SettingsOverlayViewModel BuildSettingsOverlayViewModel(WorkspaceShell& shell) {
    const auto layout = shell.CurrentWorkspaceLayout();
    if (!layout.has_value()) {
      return {};
    }
    return RenderViewModelBuilder(shell.context_)
        .BuildSettingsOverlay(*layout, shell.settings_overlay_service_, shell.text_renderer_);
  }
  static const editor::SingleLineEditor& PromptSurfaceInputEditor(const WorkspaceShell& shell) {
    return shell.context_.prompts.surface.input;
  }
  static void StepSetting(WorkspaceShell& shell, std::string_view id, bool forward) {
    shell.StepSetting(id, forward);
  }
  static void OpenHelpAboutOverlay(WorkspaceShell& shell) { shell.OpenHelpAboutOverlay(); }
  static std::vector<HelpAboutRow> HelpAboutRows(const WorkspaceShell& shell) {
    return shell.settings_overlay_service_.HelpRows();
  }
  static SDL_FRect SettingsOverlayRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return ComputeSettingsOverlaySurfaceRect(layout.overlay_anchor, layout.full);
  }
  static SDL_FRect ActiveOverlayRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return shell.ComputeOverlayRect(layout.overlay_anchor);
  }
  static std::optional<SDL_FRect> CaretDirtyRect(const WorkspaceShell& shell) {
    return shell.CurrentCaretDirtyRect();
  }
  static const std::optional<SDL_FRect>& OverlayCaretAnchor(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.overlay.caret_anchor;
  }
  static SDL_FRect CodeActionMenuRectForItemCount(WorkspaceShell& shell, std::size_t item_count) {
    auto& overlay = shell.context_.current_project_state.overlay;
    overlay.visible = true;
    overlay.mode = WorkspaceShell::OverlayMode::CodeActions;
    overlay.caret_anchor.reset();
    overlay.workflow.code_actions.items.clear();
    overlay.workflow.code_actions.items.resize(item_count);
    overlay.workflow.code_actions.selected_index = 0;
    const WorkspaceLayout layout = CurrentLayout(shell);
    return shell.ComputeOverlayRect(layout.overlay_anchor);
  }
  static int SettingsOverlayScrollRow(const WorkspaceShell& shell) {
    return shell.settings_overlay_service_.ScrollRow();
  }
  static void SetSettingsOverlayCategory(WorkspaceShell& shell, int category) {
    shell.settings_overlay_service_.SetSelectedCategory(category);
  }
  static void SetSettingsOverlaySelectedRow(WorkspaceShell& shell, int row) {
    shell.settings_overlay_service_.SetSelectedRow(row);
  }
  static void SetSettingsOverlayScrollRow(WorkspaceShell& shell, int row) {
    shell.settings_overlay_service_.SetScrollRow(row);
  }
  static std::size_t SettingsOverlayRowCountInSelectedCategory(const WorkspaceShell& shell) {
    return shell.settings_overlay_service_.RowCountInSelectedCategory();
  }
  static int SettingsOverlayMaxScroll(WorkspaceShell& shell) {
    const auto layout = shell.CurrentWorkspaceLayout();
    if (!layout.has_value()) {
      return 0;
    }
    return RenderViewModelBuilder(shell.context_)
        .BuildSettingsOverlay(*layout, shell.settings_overlay_service_, shell.text_renderer_)
        .max_scroll;
  }
  static std::optional<ScrollbarGeometry> SettingsOverlayScrollbar(WorkspaceShell& shell) {
    const auto layout = shell.CurrentWorkspaceLayout();
    if (!layout.has_value()) {
      return std::nullopt;
    }
    return RenderViewModelBuilder(shell.context_)
        .BuildSettingsOverlay(*layout, shell.settings_overlay_service_, shell.text_renderer_)
        .scrollbar;
  }
  static void EnsureSettingsSelectionVisible(WorkspaceShell& shell) {
    shell.EnsureSettingsSelectionVisible();
  }
  static bool SettingsOverlaySelectedRowFullyVisible(WorkspaceShell& shell) {
    const auto layout = shell.CurrentWorkspaceLayout();
    if (!layout.has_value()) {
      return false;
    }
    const SettingsOverlayViewModel vm =
        RenderViewModelBuilder(shell.context_)
            .BuildSettingsOverlay(*layout, shell.settings_overlay_service_, shell.text_renderer_);
    const int selected = shell.settings_overlay_service_.SelectedRow();
    const float pane_bottom = vm.right_pane_rect.y + vm.right_pane_rect.h;
    for (const SettingsRowViewModel& row : vm.rows) {
      if (row.row_in_category == selected) {
        return row.row_rect.y >= vm.right_pane_rect.y - 0.5f &&
               row.row_rect.y + row.row_rect.h <= pane_bottom + 0.5f;
      }
    }
    return false;
  }
  static SDL_FRect SettingsOverlayVisibleRowRect(WorkspaceShell& shell, std::size_t index) {
    const auto layout = shell.CurrentWorkspaceLayout();
    const SettingsOverlayViewModel vm =
        RenderViewModelBuilder(shell.context_)
            .BuildSettingsOverlay(*layout, shell.settings_overlay_service_, shell.text_renderer_);
    return index < vm.rows.size() ? vm.rows[index].row_rect : SDL_FRect{};
  }
  static std::pair<SDL_FRect, float> SettingsOverlayFilterRectAndTextWidth(WorkspaceShell& shell) {
    const auto layout = shell.CurrentWorkspaceLayout();
    const SettingsOverlayViewModel vm =
        RenderViewModelBuilder(shell.context_)
            .BuildSettingsOverlay(*layout, shell.settings_overlay_service_, shell.text_renderer_);
    return {vm.filter_rect, shell.text_renderer_.MeasureWidth(vm.query_display())};
  }
  static int SettingsOverlaySelectedRowIndex(const WorkspaceShell& shell) {
    return shell.settings_overlay_service_.SelectedRow();
  }
  static void BeginSettingsFontValueEdit(WorkspaceShell& shell, std::string row_id,
                                         std::vector<std::string> families) {
    shell.settings_overlay_service_.BeginFontValueEdit(std::move(row_id), std::move(families));
  }
  static int SettingsOverlayPickerScroll(const WorkspaceShell& shell) {
    return shell.settings_overlay_service_.PickerScroll();
  }
  static std::optional<ScrollbarGeometry> SettingsOverlayPickerScrollbar(WorkspaceShell& shell) {
    const auto layout = shell.CurrentWorkspaceLayout();
    if (!layout.has_value()) {
      return std::nullopt;
    }
    return RenderViewModelBuilder(shell.context_)
        .BuildSettingsOverlay(*layout, shell.settings_overlay_service_, shell.text_renderer_)
        .value_picker.scrollbar;
  }
  static void SetSettingsOverlayQueryAndRefresh(WorkspaceShell& shell, std::string query) {
    shell.settings_overlay_service_.SetQuery(std::move(query));
    shell.RefreshSettingsOverlayCatalog();
  }
  static void SetTransientDragTargetSidebarDivider(WorkspaceShell& shell) {
    shell.context_.interaction_state.drag_target = WorkspaceShell::DragTarget::SidebarDivider;
  }
  static void SetTransientDragTargetBottomPanelScrollbar(WorkspaceShell& shell) {
    shell.context_.interaction_state.drag_target = WorkspaceShell::DragTarget::BottomPanelScrollbar;
  }
  static void SetTransientDragTarget(WorkspaceShell& shell, DragTarget target) {
    shell.context_.interaction_state.drag_target = target;
  }
  static float TransientDragScrollbarOffset(const WorkspaceShell& shell) {
    return shell.context_.interaction_state.drag_scrollbar_offset;
  }
  static void SetTransientDragScrollbarOffset(WorkspaceShell& shell, float offset) {
    shell.context_.interaction_state.drag_scrollbar_offset = offset;
  }
  static bool HandleMouseButtonUp(WorkspaceShell& shell, const SDL_Event& event) {
    return shell.HandleMouseButtonUp(event);
  }
  static bool TransientDragTargetIsNone(const WorkspaceShell& shell) {
    return shell.context_.interaction_state.drag_target == WorkspaceShell::DragTarget::None;
  }
  static void SetTransientMouseSelecting(WorkspaceShell& shell, bool selecting) {
    shell.context_.interaction_state.mouse_selecting = selecting;
  }
  static bool TransientMouseSelecting(const WorkspaceShell& shell) {
    return shell.context_.interaction_state.mouse_selecting;
  }
  static void SetTransientTextDragging(WorkspaceShell& shell) {
    shell.context_.interaction_state.text_drag = InteractionState::TextDragState::Dragging;
    shell.context_.interaction_state.text_drag_has_drop = true;
  }
  static void ArmTextDrag(WorkspaceShell& shell, editor::SelectionRange source,
                          editor::TextPosition drop) {
    auto& interaction = shell.context_.interaction_state;
    const editor::TextViewport* viewport = shell.ActiveEditorViewport();
    interaction.text_drag = InteractionState::TextDragState::Dragging;
    interaction.text_drag_has_drop = true;
    interaction.text_drag_source_start_line = source.start.line;
    interaction.text_drag_source_start_column = source.start.column;
    interaction.text_drag_source_end_line = source.end.line;
    interaction.text_drag_source_end_column = source.end.column;
    interaction.text_drag_drop_line = drop.line;
    interaction.text_drag_drop_column = drop.column;
    interaction.text_drag_viewport = viewport;
    interaction.text_drag_content_revision =
        viewport != nullptr ? viewport->content_revision() : 0;
  }
  static bool FinishTextDrag(WorkspaceShell& shell, bool copy) {
    return shell.MakeEditorMouseCoordinator().FinishTextDrag(copy);
  }
  static void SetEditorGestureViewportToActive(WorkspaceShell& shell) {
    shell.context_.interaction_state.editor_gesture_viewport = shell.ActiveEditorViewport();
  }
  static InteractionState& InteractionState_(WorkspaceShell& shell) {
    return shell.context_.interaction_state;
  }
  static const render::Theme& PaintTheme(const WorkspaceShell& shell) { return shell.theme_; }
  static bool TransientTextDragIsIdle(const WorkspaceShell& shell) {
    return shell.context_.interaction_state.text_drag == InteractionState::TextDragState::None &&
           !shell.context_.interaction_state.text_drag_has_drop;
  }
  static void SetTransientBoxSelecting(WorkspaceShell& shell) {
    shell.context_.interaction_state.editor_box_selecting = true;
  }
  static bool TransientBoxSelecting(const WorkspaceShell& shell) {
    return shell.context_.interaction_state.editor_box_selecting;
  }
  static bool FocusIsEditor(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.surface.focus == WorkspaceShell::FocusTarget::Editor;
  }
  static bool FocusIsSidebar(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.surface.focus == WorkspaceShell::FocusTarget::Sidebar;
  }
  static bool FocusIsPanel(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.surface.focus == WorkspaceShell::FocusTarget::Panel;
  }
  static void SetFocusSidebar(WorkspaceShell& shell) {
    shell.context_.current_project_state.surface.focus = WorkspaceShell::FocusTarget::Sidebar;
  }
  static void SetFocusEditor(WorkspaceShell& shell) {
    shell.context_.current_project_state.surface.focus = WorkspaceShell::FocusTarget::Editor;
  }
  static void InvalidateEditorBlamePath(WorkspaceShell& shell, const std::filesystem::path& path) {
    shell.InvalidateEditorBlamePath(path);
  }
  static void SetPanelContentTerminal(WorkspaceShell& shell) {
    shell.context_.current_project_state.panel.content = WorkspaceShell::PanelContentKind::Terminal;
  }
  static void SetFocusPanel(WorkspaceShell& shell) {
    shell.context_.current_project_state.surface.focus = WorkspaceShell::FocusTarget::Panel;
  }
  static void ClearPluginSurface(WorkspaceShell& shell, std::string_view owner, std::string_view surface_id) {
    auto& state = shell.context_.current_project_state;
    if (auto* pres = state.plugin_presentation.get(); pres != nullptr && pres->surfaces.ClearOwnerSurface(owner, surface_id)) {
      shell.SyncPluginSurfacePreviewClosed();
    }
  }
  static void SetFocusDebugPane(WorkspaceShell& shell) {
    shell.context_.current_project_state.surface.focus = WorkspaceShell::FocusTarget::DebugPane;
  }
  static bool FocusIsDebugPane(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.surface.focus ==
           WorkspaceShell::FocusTarget::DebugPane;
  }
  static void SetDebugPaneVisible(WorkspaceShell& shell, bool visible) {
    shell.context_.current_project_state.debug_pane.visible = visible;
    if (visible && shell.context_.current_project_state.debug_pane.width <= 0.0f) {
      shell.context_.current_project_state.debug_pane.width = kWorkspaceDefaultRightPaneWidth;
    }
  }
  static void FocusDebugPaneWatch(WorkspaceShell& shell) {
    SetDebugPaneVisible(shell, true);
    shell.context_.current_project_state.debug_pane.mode = DebugPaneMode::Watch;
    shell.context_.current_project_state.surface.focus = WorkspaceShell::FocusTarget::DebugPane;
  }
  static void FocusDebugPaneCallStack(WorkspaceShell& shell) {
    SetDebugPaneVisible(shell, true);
    shell.context_.current_project_state.debug_pane.mode = DebugPaneMode::CallStack;
    shell.context_.current_project_state.surface.focus = WorkspaceShell::FocusTarget::DebugPane;
  }
  static void FocusDebugPaneBreakpoints(WorkspaceShell& shell) {
    SetDebugPaneVisible(shell, true);
    shell.context_.current_project_state.debug_pane.mode = DebugPaneMode::Breakpoints;
    shell.context_.current_project_state.surface.focus = WorkspaceShell::FocusTarget::DebugPane;
  }
  static void ToggleBreakpointForTest(WorkspaceShell& shell, const std::filesystem::path& path,
                                      std::size_t line) {
    shell.context_.current_project_state.breakpoint_store.Toggle(path, line);
    shell.context_.current_project_state.debug_breakpoints_panel.Rebuild(
        shell.context_.current_project_state.breakpoint_store,
        shell.context_.current_project_state.function_breakpoint_store);
  }
  static std::size_t DebugBreakpointRowCount(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.debug_breakpoints_panel.RowCount();
  }
  static int DebugBreakpointsSelectedRow(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.debug_pane.breakpoints_selected_row;
  }
  static bool BreakpointEnabledForTest(const WorkspaceShell& shell,
                                       const std::filesystem::path& path, std::size_t line) {
    const editor::Breakpoint* breakpoint =
        shell.context_.current_project_state.breakpoint_store.FindAt(path, line);
    return breakpoint != nullptr && breakpoint->enabled;
  }
  static DebugExecutionView& DebugExecution(WorkspaceShell& shell) {
    return shell.context_.current_project_state.debug_execution;
  }
  static int DebugPaneCallStackScrollRow(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.debug_pane.call_stack_scroll_row;
  }
  static bool LastPaintedDebugPaneWasVisible(const WorkspaceShell& shell) {
    return shell.prepare_cached_debug_pane_vm_.has_value() &&
           shell.prepare_cached_debug_pane_vm_->visible;
  }
  static void SetDebugPaneMode(WorkspaceShell& shell, DebugPaneMode mode) {
    SetDebugPaneVisible(shell, true);
    shell.context_.current_project_state.debug_pane.mode = mode;
  }
  static DebugVariablesModel& DebugVariables(WorkspaceShell& shell) {
    return shell.context_.current_project_state.debug_variables;
  }
  static DebugWatchModel& DebugWatch(WorkspaceShell& shell) {
    return shell.context_.current_project_state.debug_watch;
  }
  static void AddDebugWatchExpressionForTest(WorkspaceShell& shell, std::string expression) {
    shell.context_.current_project_state.debug_watch.AddExpression(std::move(expression));
  }
  static std::size_t DebugWatchRowCount(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.debug_watch.Rows().size();
  }
  static std::size_t DebugWatchSelectedRow(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.debug_watch.SelectedRow();
  }
  static int DebugPaneWatchScrollRow(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.debug_pane.watch_scroll_row;
  }
  static int DebugPaneVisibleRows(WorkspaceShell& shell) {
    const auto layout = shell.CurrentWorkspaceLayout();
    if (!layout.has_value()) {
      return 0;
    }
    return shell.ComputeDebugPaneListLayout(*layout, shell.DebugPaneActiveRowCount())
        .scroll.visible_rows;
  }
  static void ResetCaretBlink(WorkspaceShell& shell) { shell.ResetCaretBlink(); }
  static Uint64 CaretBlinkIntervalMs(const WorkspaceShell& shell) { return shell.CaretBlinkIntervalMs(); }
  static void SetCaretBlinkEpochMs(WorkspaceShell& shell, Uint64 epoch_ms) {
    shell.caret_blink_epoch_ms_ = epoch_ms;
  }
  static bool CaretVisibleNow(const WorkspaceShell& shell) { return shell.CaretVisibleNow(); }
  static bool CaretBlinkAnimating(const WorkspaceShell& shell) {
    return shell.CaretBlinkAnimating();
  }
  static std::optional<Uint32> NextCaretBlinkDelayMs(const WorkspaceShell& shell) {
    return shell.NextCaretBlinkDelayMs();
  }
  static void MoveMergeSelection(WorkspaceShell& shell, int delta) { shell.MoveMergeSelection(delta); }
  static bool ShouldBlinkCaret(const WorkspaceShell& shell) { return shell.ShouldBlinkCaret(); }
  static bool FocusIsOverlay(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.surface.focus == WorkspaceShell::FocusTarget::Overlay;
  }
  static bool MenuBarOpen(const WorkspaceShell& shell) { return shell.context_.menu_state.menu_bar_open; }
  static util::JsonValue ControlUi(WorkspaceShell& shell) {
    return ControlUiQuery::Build(shell.context_, shell.MakeControlUiOperations());
  }
  static void RegisterDebugAdapterForTest(WorkspaceShell& shell, const std::string& type,
                                          const std::vector<std::string>& command) {
    shell.CurrentDapManager().RegisterAdapter(type, platform::LocalProcessLauncher(), command);
  }
  static void ConsumeDapCallbacks(WorkspaceShell& shell) { shell.debug_service_.ConsumeDapCallbacks(); }
  static bool DebugSessionStopped(const WorkspaceShell& shell) { return shell.IsDebugSessionStopped(); }
  static bool DebugSessionActive(const WorkspaceShell& shell) { return shell.debug_service_.IsSessionActive(); }
  static bool TabDragBodyDropActive(const WorkspaceShell& shell) {
    const auto& drag = shell.context_.interaction_state.tab_drag;
    return drag.dragging && drag.body_drop();
  }
  static util::JsonValue ControlQuery(WorkspaceShell& shell, const std::string& verb) {
    bool ok = false;
    std::string error;
    return shell.control_channel_service_.AnswerQuery(verb, &ok, &error);
  }
  static util::JsonValue ControlQueryWithArgs(WorkspaceShell& shell, const std::string& verb,
                                              const util::JsonValue& args) {
    bool ok = false;
    std::string error;
    return shell.control_channel_service_.AnswerQuery(verb, args, &ok, &error);
  }
  static util::JsonValue ControlQueryWithError(WorkspaceShell& shell, const std::string& verb,
                                               std::string* error) {
    bool ok = false;
    return shell.control_channel_service_.AnswerQuery(verb, &ok, error);
  }
  static KeyInputCoordinator::DispatchProbe& KeyDispatchProbe(WorkspaceShell& shell) {
    return shell.MakeKeyInputCoordinator().dispatch_probe();
  }
  static const std::vector<ResolvedKeybinding>& ResolvedKeybindingsForTest(WorkspaceShell& shell) {
    return shell.ResolvedKeybindings();
  }
  static WorkspaceShell::MenuId ActiveMenuId(const WorkspaceShell& shell) {
    return shell.context_.menu_state.active_menu_id;
  }
  static void SetTreeSelectedIndexSilently(WorkspaceShell& shell, std::size_t index) {
    shell.context_.current_project_state.directory_tree.SetSelectedIndex(index);
  }
  static std::size_t TreeSelectedIndex(const WorkspaceShell& shell) {
    return shell.context_.current_project_state.directory_tree.selected_index();
  }
  static bool EditMenuOpen(const WorkspaceShell& shell) {
    return shell.context_.menu_state.menu_bar_open &&
           shell.context_.menu_state.active_menu_id == WorkspaceShell::MenuId::Edit;
  }
  static bool EditorContextMenuOpen(const WorkspaceShell& shell) {
    return shell.context_.menu_state.menu_bar_open &&
           shell.context_.menu_state.active_menu_id == WorkspaceShell::MenuId::EditorContext;
  }
  static bool FileMenuOpen(const WorkspaceShell& shell) {
    return shell.context_.menu_state.menu_bar_open &&
           shell.context_.menu_state.active_menu_id == WorkspaceShell::MenuId::File;
  }
  static bool EditorTabContextMenuOpen(const WorkspaceShell& shell) {
    return shell.context_.menu_state.menu_bar_open &&
           shell.context_.menu_state.active_menu_id == WorkspaceShell::MenuId::EditorTabContext;
  }
  static bool SidebarModeMenuOpen(const WorkspaceShell& shell) {
    return shell.context_.menu_state.menu_bar_open &&
           shell.context_.menu_state.active_menu_id == WorkspaceShell::MenuId::SidebarMode;
  }
  static bool TerminalTabContextMenuOpen(const WorkspaceShell& shell) {
    return shell.context_.menu_state.menu_bar_open &&
           shell.context_.menu_state.active_menu_id == WorkspaceShell::MenuId::TerminalTabContext;
  }
  static bool ProjectTabContextMenuOpen(const WorkspaceShell& shell) {
    return shell.context_.menu_state.menu_bar_open &&
           shell.context_.menu_state.active_menu_id == WorkspaceShell::MenuId::ProjectTabContext;
  }
  static bool TerminalContextMenuOpen(const WorkspaceShell& shell) {
    return shell.context_.menu_state.menu_bar_open &&
           shell.context_.menu_state.active_menu_id == WorkspaceShell::MenuId::TerminalContext;
  }
