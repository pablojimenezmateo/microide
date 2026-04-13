#pragma once

#include "workspace/WorkspaceShell.h"
#include "workspace/WorkspaceShellShared.h"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace microide::workspace {

struct WorkspaceShellTestAccess {
  static void SetProjectRoot(WorkspaceShell& shell, const std::filesystem::path& root) {
    shell.project_root_ = root.lexically_normal();
    shell.directory_tree_.SetRoot(shell.project_root_);
    shell.file_index_.SetRoot(shell.project_root_);
    shell.file_finder_.SetIndex(&shell.file_index_);
    shell.surface_.sidebar_visible = true;
    shell.surface_.sidebar_mode = WorkspaceShell::SidebarMode::Tree;
    shell.surface_.focus = WorkspaceShell::FocusTarget::Sidebar;
  }

  static void OpenSingleEditorTab(WorkspaceShell& shell, const std::filesystem::path& path) {
    editor::TextViewport opened_view;
    if (!opened_view.OpenFile(path)) {
      throw std::runtime_error("failed to open editor fixture: " + path.string());
    }
    shell.ApplyEditorPreferences(opened_view);
    shell.text_viewport_ = opened_view;
    shell.open_tabs_.push_back(WorkspaceShell::TabEntry{
        .kind = WorkspaceShell::TabEntry::Kind::Editor,
        .path = path.lexically_normal(),
        .title = path.filename().string(),
        .editor_state = WorkspaceShell::MakeEditorTabState(opened_view),
        .compare = std::nullopt,
        .merge = std::nullopt,
    });
    shell.active_tab_index_ = 0;
    shell.surface_.focus = WorkspaceShell::FocusTarget::Editor;
  }

