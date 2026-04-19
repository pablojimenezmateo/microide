#pragma once

#include "workspace/WorkspaceActionCoordinator.h"
#include "workspace/WorkspaceCommandPromptCoordinator.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/WorkspacePersistenceCoordinator.h"
#include "workspace/WorkspaceShell.h"
#include "workspace/WorkspaceTextInputCoordinator.h"

#include <algorithm>
#include <array>
#include <chrono>
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
    shell.surface_.sidebar_view_id = "tree";
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
  static bool ActiveEditorHasSelection(const WorkspaceShell& shell) {
    return shell.text_viewport_.selection_range().has_value();
  }
  static std::string ActiveEditorSelectedText(WorkspaceShell& shell) {
    return shell.text_viewport_.SelectedText();
  }
  static WorkspaceShell::CompareTabState& ActiveCompare(WorkspaceShell& shell) {
    return shell.open_tabs_[shell.active_tab_index_].compare.value();
  }
  static WorkspaceShell::MergeTabState& ActiveMerge(WorkspaceShell& shell) {
    return shell.open_tabs_[shell.active_tab_index_].merge.value();
  }
  static bool ActiveMergeHasSelection(const WorkspaceShell& shell) {
    return shell.open_tabs_[shell.active_tab_index_]
        .merge->result_viewport.selection_range()
        .has_value();
  }
  static std::string ActiveMergeSelectedText(WorkspaceShell& shell) {
    return shell.open_tabs_[shell.active_tab_index_].merge->result_viewport.SelectedText();
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
  static WorkspaceLayout CurrentLayout(WorkspaceShell& shell) {
    return shell.CurrentWorkspaceLayout().value();
  }
  static WorkspaceShell::MergeSurfaceLayout ActiveMergeSurfaceLayout(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return shell.ComputeMergeSurfaceLayout(layout.editor_surface, ActiveMerge(shell));
  }
  static WorkspaceShell::MergeInteractionLayout ActiveMergeInteractionLayout(WorkspaceShell& shell) {
    auto& merge = ActiveMerge(shell);
    const WorkspaceLayout layout = CurrentLayout(shell);
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
    const WorkspaceLayout layout = CurrentLayout(shell);
    return shell.ComputeCompareSurfaceLayout(layout.editor_surface, ActiveCompare(shell));
  }
  static SDL_FRect ActiveCompareEditableRect(WorkspaceShell& shell) {
    auto& compare = ActiveCompare(shell);
    const auto surface = ActiveCompareSurfaceLayout(shell);
    return shell.BuildCompareRightInteractionLayout(surface, compare).rect;
  }
  static SDL_FRect ActiveMergeResultRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
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
    const WorkspaceLayout layout = CurrentLayout(shell);
    const auto surface = ActiveMergeSurfaceLayout(shell);
    const auto toolbar = shell.ComputeMergeToolbarLayout(layout.editor_surface, surface);
    return {toolbar.prev_rect, toolbar.next_rect};
  }
  static std::array<SDL_FRect, 2> MergeDividerRects(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    const auto surface = ActiveMergeSurfaceLayout(shell);
    return {MakeRect(surface.center_x - surface.divider_width, layout.editor_surface.y,
                     surface.divider_width, layout.editor_surface.h),
            MakeRect(surface.right_x - surface.divider_width, layout.editor_surface.y,
                     surface.divider_width, layout.editor_surface.h)};
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
    shell.window_presentation_.logical_width = width;
    shell.window_presentation_.logical_height = height;
  }
  static void SetWindowChromeEnabled(WorkspaceShell& shell,
                                     bool enabled,
                                     bool maximized = false,
                                     bool fullscreen = false) {
    shell.window_presentation_.chrome = WorkspaceShell::WindowChromeState{
        .custom_enabled = enabled,
        .maximized = maximized,
        .fullscreen = fullscreen,
    };
  }
  static void RenderFrame(WorkspaceShell& shell) {
    shell.Render(nullptr, shell.window_presentation_.logical_width,
                 shell.window_presentation_.logical_height);
  }
  static bool ExecuteProjectOpenFromMenu(WorkspaceShell& shell) {
    return WorkspaceShell::ActionCoordinator(shell).Execute(
        WorkspaceShell::ActionId::ProjectOpen, {}, WorkspaceShell::ActionSource::Menu);
  }
  static bool ExecuteProjectOpenFromCommand(WorkspaceShell& shell) {
    return WorkspaceShell::ActionCoordinator(shell).Execute(
        WorkspaceShell::ActionId::ProjectOpen, {}, WorkspaceShell::ActionSource::Command);
  }
  static void ResetProjectScopedState(WorkspaceShell& shell, bool show_welcome) {
    shell.ResetProjectScopedState(show_welcome);
  }
  static bool ExecuteFilesFromShortcut(WorkspaceShell& shell) {
    return WorkspaceShell::ActionCoordinator(shell).Execute(WorkspaceShell::ActionId::Files, {},
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
  static void SetPrimarySelectionTextReader(
      WorkspaceShell& shell,
      std::function<std::optional<std::string>()> reader) {
    shell.primary_selection_text_reader_ = std::move(reader);
  }
  static void SetPrimarySelectionTextWriter(
      WorkspaceShell& shell,
      std::function<bool(std::string_view)> writer) {
    shell.primary_selection_text_writer_ = std::move(writer);
  }
  static void SetExternalUrlOpener(WorkspaceShell& shell,
                                   std::function<bool(std::string_view)> opener) {
    shell.external_url_opener_ = std::move(opener);
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
  static bool ExecuteCommandLine(WorkspaceShell& shell, const std::string& command_line) {
    return WorkspaceShell::CommandPromptCoordinator(shell).ExecuteCommandLine(command_line);
  }
  static const std::vector<std::string>& PluginMessages(const WorkspaceShell& shell) {
    return shell.plugin_runtime_.Host().Messages();
  }
  static const std::vector<std::string>& PluginErrors(const WorkspaceShell& shell) {
    return shell.plugin_runtime_.Host().Errors();
  }
  static void ClearPluginMessages(WorkspaceShell& shell) {
    shell.plugin_runtime_.Host().ClearMessages();
  }
  static void SetPluginAssetPollInterval(WorkspaceShell& shell,
                                         std::chrono::milliseconds poll_interval) {
    shell.plugin_runtime_.SetPollInterval(poll_interval);
  }
  static WorkspaceShell::EventResult HandleScheduledWake(WorkspaceShell& shell) {
    return shell.HandleScheduledWake();
  }
  static const std::vector<std::string>* OutputChannelEntries(const WorkspaceShell& shell,
                                                              std::string_view id) {
    return shell.plugin_runtime_.OutputChannelEntries(id);
  }
  static const std::vector<editor::PublishedDiagnostic>* DiagnosticsForPath(
      const WorkspaceShell& shell,
      const std::filesystem::path& path) {
    return shell.diagnostics_store_.FindByPath(path);
  }
  static bool PublishDiagnostics(WorkspaceShell& shell,
                                 std::string_view owner,
                                 const std::filesystem::path& path,
                                 std::vector<editor::Diagnostic> diagnostics) {
    return shell.diagnostics_store_.ReplaceForOwnerFile(owner, path, std::move(diagnostics));
  }
  static bool ExecuteCopySelectionWithContext(WorkspaceShell& shell) {
    return WorkspaceShell::ActionCoordinator(shell).Execute(
        WorkspaceShell::ActionId::CopySelectionWithContext, {},
        WorkspaceShell::ActionSource::Menu);
  }
  static bool ExecutePasteClipboard(WorkspaceShell& shell) {
    return WorkspaceShell::ActionCoordinator(shell).Execute(
        WorkspaceShell::ActionId::PasteClipboard, {}, WorkspaceShell::ActionSource::Menu);
  }
  static bool ExecuteCopyLastTerminalCommand(WorkspaceShell& shell) {
    return WorkspaceShell::ActionCoordinator(shell).Execute(
        WorkspaceShell::ActionId::CopyLastTerminalCommand, {},
        WorkspaceShell::ActionSource::Menu);
  }
  static bool ExecuteCloseAllTabs(WorkspaceShell& shell) {
    return WorkspaceShell::ActionCoordinator(shell).Execute(
        WorkspaceShell::ActionId::CloseAllTabs, {}, WorkspaceShell::ActionSource::Menu);
  }
  static bool ExecuteCloseOtherTabs(WorkspaceShell& shell) {
    return WorkspaceShell::ActionCoordinator(shell).Execute(
        WorkspaceShell::ActionId::CloseOtherTabs, {}, WorkspaceShell::ActionSource::Menu);
  }
  static bool ExecuteCloseTabsToRight(WorkspaceShell& shell) {
    return WorkspaceShell::ActionCoordinator(shell).Execute(
        WorkspaceShell::ActionId::CloseTabsToRight, {}, WorkspaceShell::ActionSource::Menu);
  }
  static bool ExecuteCloseTabsToLeft(WorkspaceShell& shell) {
    return WorkspaceShell::ActionCoordinator(shell).Execute(
        WorkspaceShell::ActionId::CloseTabsToLeft, {}, WorkspaceShell::ActionSource::Menu);
  }
  static bool ExecuteTreeRefresh(WorkspaceShell& shell) {
    return WorkspaceShell::ActionCoordinator(shell).Execute(
        WorkspaceShell::ActionId::TreeRefresh, {}, WorkspaceShell::ActionSource::Menu);
  }
  static bool ExecuteGitRefresh(WorkspaceShell& shell) {
    return WorkspaceShell::ActionCoordinator(shell).Execute(
        WorkspaceShell::ActionId::GitRefresh, {}, WorkspaceShell::ActionSource::Menu);
  }
  static bool ExecuteShowGitSidebar(WorkspaceShell& shell) {
    return WorkspaceShell::ActionCoordinator(shell).Execute(
        WorkspaceShell::ActionId::SidebarShow, {"git"}, WorkspaceShell::ActionSource::Menu);
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
  static void ShowProblemsSidebar(WorkspaceShell& shell) { shell.ShowProblemsSidebar(); }
  static void ShowGitSidebar(WorkspaceShell& shell) { shell.ShowGitSidebar(); }
  static bool RefreshProblemsSidebar(WorkspaceShell& shell) {
    return shell.RefreshProblemsSidebar();
  }
  static void RefreshGitSidebar(WorkspaceShell& shell) { shell.RefreshGitSidebar(); }
  static const std::vector<WorkspaceShell::ProblemsSidebarEntry>& ProblemsSidebarEntries(
      const WorkspaceShell& shell) {
    return shell.problems_sidebar_.entries;
  }
  static const std::vector<WorkspaceShell::GitSidebarEntry>& GitSidebarEntries(
      const WorkspaceShell& shell) {
    return shell.git_sidebar_.entries;
  }
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
  static void SetActiveTerminalFollowTail(WorkspaceShell& shell, bool follow_tail) {
    shell.terminal_tabs_[shell.active_terminal_tab_index_]->follow_tail = follow_tail;
  }
  static bool ActiveTerminalFollowTail(const WorkspaceShell& shell) {
    return shell.terminal_tabs_[shell.active_terminal_tab_index_]->follow_tail;
  }
  static void SetActiveTerminalScrollRow(WorkspaceShell& shell, int scroll_row) {
    shell.terminal_tabs_[shell.active_terminal_tab_index_]->scroll_row = scroll_row;
  }
  static int ActiveTerminalScrollRow(const WorkspaceShell& shell) {
    return shell.terminal_tabs_[shell.active_terminal_tab_index_]->scroll_row;
  }
  static bool HandleTerminalKeyDown(WorkspaceShell& shell,
                                    SDL_Keycode key,
                                    SDL_Keymod modifiers) {
    SDL_KeyboardEvent event{};
    event.key = key;
    return WorkspaceShell::TextInputCoordinator(shell).HandleTerminalKeyDown(event, modifiers);
  }
  static bool HandleTextInput(WorkspaceShell& shell, std::string_view text) {
    SDL_TextInputEvent event{};
    const std::string storage(text);
    event.text = storage.c_str();
    return WorkspaceShell::TextInputCoordinator(shell).HandleTextInput(event);
  }
  static bool HandleKeyEvent(WorkspaceShell& shell, SDL_Keycode key, SDL_Keymod modifiers) {
    SDL_Event event{};
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.key = key;
    event.key.mod = modifiers;
    return shell.HandleEvent(event).handled;
  }
  static bool HandleWindowFocusEvent(WorkspaceShell& shell, bool focused) {
    SDL_Event event{};
    event.type = focused ? SDL_EVENT_WINDOW_FOCUS_GAINED : SDL_EVENT_WINDOW_FOCUS_LOST;
    return shell.HandleEvent(event).handled;
  }
  static bool HandleMouseButtonDown(WorkspaceShell& shell, float x, float y, Uint8 button) {
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    event.button.button = button;
    event.button.x = x;
    event.button.y = y;
    return shell.HandleEvent(event).handled;
  }
  static bool HandleMouseButtonDown(WorkspaceShell& shell,
                                    float x,
                                    float y,
                                    Uint8 button,
                                    Uint8 clicks) {
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    event.button.button = button;
    event.button.x = x;
    event.button.y = y;
    event.button.clicks = clicks;
    return shell.HandleEvent(event).handled;
  }
  static bool HandleMouseButtonUp(WorkspaceShell& shell, float x, float y, Uint8 button) {
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_BUTTON_UP;
    event.button.button = button;
    event.button.x = x;
    event.button.y = y;
    return shell.HandleEvent(event).handled;
  }
  static bool HandleMouseMotion(WorkspaceShell& shell, float x, float y, SDL_MouseButtonFlags state) {
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.x = x;
    event.motion.y = y;
    event.motion.state = state;
    return shell.HandleEvent(event).handled;
  }
  static bool HandleMouseWheel(WorkspaceShell& shell,
                               float x,
                               float y,
                               int vertical_ticks,
                               int horizontal_ticks = 0) {
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_WHEEL;
    event.wheel.mouse_x = x;
    event.wheel.mouse_y = y;
    event.wheel.integer_x = horizontal_ticks;
    event.wheel.integer_y = vertical_ticks;
    event.wheel.x = static_cast<float>(horizontal_ticks);
    event.wheel.y = static_cast<float>(vertical_ticks);
    return shell.HandleEvent(event).handled;
  }
  static int ProjectTabScrollIndex(const WorkspaceShell& shell) {
    return shell.project_catalog_.tab_scroll_index;
  }
  static int EditorTabScrollIndex(const WorkspaceShell& shell) { return shell.tab_scroll_index_; }
  static SDL_FRect BottomPanelContentRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return microide::workspace::BottomPanelContentRect(layout, shell.surface_.command_mode);
  }
  static SDL_FPoint TerminalCellPoint(WorkspaceShell& shell,
                                      std::size_t row,
                                      std::size_t column) {
    const auto* terminal_tab = shell.ActiveTerminalTab();
    const auto lines = terminal_tab != nullptr ? terminal_tab->session.SnapshotLines()
                                               : std::vector<microide::terminal::TerminalLine>{};
    const WorkspaceLayout layout = CurrentLayout(shell);
    const auto panel_layout = shell.ComputeBottomPanelLogLayout(layout, lines.size());
    return SDL_FPoint{
        .x = panel_layout.text_x +
             static_cast<float>(column) * std::max(1.0f, shell.text_renderer_.CharWidth()) + 1.0f,
        .y = panel_layout.text_y + static_cast<float>(row) * panel_layout.line_height +
             panel_layout.line_height * 0.5f,
    };
  }
  static SDL_FPoint BottomPanelTextOrigin(WorkspaceShell& shell) {
    const auto* terminal_tab = shell.ActiveTerminalTab();
    const std::size_t line_count =
        terminal_tab != nullptr ? terminal_tab->session.LineCount() : 0;
    const WorkspaceLayout layout = CurrentLayout(shell);
    const auto panel_layout = shell.ComputeBottomPanelLogLayout(layout, line_count);
    return SDL_FPoint{.x = panel_layout.text_x, .y = panel_layout.text_y};
  }
  static bool TerminalHasSelection(const WorkspaceShell& shell) {
    return shell.TerminalHasSelection();
  }
  static std::string ActiveTerminalSelectedText(WorkspaceShell& shell) {
    return shell.SelectedTerminalText();
  }
  static SDL_FRect ProjectSearchResultRect(WorkspaceShell& shell, std::size_t result_index) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    const auto line_map = shell.BuildProjectSearchLineMap();
    const auto list_layout =
        shell.ComputeProjectSearchSidebarListLayout(layout.sidebar, line_map.size());
    const int line_index = shell.ProjectSearchLineForResult(result_index);
    return ScrollableListRowRect(list_layout, line_index - list_layout.scroll_row);
  }
  static SDL_FRect ProjectTabRect(WorkspaceShell& shell, std::size_t index) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    for (const WorkspaceShell::VisibleStripTab& tab :
         shell.ComputeVisibleProjectTabs(layout.project_tab_strip)) {
      if (tab.index == index) {
        return tab.rect;
      }
    }
    return {};
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
  static std::string HoveredGitSidebarTooltipLabel(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return shell.HoveredGitSidebarTooltipLabel(layout.sidebar);
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
          row_rect, shell.git_sidebar_.entries[entry_index]);
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
                                                                shell.text_viewport_, pane);
  }
  static std::optional<SDL_FRect> ActiveEditorLineRangeRect(WorkspaceShell& shell,
                                                            std::size_t start_line,
                                                            std::size_t end_line) {
    return shell.CurrentEditorLineRangeRect(start_line, end_line);
  }
  static std::optional<SDL_FRect> ActiveEditorLineToBottomRect(WorkspaceShell& shell,
                                                               std::size_t start_line) {
    return shell.CurrentEditorLineToBottomRect(start_line);
  }
  static std::optional<SDL_FRect> ActiveCompareRowRangeRect(WorkspaceShell& shell,
                                                            std::size_t start_row,
                                                            std::size_t end_row) {
    return shell.CurrentCompareRowRangeRect(start_row, end_row);
  }
  static std::optional<SDL_FRect> ActiveCompareRowToBottomRect(WorkspaceShell& shell,
                                                               std::size_t start_row) {
    return shell.CurrentCompareRowToBottomRect(start_row);
  }
  static std::optional<SDL_FRect> ActiveMergeConflictRect(WorkspaceShell& shell,
                                                          std::size_t conflict_index) {
    return shell.CurrentMergeConflictRect(conflict_index);
  }
  static std::optional<SDL_FRect> ActiveMergeResultLineToBottomRect(WorkspaceShell& shell,
                                                                    std::size_t start_line) {
    return shell.CurrentMergeResultLineToBottomRect(start_line);
  }
  static float TextCharWidth(WorkspaceShell& shell) { return shell.text_renderer_.CharWidth(); }
  static float TextLineHeight(WorkspaceShell& shell) { return shell.text_renderer_.LineHeight(); }
  static SDL_HitTestResult WindowHitTest(WorkspaceShell& shell, float x, float y) {
    return shell.WindowHitTest(x, y);
  }
  static bool WindowDragRegionContains(WorkspaceShell& shell, float x, float y) {
    return shell.WindowDragRegionContains(x, y);
  }
  static std::optional<microide::editor::EditorBlameOverlay> ActiveEditorBlameOverlay(
      WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
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
      if (tab.index == shell.active_terminal_tab_index_) {
        return tab.rect;
      }
    }
    return {};
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
  static bool RestoreSessionState(WorkspaceShell& shell) {
    return WorkspaceShell::PersistenceCoordinator(shell).RestoreSessionState();
  }
  static void SaveSessionState(WorkspaceShell& shell) {
    WorkspaceShell::PersistenceCoordinator(shell).SaveSessionState();
  }
  static bool RestoreWorkspaceSession(WorkspaceShell& shell) {
    return WorkspaceShell::PersistenceCoordinator(shell).RestoreWorkspaceSession();
  }
  static void SaveWorkspaceSession(WorkspaceShell& shell) {
    WorkspaceShell::PersistenceCoordinator(shell).SaveWorkspaceSession();
  }
  static void RequestQuit(WorkspaceShell& shell) { shell.RequestQuit(); }
  static bool ConsumeQuitRequested(WorkspaceShell& shell) { return shell.ConsumeQuitRequested(); }
  static WorkspaceShell::WindowAction ConsumeWindowAction(WorkspaceShell& shell) {
    return shell.ConsumeWindowAction();
  }

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
    const WorkspaceLayout layout = CurrentLayout(shell);
    std::vector<std::string> labels;
    for (const auto& item : shell.ComputeVisibleMenuBarItems(layout.menu_bar)) {
      if (const auto* menu = shell.FindMenuSpec(item.id); menu != nullptr) {
        labels.emplace_back(menu->label);
      }
    }
    return labels;
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
      labels.push_back(shell.MenuItemLabel(items[visible_item.index]));
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
      if (shell.MenuItemLabel(items[visible_item.index]) == label) {
        return visible_item.rect;
      }
    }
    return std::nullopt;
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
    return shell.SidebarModeControlRect(layout.sidebar);
  }
  static SDL_FRect TreeSidebarCollapseButtonRect(WorkspaceShell& shell) {
    const WorkspaceLayout layout = CurrentLayout(shell);
    return shell.TreeSidebarCollapseButtonRect(layout.sidebar);
  }
  static std::string BreadcrumbLabel(WorkspaceShell& shell) { return shell.BreadcrumbLabel(); }
  static const std::vector<project::TreeEntry>& TreeEntries(const WorkspaceShell& shell) {
    return shell.directory_tree_.entries();
  }
  static std::filesystem::path SelectedTreePath(const WorkspaceShell& shell) {
    return shell.SelectedTreePath();
  }
  static int SidebarScrollRow(const WorkspaceShell& shell) {
    return shell.surface_.sidebar_scroll_row;
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
    return WorkspaceShell::CommandPromptCoordinator::PromptStatusText(shell);
  }
  static const std::string& ProjectSearchQuery(const WorkspaceShell& shell) {
    return shell.overlay_workflow_.project_search.query;
  }
  static bool OverlayVisible(const WorkspaceShell& shell) { return shell.surface_.overlay_visible; }
  static bool OverlayModeIsFileFinder(const WorkspaceShell& shell) {
    return shell.surface_.overlay_mode == WorkspaceShell::OverlayMode::FileFinder;
  }
  static bool TextInputSurfaceIsEditor(const WorkspaceShell& shell) {
    return shell.CurrentTextInputSurface() == WorkspaceShell::TextInputSurface::Editor;
  }
  static bool TextInputSurfaceIsFileFinder(const WorkspaceShell& shell) {
    return shell.CurrentTextInputSurface() == WorkspaceShell::TextInputSurface::FileFinder;
  }
  static bool TextInputSurfaceIsPromptInput(const WorkspaceShell& shell) {
    return shell.CurrentTextInputSurface() == WorkspaceShell::TextInputSurface::PromptInput;
  }
  static bool SidebarVisible(const WorkspaceShell& shell) { return shell.surface_.sidebar_visible; }
  static WorkspaceShell::SidebarMode SidebarMode(const WorkspaceShell& shell) {
    return shell.surface_.sidebar_mode;
  }
  static const std::string& SidebarViewId(const WorkspaceShell& shell) {
    return shell.surface_.sidebar_view_id;
  }
  static std::size_t ProblemsSidebarSelectedIndex(const WorkspaceShell& shell) {
    return shell.problems_sidebar_.selected_index;
  }
  static const std::vector<plugin::PluginHost::SidebarItem>& PluginSidebarItems(
      const WorkspaceShell& shell) {
    return shell.plugin_sidebar_.items;
  }
  static const std::string& PluginSidebarError(const WorkspaceShell& shell) {
    return shell.plugin_sidebar_.error;
  }
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
  static void ResetCaretBlink(WorkspaceShell& shell) { shell.ResetCaretBlink(); }
  static void SetCaretBlinkEpochMs(WorkspaceShell& shell, Uint64 epoch_ms) {
    shell.caret_blink_epoch_ms_ = epoch_ms;
  }
  static bool CaretVisibleNow(const WorkspaceShell& shell) { return shell.CaretVisibleNow(); }
  static std::optional<SDL_FRect> CurrentCaretDirtyRect(const WorkspaceShell& shell) {
    return shell.CurrentCaretDirtyRect();
  }
  static WorkspaceShell::RenderInvalidation ConsumePendingRenderInvalidation(WorkspaceShell& shell) {
    return shell.ConsumePendingRenderInvalidation();
  }
  static void MoveMergeSelection(WorkspaceShell& shell, int delta) { shell.MoveMergeSelection(delta); }
  static void MoveCompareSelection(WorkspaceShell& shell, int delta) { shell.MoveCompareSelection(delta); }
  static bool ShouldBlinkCaret(const WorkspaceShell& shell) { return shell.ShouldBlinkCaret(); }
  static bool FocusIsOverlay(const WorkspaceShell& shell) {
    return shell.surface_.focus == WorkspaceShell::FocusTarget::Overlay;
  }
  static bool MenuBarOpen(const WorkspaceShell& shell) { return shell.menu_state_.menu_bar_open; }
  static bool EditMenuOpen(const WorkspaceShell& shell) {
    return shell.menu_state_.menu_bar_open &&
           shell.menu_state_.active_menu_id == WorkspaceShell::MenuId::Edit;
  }
  static bool FileMenuOpen(const WorkspaceShell& shell) {
    return shell.menu_state_.menu_bar_open &&
           shell.menu_state_.active_menu_id == WorkspaceShell::MenuId::File;
  }
  static bool EditorTabContextMenuOpen(const WorkspaceShell& shell) {
    return shell.menu_state_.menu_bar_open &&
           shell.menu_state_.active_menu_id == WorkspaceShell::MenuId::EditorTabContext;
  }
  static bool SidebarModeMenuOpen(const WorkspaceShell& shell) {
    return shell.menu_state_.menu_bar_open &&
           shell.menu_state_.active_menu_id == WorkspaceShell::MenuId::SidebarMode;
  }
  static bool TerminalTabContextMenuOpen(const WorkspaceShell& shell) {
    return shell.menu_state_.menu_bar_open &&
           shell.menu_state_.active_menu_id == WorkspaceShell::MenuId::TerminalTabContext;
  }
  static bool TerminalContextMenuOpen(const WorkspaceShell& shell) {
    return shell.menu_state_.menu_bar_open &&
           shell.menu_state_.active_menu_id == WorkspaceShell::MenuId::TerminalContext;
  }
};

}  // namespace microide::workspace