  static editor::TextViewport& ActiveEditor(WorkspaceShell& shell) { return shell.text_viewport_; }
  static WorkspaceShell::CompareTabState& ActiveCompare(WorkspaceShell& shell) {
    return shell.open_tabs_[shell.active_tab_index_].compare.value();
  }
  static WorkspaceShell::MergeTabState& ActiveMerge(WorkspaceShell& shell) {
    return shell.open_tabs_[shell.active_tab_index_].merge.value();
  }
  static void ApplyMergeChoice(WorkspaceShell& shell, microide::compare::MergeChoice choice) {
    shell.ApplyMergeChoice(choice);
  }
  static void RefreshMergeTabDerivedState(WorkspaceShell& shell) {
    shell.RefreshMergeTabDerivedState(ActiveMerge(shell));
  }
  static const std::optional<WorkspaceShell::MergeHoverState>& ActiveMergeHoverState(
      const WorkspaceShell& shell) {
    return shell.open_tabs_[shell.active_tab_index_].merge->hover_state;
  }
  static bool ActiveMergeHoverIsIncomingConflict(const WorkspaceShell& shell) {
    return shell.open_tabs_[shell.active_tab_index_].merge->hover_state.has_value() &&
           shell.open_tabs_[shell.active_tab_index_].merge->hover_state->kind ==
               WorkspaceShell::MergeHoverState::Kind::IncomingConflict;
  }
  static microide::compare::MergeChoice ActiveMergeHoverPreviewChoice(const WorkspaceShell& shell) {
    const auto& hover = shell.open_tabs_[shell.active_tab_index_].merge->hover_state;
    return hover.has_value() ? hover->preview_choice : microide::compare::MergeChoice::Base;
  }
  static WorkspaceShell::MergeSurfaceLayout ActiveMergeSurfaceLayout(WorkspaceShell& shell) {
    const WorkspaceLayout layout =
        ComputeLayout(static_cast<float>(shell.last_window_width_),
                      static_cast<float>(shell.last_window_height_), shell.surface_.sidebar_visible,
                      shell.BottomPanelVisible(), shell.surface_.sidebar_width, shell.surface_.bottom_panel_height);
    return shell.ComputeMergeSurfaceLayout(layout.editor_surface, ActiveMerge(shell));
  }
  static WorkspaceShell::MergeInteractionLayout ActiveMergeInteractionLayout(WorkspaceShell& shell) {
    auto& merge = ActiveMerge(shell);
    const WorkspaceLayout layout =
        ComputeLayout(static_cast<float>(shell.last_window_width_),
                      static_cast<float>(shell.last_window_height_), shell.surface_.sidebar_visible,
                      shell.BottomPanelVisible(), shell.surface_.sidebar_width, shell.surface_.bottom_panel_height);
    const auto surface = shell.ComputeMergeSurfaceLayout(layout.editor_surface, merge);
    const auto scroll =
        shell.ComputeMergeScrollLayout(layout.editor_surface, surface, merge);
    merge.scroll_row = scroll.vertical_scroll;
    merge.horizontal_scroll = scroll.horizontal_scroll;
    merge.result_viewport.SetScrollLine(
        static_cast<std::size_t>(std::max(0, merge.scroll_row)));
    merge.result_viewport.SetHorizontalScroll(merge.horizontal_scroll);
    merge.scroll_row = static_cast<int>(merge.result_viewport.scroll_line());
    merge.horizontal_scroll = merge.result_viewport.horizontal_scroll();
    return shell.BuildMergeInteractionLayout(layout.editor_surface, surface, merge);
  }
  static WorkspaceShell::CompareSurfaceLayout ActiveCompareSurfaceLayout(WorkspaceShell& shell) {
    const WorkspaceLayout layout =
        ComputeLayout(static_cast<float>(shell.last_window_width_),
                      static_cast<float>(shell.last_window_height_), shell.surface_.sidebar_visible,
                      shell.BottomPanelVisible(), shell.surface_.sidebar_width, shell.surface_.bottom_panel_height);
    return shell.ComputeCompareSurfaceLayout(layout.editor_surface, ActiveCompare(shell));
  }
  static SDL_FRect ActiveMergeResultRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout =
        ComputeLayout(static_cast<float>(shell.last_window_width_),
                      static_cast<float>(shell.last_window_height_), shell.surface_.sidebar_visible,
                      shell.BottomPanelVisible(), shell.surface_.sidebar_width, shell.surface_.bottom_panel_height);
    const auto surface = ActiveMergeSurfaceLayout(shell);
    return ComputeMergeResultViewportRect(layout.editor_surface, surface.center_x, surface.rows_y,
                                          surface.gutter_width, surface.center_width,
                                          surface.show_horizontal);
  }
  static SDL_FRect MergeSourceAcceptRect(WorkspaceShell& shell,
                                         std::size_t conflict_index,
                                         bool incoming) {
    auto& merge = ActiveMerge(shell);
    const auto surface = ActiveMergeSurfaceLayout(shell);
    const auto interaction = ActiveMergeInteractionLayout(shell);
    const auto& conflict = merge.conflicts[conflict_index];
    return shell.BuildMergeSourceActionButtonRect(surface, interaction, conflict, incoming);
  }
  static std::array<SDL_FRect, 4> MergeResultActionRects(WorkspaceShell& shell,
                                                         std::size_t conflict_index) {
    auto& merge = ActiveMerge(shell);
    const auto surface = ActiveMergeSurfaceLayout(shell);
    const auto interaction = ActiveMergeInteractionLayout(shell);
    const auto& conflict = merge.conflicts[conflict_index];
    return shell.BuildMergeResultActionButtonRects(surface, interaction, conflict);
  }
  static std::array<SDL_FRect, 2> MergeToolbarNavigationRects(WorkspaceShell& shell) {
    const WorkspaceLayout layout =
        ComputeLayout(static_cast<float>(shell.last_window_width_),
                      static_cast<float>(shell.last_window_height_), shell.surface_.sidebar_visible,
                      shell.BottomPanelVisible(), shell.surface_.sidebar_width, shell.surface_.bottom_panel_height);
    const auto surface = ActiveMergeSurfaceLayout(shell);
    const auto toolbar = shell.ComputeMergeToolbarLayout(layout.editor_surface, surface);
    return {toolbar.prev_rect, toolbar.next_rect};
  }

  static void PrepareRenamePrompt(WorkspaceShell& shell,
                                  const std::filesystem::path& path,
                                  std::string input) {
    shell.OpenPromptSurface(WorkspaceShell::PromptSurfaceState::Action::RenamePath,
                            WorkspaceShell::PromptSurfaceState::Kind::TextInput, path,
                            std::move(input));
  }

  static void PrepareDeletePrompt(WorkspaceShell& shell, const std::filesystem::path& path) {
    shell.OpenPromptSurface(WorkspaceShell::PromptSurfaceState::Action::DeletePath,
                            WorkspaceShell::PromptSurfaceState::Kind::Confirm, path);
  }
  static void PrepareDiscardAllGitPrompt(WorkspaceShell& shell) {
    shell.OpenDiscardAllGitSidebarPrompt();
  }

  static void ConfirmPromptSurface(WorkspaceShell& shell) { shell.ConfirmPromptSurface(); }
  static bool SplitActiveEditor(WorkspaceShell& shell, bool vertical = true) {
    return shell.SplitActiveEditor(vertical ? WorkspaceShell::EditorSplitOrientation::Vertical
                                            : WorkspaceShell::EditorSplitOrientation::Horizontal);
  }
  static bool ActivateOrderedEditorSplit(WorkspaceShell& shell, std::size_t order_index) {
    return shell.ActivateOrderedEditorSplit(order_index);
  }
  static bool ReplaceActiveEditorWithFile(WorkspaceShell& shell, const std::filesystem::path& path) {
    editor::TextViewport opened_view;
    if (!opened_view.OpenFile(path)) {
      return false;
    }
    shell.ApplyEditorPreferences(opened_view);
    return shell.ReplaceActiveEditorView(opened_view);
  }
  static bool OpenWorkingTreeComparison(WorkspaceShell& shell,
                                        const std::filesystem::path& path,
                                        const std::string& left_ref,
                                        const std::string& left_label) {
    return shell.OpenWorkingTreeComparison(path, left_ref, left_label);
  }
  static bool OpenBranchHeadComparison(WorkspaceShell& shell,
                                       const std::filesystem::path& path,
                                       const std::string& left_ref,
                                       const std::string& left_label,
                                       const std::string& right_ref,
                                       const std::string& right_label) {
    return shell.OpenBranchHeadComparison(path, left_ref, left_label, right_ref, right_label);
  }
  static bool OpenMergeEditor(WorkspaceShell& shell,
                              const std::filesystem::path& base_path,
                              const std::filesystem::path& incoming_path,
                              const std::filesystem::path& current_path,
                              const std::filesystem::path& output_path) {
    return shell.OpenMergeEditor(base_path, incoming_path, current_path, output_path);
  }
  static bool OpenProjectTab(WorkspaceShell& shell,
                             const std::filesystem::path& project_root,
                             bool restore_persistence = false,
                             bool log_feedback = false) {
    return shell.OpenProjectTab(project_root, restore_persistence, log_feedback);
  }
  static void SetWindowSize(WorkspaceShell& shell, int width, int height) {
    shell.last_window_width_ = width;
    shell.last_window_height_ = height;
  }
  static bool ExecuteProjectOpenFromMenu(WorkspaceShell& shell) {
    return shell.ExecuteAction(WorkspaceShell::ActionId::ProjectOpen, {},
                               WorkspaceShell::ActionSource::Menu);
  }
  static bool ExecuteProjectOpenFromCommand(WorkspaceShell& shell) {
    return shell.ExecuteAction(WorkspaceShell::ActionId::ProjectOpen, {},
                               WorkspaceShell::ActionSource::Command);
  }
  static void ResetProjectScopedState(WorkspaceShell& shell, bool show_welcome) {
    shell.ResetProjectScopedState(show_welcome);
  }
  static bool ExecuteFilesFromShortcut(WorkspaceShell& shell) {
    return shell.ExecuteAction(WorkspaceShell::ActionId::Files, {},
                               WorkspaceShell::ActionSource::Shortcut);
  }
  static void SetClipboardTextReader(
      WorkspaceShell& shell,
      std::function<std::optional<std::string>()> reader) {
    shell.clipboard_text_reader_ = std::move(reader);
  }
  static void SetClipboardTextWriter(WorkspaceShell& shell,
                                     std::function<bool(std::string_view)> writer) {
    shell.clipboard_text_writer_ = std::move(writer);
  }
  static void SetProjectOpenDialogLauncher(
      WorkspaceShell& shell,
      std::function<bool(WorkspaceShell&, const std::filesystem::path&)> launcher) {
    shell.project_open_dialog_launcher_ = std::move(launcher);
  }
  static void QueueProjectOpenDialogSelection(WorkspaceShell& shell,
                                              const std::filesystem::path& path) {
    std::lock_guard<std::mutex> lock(shell.project_open_dialog_mutex_);
    shell.pending_project_open_dialog_result_ = WorkspaceShell::PendingProjectOpenDialogResult{
        .ready = true,
        .cancelled = false,
        .selected_path = path.lexically_normal(),
        .error_message = {},
    };
  }
  static void QueueProjectOpenDialogCancel(WorkspaceShell& shell) {
    std::lock_guard<std::mutex> lock(shell.project_open_dialog_mutex_);
    shell.pending_project_open_dialog_result_ = WorkspaceShell::PendingProjectOpenDialogResult{
        .ready = true,
        .cancelled = true,
        .selected_path = {},
        .error_message = {},
    };
  }
  static void ConsumePendingProjectOpenDialogResult(WorkspaceShell& shell) {
    shell.ConsumePendingProjectOpenDialogResult();
  }
  static bool SwitchProject(WorkspaceShell& shell,
                            std::size_t index,
                            bool log_feedback = false) {
    return shell.SwitchProject(index, log_feedback);
  }
  static void RequestCloseProject(WorkspaceShell& shell, std::size_t index) {
    shell.RequestCloseProject(index);
  }
  static void CloseProject(WorkspaceShell& shell, std::size_t index) { shell.CloseProject(index); }
  static void OpenFile(WorkspaceShell& shell, const std::filesystem::path& path) {
    shell.OpenFile(path);
  }
  static bool OpenFileInNewTab(WorkspaceShell& shell, const std::filesystem::path& path) {
    return shell.OpenFileInNewTab(path);
  }
  static bool ExecuteCopySelectionWithContext(WorkspaceShell& shell) {
    return shell.ExecuteAction(WorkspaceShell::ActionId::CopySelectionWithContext, {},
                               WorkspaceShell::ActionSource::Menu);
  }
  static bool ExecuteCopyLastTerminalCommand(WorkspaceShell& shell) {
    return shell.ExecuteAction(WorkspaceShell::ActionId::CopyLastTerminalCommand, {},
                               WorkspaceShell::ActionSource::Menu);
  }
  static bool SaveTab(WorkspaceShell& shell, std::size_t index) { return shell.SaveTab(index); }
  static void ActivateTab(WorkspaceShell& shell, std::size_t index) { shell.ActivateTab(index); }
  static bool SelectTreePath(WorkspaceShell& shell, const std::filesystem::path& path) {
    return shell.directory_tree_.SelectPath(path);
  }
  static void CollapseTreeSelection(WorkspaceShell& shell) { shell.directory_tree_.CollapseSelection(); }
  static void ShowSearchSidebar(WorkspaceShell& shell,
                                std::string query,
                                bool temporary = false) {
    shell.ShowSearchSidebar(std::move(query), temporary);
  }
  static void ShowGitSidebar(WorkspaceShell& shell) { shell.ShowGitSidebar(); }
  static void RefreshGitSidebar(WorkspaceShell& shell) { shell.RefreshGitSidebar(); }
  static void ConsumeProjectSearchUpdates(WorkspaceShell& shell) {
    shell.ConsumeProjectSearchUpdates();
  }
  static void ToggleProjectSearchPatternMode(WorkspaceShell& shell) {
    shell.ToggleProjectSearchPatternMode();
  }
  static void CycleProjectSearchCaseMode(WorkspaceShell& shell) {
    shell.CycleProjectSearchCaseMode();
  }
  static void ToggleProjectSearchHiddenFiles(WorkspaceShell& shell) {
    shell.ToggleProjectSearchHiddenFiles();
  }
  static void EnsureTerminalTab(WorkspaceShell& shell) {
    if (shell.terminal_tabs_.empty()) {
      shell.terminal_tabs_.push_back(std::make_unique<WorkspaceShell::TerminalTabState>());
    }
    shell.active_terminal_tab_index_ = shell.terminal_tabs_.size() - 1;
    shell.surface_.focus = WorkspaceShell::FocusTarget::Panel;
  }
  static void AddTerminalTab(WorkspaceShell& shell) {
    shell.terminal_tabs_.push_back(std::make_unique<WorkspaceShell::TerminalTabState>());
    shell.active_terminal_tab_index_ = shell.terminal_tabs_.size() - 1;
    shell.surface_.focus = WorkspaceShell::FocusTarget::Panel;
  }
  static void ConsumeTerminalSessionUpdates(WorkspaceShell& shell) {
    shell.ConsumeTerminalSessionUpdates();
  }
  static microide::terminal::TerminalSession& ActiveTerminalSession(WorkspaceShell& shell) {
    return shell.terminal_tabs_[shell.active_terminal_tab_index_]->session;
  }
  static bool HandleTerminalKeyDown(WorkspaceShell& shell,
                                    SDL_Keycode key,
                                    SDL_Keymod modifiers) {
    SDL_KeyboardEvent event{};
    event.key = key;
    return shell.HandleTerminalKeyDown(event, modifiers);
  }
  static bool HandleTextInput(WorkspaceShell& shell, std::string_view text) {
    SDL_TextInputEvent event{};
    const std::string storage(text);
    event.text = storage.c_str();
    return shell.HandleTextInput(event);
  }
  static bool HandleKeyEvent(WorkspaceShell& shell, SDL_Keycode key, SDL_Keymod modifiers) {
    SDL_Event event{};
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.key = key;
    event.key.mod = modifiers;
    return shell.HandleEvent(event);
  }
  static bool HandleWindowFocusEvent(WorkspaceShell& shell, bool focused) {
    SDL_Event event{};
    event.type = focused ? SDL_EVENT_WINDOW_FOCUS_GAINED : SDL_EVENT_WINDOW_FOCUS_LOST;
    return shell.HandleEvent(event);
  }
  static bool HandleMouseButtonDown(WorkspaceShell& shell, float x, float y, Uint8 button) {
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    event.button.button = button;
    event.button.x = x;
    event.button.y = y;
    return shell.HandleEvent(event);
  }
  static bool HandleMouseButtonUp(WorkspaceShell& shell, float x, float y, Uint8 button) {
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_BUTTON_UP;
    event.button.button = button;
    event.button.x = x;
    event.button.y = y;
    return shell.HandleEvent(event);
  }
  static bool HandleMouseMotion(WorkspaceShell& shell, float x, float y, SDL_MouseButtonFlags state) {
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.x = x;
    event.motion.y = y;
    event.motion.state = state;
    return shell.HandleEvent(event);
  }
  static SDL_FRect ProjectTabRect(WorkspaceShell& shell, std::size_t index) {
    const WorkspaceLayout layout =
        ComputeLayout(static_cast<float>(shell.last_window_width_),
                      static_cast<float>(shell.last_window_height_), shell.surface_.sidebar_visible,
                      shell.BottomPanelVisible(), shell.surface_.sidebar_width, shell.surface_.bottom_panel_height);
    for (const WorkspaceShell::VisibleProjectTab& tab :
         shell.ComputeVisibleProjectTabs(layout.project_tab_strip)) {
      if (tab.index == index) {
        return tab.rect;
      }
    }
    return {};
  }
  static SDL_FRect EditorTabRect(WorkspaceShell& shell, std::size_t index) {
    const WorkspaceLayout layout =
        ComputeLayout(static_cast<float>(shell.last_window_width_),
                      static_cast<float>(shell.last_window_height_), shell.surface_.sidebar_visible,
                      shell.BottomPanelVisible(), shell.surface_.sidebar_width, shell.surface_.bottom_panel_height);
    for (const WorkspaceShell::VisibleTab& tab : shell.ComputeVisibleTabs(layout.tab_strip)) {
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
    const WorkspaceLayout layout =
        ComputeLayout(static_cast<float>(shell.last_window_width_),
                      static_cast<float>(shell.last_window_height_), shell.surface_.sidebar_visible,
                      shell.BottomPanelVisible(), shell.surface_.sidebar_width, shell.surface_.bottom_panel_height);
    return shell.HoveredTabTooltipLabel(layout.tab_strip);
  }
  static SDL_FRect ActiveEditorPaneRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout =
        ComputeLayout(static_cast<float>(shell.last_window_width_),
                      static_cast<float>(shell.last_window_height_), shell.surface_.sidebar_visible,
                      shell.BottomPanelVisible(), shell.surface_.sidebar_width, shell.surface_.bottom_panel_height);
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
  static microide::editor::EditorViewMetrics ActiveEditorMetrics(WorkspaceShell& shell) {
    const SDL_FRect pane = ActiveEditorPaneRect(shell);
    return microide::editor::EditorViewRenderer::ComputeMetrics(shell.text_renderer_,
                                                                shell.text_viewport_, pane);
  }
  static float TextCharWidth(WorkspaceShell& shell) { return shell.text_renderer_.CharWidth(); }
  static std::optional<microide::editor::EditorBlameOverlay> ActiveEditorBlameOverlay(
      WorkspaceShell& shell) {
    const WorkspaceLayout layout =
        ComputeLayout(static_cast<float>(shell.last_window_width_),
                      static_cast<float>(shell.last_window_height_), shell.surface_.sidebar_visible,
                      shell.BottomPanelVisible(), shell.surface_.sidebar_width, shell.surface_.bottom_panel_height);
    if (!shell.ActiveTabIsEditor()) {
      return std::nullopt;
    }
    shell.SyncActiveEditorTab();
    auto* editor_tab = shell.ActiveEditorTab();
    if (editor_tab == nullptr) {
      return std::nullopt;
    }
    shell.NormalizeEditorSplitTree(*editor_tab);
    const auto panes = shell.ComputeEditorPaneLayouts(layout.editor_surface);
    for (const auto& pane : panes) {
      if (pane.active) {
        return shell.BuildEditorBlameOverlay(shell.text_viewport_, pane.rect);
      }
    }
    return shell.text_viewport_.is_placeholder()
               ? shell.BuildEditorBlameOverlay(shell.text_viewport_, layout.editor_surface)
               : std::nullopt;
  }
  static std::optional<microide::editor::EditorBlameOverlay> ActiveCompareBlameOverlay(
      WorkspaceShell& shell) {
    const WorkspaceLayout layout =
        ComputeLayout(static_cast<float>(shell.last_window_width_),
                      static_cast<float>(shell.last_window_height_), shell.surface_.sidebar_visible,
                      shell.BottomPanelVisible(), shell.surface_.sidebar_width, shell.surface_.bottom_panel_height);
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
  static std::optional<SDL_FRect> ActiveEditorBlamePopupRect(WorkspaceShell& shell) {
    const auto popup = shell.ActiveEditorBlamePopupLayout();
    return popup.has_value() ? std::make_optional(popup->rect) : std::nullopt;
  }
  static std::optional<SDL_FRect> ActiveEditorBlamePopupCopyShaRect(WorkspaceShell& shell) {
    const auto popup = shell.ActiveEditorBlamePopupLayout();
    return popup.has_value() ? std::make_optional(popup->copy_sha_rect) : std::nullopt;
  }
  static SDL_FRect ActiveTerminalTabRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout =
        ComputeLayout(static_cast<float>(shell.last_window_width_),
                      static_cast<float>(shell.last_window_height_), shell.surface_.sidebar_visible,
                      shell.BottomPanelVisible(), shell.surface_.sidebar_width, shell.surface_.bottom_panel_height);
    const SDL_FRect panel_header =
        MakeRect(layout.bottom_panel.x, layout.bottom_panel.y, layout.bottom_panel.w, 28.0f);
    for (const WorkspaceShell::VisibleTerminalTab& tab : shell.ComputeVisibleTerminalTabs(panel_header)) {
      if (tab.index == shell.active_terminal_tab_index_) {
        return tab.rect;
      }
    }
    return {};
  }
  static SDL_FRect TerminalTabRect(WorkspaceShell& shell, std::size_t index) {
    const WorkspaceLayout layout =
        ComputeLayout(static_cast<float>(shell.last_window_width_),
                      static_cast<float>(shell.last_window_height_), shell.surface_.sidebar_visible,
                      shell.BottomPanelVisible(), shell.surface_.sidebar_width, shell.surface_.bottom_panel_height);
    const SDL_FRect panel_header =
        MakeRect(layout.bottom_panel.x, layout.bottom_panel.y, layout.bottom_panel.w, 28.0f);
    for (const WorkspaceShell::VisibleTerminalTab& tab : shell.ComputeVisibleTerminalTabs(panel_header)) {
      if (tab.index == index) {
        return tab.rect;
      }
    }
    return {};
  }
  static bool RestoreSessionState(WorkspaceShell& shell) { return shell.RestoreSessionState(); }
  static void SaveSessionState(WorkspaceShell& shell) { shell.SaveSessionState(); }
  static bool RestoreWorkspaceSession(WorkspaceShell& shell) {
    return shell.RestoreWorkspaceSession();
  }
  static void SaveWorkspaceSession(WorkspaceShell& shell) { shell.SaveWorkspaceSession(); }
  static void RequestQuit(WorkspaceShell& shell) { shell.RequestQuit(); }
  static bool ConsumeQuitRequested(WorkspaceShell& shell) { return shell.ConsumeQuitRequested(); }

  static void ConfirmDirtyPrompt(WorkspaceShell& shell, int selected_action) {
    shell.prompts_.dirty.selected_action = selected_action;
    shell.ConfirmDirtyPrompt();
  }
  static bool StageAllGitSidebarEntries(WorkspaceShell& shell) {
    return shell.StageAllGitSidebarEntries();
  }
  static bool DiscardAllGitSidebarEntries(WorkspaceShell& shell) {
    return shell.DiscardAllGitSidebarEntries();
  }

  static bool DirtyPromptVisible(const WorkspaceShell& shell) { return shell.prompts_.dirty_visible; }
  static std::string DirtyPromptMessage(const WorkspaceShell& shell) {
    return shell.DirtyPromptMessage();
  }
  static bool PromptSurfaceVisible(const WorkspaceShell& shell) {
    return shell.prompts_.surface_visible;
  }
  static std::string PromptSurfaceTitle(const WorkspaceShell& shell) {
    return shell.PromptSurfaceTitle();
  }
  static std::string PromptSurfaceMessage(const WorkspaceShell& shell) {
    return shell.PromptSurfaceMessage();
  }
  static const std::vector<WorkspaceShell::TabEntry>& OpenTabs(const WorkspaceShell& shell) {
    return shell.open_tabs_;
  }
  static std::size_t ActiveTabIndex(const WorkspaceShell& shell) { return shell.active_tab_index_; }
  static std::size_t ActiveTerminalTabIndex(const WorkspaceShell& shell) {
    return shell.active_terminal_tab_index_;
  }
  static std::vector<std::filesystem::path> ProjectRoots(const WorkspaceShell& shell) {
    std::vector<std::filesystem::path> roots;
    roots.reserve(shell.project_catalog_.entries.size());
    for (std::size_t i = 0; i < shell.project_catalog_.entries.size(); ++i) {
      roots.push_back(shell.ProjectCatalogRoot(i));
    }
    return roots;
  }
  static std::vector<std::string> TerminalLaunchLabels(WorkspaceShell& shell) {
    std::vector<std::string> labels;
    labels.reserve(shell.terminal_tabs_.size());
    for (const auto& terminal_tab : shell.terminal_tabs_) {
      labels.push_back(terminal_tab == nullptr ? std::string{} : terminal_tab->session.LaunchLabel());
    }
    return labels;
  }
  static std::vector<std::string> VisibleMenuBarLabels(WorkspaceShell& shell) {
    const WorkspaceLayout layout =
        ComputeLayout(static_cast<float>(shell.last_window_width_),
                      static_cast<float>(shell.last_window_height_), shell.surface_.sidebar_visible,
                      shell.BottomPanelVisible(), shell.surface_.sidebar_width, shell.surface_.bottom_panel_height);
    std::vector<std::string> labels;
    for (const auto& item : shell.ComputeVisibleMenuBarItems(layout.menu_bar)) {
      if (const auto* menu = shell.FindMenuSpec(item.id); menu != nullptr) {
        labels.emplace_back(menu->label);
      }
    }
    return labels;
  }
  static std::string BreadcrumbLabel(WorkspaceShell& shell) { return shell.BreadcrumbLabel(); }
  static const std::vector<project::TreeEntry>& TreeEntries(const WorkspaceShell& shell) {
    return shell.directory_tree_.entries();
  }
  static std::filesystem::path SelectedTreePath(const WorkspaceShell& shell) {
    return shell.SelectedTreePath();
  }
  static std::size_t ProjectCount(const WorkspaceShell& shell) { return shell.project_catalog_.entries.size(); }
  static std::size_t ActiveProjectIndex(const WorkspaceShell& shell) {
    return shell.project_catalog_.active_index;
  }
  static const std::filesystem::path& ProjectRoot(const WorkspaceShell& shell) {
    return shell.project_root_;
  }
  static const std::vector<project::ProjectSearchResult>& ProjectSearchResults(
      const WorkspaceShell& shell) {
    return shell.overlay_workflow_.project_search.results;
  }
  static bool ProjectSearchRunning(const WorkspaceShell& shell) {
    return shell.overlay_workflow_.project_search.running;
  }
  static bool ProjectSearchTruncated(const WorkspaceShell& shell) {
    return shell.overlay_workflow_.project_search.truncated;
  }
  static const std::string& ProjectSearchError(const WorkspaceShell& shell) {
    return shell.overlay_workflow_.project_search.error;
  }
  static bool ProjectOpenDialogActive(const WorkspaceShell& shell) {
    return shell.project_open_dialog_active_;
  }
  static bool CommandMode(const WorkspaceShell& shell) { return shell.surface_.command_mode; }
  static const std::string& CommandInput(const WorkspaceShell& shell) { return shell.command_.input; }
  static std::string CommandPromptStatusText(const WorkspaceShell& shell) {
    return shell.CommandPromptStatusText();
  }
  static bool OverlayVisible(const WorkspaceShell& shell) { return shell.surface_.overlay_visible; }
  static bool OverlayModeIsFileFinder(const WorkspaceShell& shell) {
    return shell.surface_.overlay_mode == WorkspaceShell::OverlayMode::FileFinder;
  }
  static bool SidebarVisible(const WorkspaceShell& shell) { return shell.surface_.sidebar_visible; }
  static float SidebarWidth(const WorkspaceShell& shell) { return shell.surface_.sidebar_width; }
  static float UiScale(const WorkspaceShell& shell) { return shell.UiScale(); }
  static bool SoftTabsEnabled(const WorkspaceShell& shell) {
    return shell.editor_preferences_.soft_tabs;
  }
  static bool FocusIsEditor(const WorkspaceShell& shell) {
    return shell.surface_.focus == WorkspaceShell::FocusTarget::Editor;
  }
  static bool FocusIsSidebar(const WorkspaceShell& shell) {
    return shell.surface_.focus == WorkspaceShell::FocusTarget::Sidebar;
  }
  static bool FocusIsPanel(const WorkspaceShell& shell) {
    return shell.surface_.focus == WorkspaceShell::FocusTarget::Panel;
  }
  static bool FocusIsOverlay(const WorkspaceShell& shell) {
    return shell.surface_.focus == WorkspaceShell::FocusTarget::Overlay;
  }
  static bool MenuBarOpen(const WorkspaceShell& shell) { return shell.surface_.menu_bar_open; }
  static bool EditMenuOpen(const WorkspaceShell& shell) {
    return shell.surface_.menu_bar_open && shell.surface_.active_menu_id == WorkspaceShell::MenuId::Edit;
  }
  static bool TerminalTabContextMenuOpen(const WorkspaceShell& shell) {
    return shell.surface_.menu_bar_open &&
           shell.surface_.active_menu_id == WorkspaceShell::MenuId::TerminalTabContext;
  }
};

}  // namespace microide::workspace
