#include "workspace/services/TerminalPanelService.h"
#include "workspace/actions/WorkspaceActionAvailability.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

#include "workspace/coordinators/WorkspaceTabCoordinator.h"
#include "workspace/debug/DebugService.h"
#include "workspace/git/GitSidebarCommandCenter.h"
#include "workspace/lsp/LspFeatureFlags.h"
#include "workspace/SettingFlags.h"
#include "workspace/git/CompareTabReview.h"
#include "workspace/services/RemoteHostService.h"

namespace microide::workspace {

namespace {

bool SettingEnabled(const ActionAvailability::Operations& operations,
                    std::string_view id,
                    bool default_value) {
  if (!operations.get_setting_value) {
    return default_value;
  }
  return SettingFlagEnabled(operations.get_setting_value(id), default_value);
}

bool LspFeatureAvailable(const ActionAvailability::Operations& operations,
                         std::string_view feature_id) {
  if (!operations.get_setting_value) {
    return true;
  }
  return LspFeatureEnabled(operations.get_setting_value, feature_id);
}

}

ActionAvailability::ActionAvailability(const WorkspaceContext& context,
                                       const DebugService& debug, Operations operations)
    : context_(context), debug_(debug), operations_(std::move(operations)) {}

TreeContextTargetKind ActionAvailability::ActiveTreeTargetKind() const {
  return context_.menu_state.tree_context_menu.open ? context_.menu_state.tree_context_menu.target
                                                    : operations_.selected_tree_target_kind();
}

bool ActionAvailability::IsEnabled(ActionId id) const {
  const editor::TextViewport* active_viewport = operations_.active_navigable_viewport();
  const editor::TextViewport* active_editable_viewport = operations_.active_editable_viewport();
  const TerminalPaneState* active_terminal_pane =
      context_.current_project_state.active_terminal_pane();
  const TextInputSurface text_input_surface = operations_.current_text_input_surface();
  const bool active_single_line_selection = operations_.active_single_line_text_has_selection();
  switch (id) {
    case ActionId::CodeActions:
      return active_viewport != nullptr && operations_.active_code_actions_available() &&
             LspFeatureAvailable(operations_, "lsp.code_actions.enabled");
    case ActionId::FormatDocument:
      return active_editable_viewport != nullptr && !active_editable_viewport->path().empty() &&
             LspFeatureAvailable(operations_, "lsp.formatting.enabled");
    case ActionId::FormatJson:
      return active_editable_viewport != nullptr;
    case ActionId::RenameSymbol:
      return active_editable_viewport != nullptr && !active_editable_viewport->path().empty() &&
             LspFeatureAvailable(operations_, "lsp.rename.enabled");
    case ActionId::GoToTypeDefinition:
    case ActionId::GoToImplementation:
    case ActionId::GoToDeclaration:
      return active_editable_viewport != nullptr && !active_editable_viewport->path().empty() &&
             LspFeatureAvailable(operations_, "lsp.navigation.enabled");
    case ActionId::WorkspaceSymbol:
      return active_editable_viewport != nullptr && !active_editable_viewport->path().empty() &&
             LspFeatureAvailable(operations_, "lsp.workspace_symbol.enabled");
    case ActionId::Completion:
      return active_viewport != nullptr && operations_.active_completion_available() &&
             LspFeatureAvailable(operations_, "lsp.completion.enabled");
    case ActionId::InsertSnippet:
      return active_editable_viewport != nullptr &&
             SettingEnabled(operations_, "editor.snippets.enabled", true) &&
             !active_editable_viewport->path().empty();
    case ActionId::FindReferences:
      return active_viewport != nullptr && operations_.active_references_available() &&
             LspFeatureAvailable(operations_, "lsp.find_references.enabled");
    case ActionId::CallHierarchy:
      return active_editable_viewport != nullptr && !active_editable_viewport->path().empty() &&
             LspFeatureAvailable(operations_, "lsp.call_hierarchy.enabled");
    case ActionId::FindNext:
    case ActionId::FindPrevious:
      return active_viewport != nullptr;
    case ActionId::GoToNextProblemInFiles:
    case ActionId::GoToPreviousProblemInFiles:
      return !context_.current_project_state.root.empty();
    case ActionId::GoToSymbolInEditor:
    case ActionId::LanguageMode:
    case ActionId::GoToNextDiagnostic:
    case ActionId::GoToPreviousDiagnostic:
      return active_viewport != nullptr;
    case ActionId::GoToDefinition:
      return active_viewport != nullptr && operations_.active_definition_available() &&
             LspFeatureAvailable(operations_, "lsp.goto_definition.enabled");
    case ActionId::SignatureHelp:
      return active_viewport != nullptr &&
             LspFeatureAvailable(operations_, "lsp.signature_help.enabled");
      return active_viewport != nullptr;
    case ActionId::Colorscheme:
    case ActionId::ToggleFullscreen:
    case ActionId::ToggleColorTheme:
    case ActionId::Files:
    case ActionId::OpenCommandPalette:
    case ActionId::FocusNotifications:
    case ActionId::NotificationAction:
    case ActionId::BannerAction:
    case ActionId::ShowOutput:
    case ActionId::RemoteOpenFolder:
    case ActionId::RemoteResolveConflict:
    case ActionId::RemoteResolveAllConflicts:
    case ActionId::RemoteBlockedSave:
    case ActionId::RemoteAttachHostTerminals:
    case ActionId::RemoteCloseHostTerminals:
    case ActionId::RemoteCopyCommand:
    case ActionId::RemoteClearHostChanges:
    case ActionId::RemoteOpenOfferedSession:
    case ActionId::RemoteApplyHeldDeletes:
    case ActionId::OpenUrl:
    case ActionId::CancelSaveWait:
    case ActionId::OpenHelpAbout:
    case ActionId::OpenKeyboardShortcuts:
    case ActionId::OpenSettings:
    case ActionId::SetSetting:
    case ActionId::PluginsReload:
    case ActionId::ProjectOpen:
    case ActionId::Quit:
    case ActionId::SidebarClose:
    case ActionId::SidebarHide:
    case ActionId::SidebarShow:
    case ActionId::SidebarToggle:
      return true;
    case ActionId::DebugToggleEnabled:
      return true;
    case ActionId::StartDebugging:
    case ActionId::StopDebugging:
      return SettingEnabled(operations_, "debug.enabled", false);
    case ActionId::DebugStartOrContinue:
      return SettingEnabled(operations_, "debug.enabled", false) &&
             !(debug_.IsSessionActive() &&
               !(debug_.SessionState() == DebugSession::State::Stopped));
    case ActionId::DebugContinue:
    case ActionId::DebugStepOver:
    case ActionId::DebugStepIn:
    case ActionId::DebugStepOut:
      return SettingEnabled(operations_, "debug.enabled", false) &&
             debug_.IsSessionActive() &&
             debug_.SessionState() == DebugSession::State::Stopped;
    case ActionId::DebugReverseContinue:
    case ActionId::DebugStepBack:
      return SettingEnabled(operations_, "debug.enabled", false) &&
             debug_.IsSessionActive() &&
             debug_.SessionState() == DebugSession::State::Stopped &&
             debug_.SupportsStepBack();
    case ActionId::DebugPause:
      return SettingEnabled(operations_, "debug.enabled", false) &&
             debug_.IsSessionActive() &&
             !(debug_.SessionState() == DebugSession::State::Stopped);
    case ActionId::DebugRestart:
      return SettingEnabled(operations_, "debug.enabled", false) &&
             debug_.IsSessionActive();
    case ActionId::DebugSwitchSession:
      return SettingEnabled(operations_, "debug.enabled", false) &&
             debug_.SessionCount() > 1;
    case ActionId::DebugStopAllSessions:
      return SettingEnabled(operations_, "debug.enabled", false) &&
             debug_.IsSessionActive();
    case ActionId::DebugConsoleRepl:
      return SettingEnabled(operations_, "debug.enabled", false) &&
             debug_.IsSessionActive();
    case ActionId::PickLaunchConfig:
      return SettingEnabled(operations_, "debug.enabled", false);
    case ActionId::DebugBreakpointEditCondition:
    case ActionId::DebugBreakpointEditHitCondition:
    case ActionId::DebugBreakpointEditLogMessage:
    case ActionId::DebugBreakpointClearCondition:
    case ActionId::DebugBreakpointToggleEnabled:
    case ActionId::DebugBreakpointRemove:
      return SettingEnabled(operations_, "debug.enabled", false);
    case ActionId::GitAcceptCurrent:
    case ActionId::GitAcceptIncoming: {
      const ProjectWorkspaceState& state = context_.current_project_state;
      const auto& git = state.sidebar.git;
      const EditorGroup& group = state.focused_group();
      return (group.has_active_tab() && group.active_tab().merge.has_value()) ||
             (git.selected_index < git.entries.size() && git.entries[git.selected_index].conflicted);
    }
    case ActionId::GitOpenChanges:
    case ActionId::GitOpenFileEntry:
    case ActionId::GitStageToggleEntry:
    case ActionId::GitDiscardEntry: {
      const auto& git = context_.current_project_state.sidebar.git;
      if (git.selected_index >= git.entries.size()) {
        return false;
      }
      const GitSidebarActionAvailability availability = GitSidebarActionAvailabilityForEntry(
          git.entries[git.selected_index], git.repo_available, git.supports_mutations);
      if (id == ActionId::GitOpenChanges) {
        return availability.default_view;
      }
      if (id == ActionId::GitOpenFileEntry) {
        return availability.open_file;
      }
      if (id == ActionId::GitStageToggleEntry) {
        return availability.stage || availability.unstage;
      }
      return availability.discard;
    }
    case ActionId::BreakpointToggle:
      return SettingEnabled(operations_, "debug.enabled", false) &&
             active_editable_viewport != nullptr && !active_editable_viewport->path().empty();
    case ActionId::BreakpointSet:
    case ActionId::BreakpointRemove:
    case ActionId::BreakpointEnable:
    case ActionId::BreakpointDisable:
    case ActionId::BreakpointCondition:
    case ActionId::BreakpointHitCondition:
    case ActionId::BreakpointLogMessage:
    case ActionId::BreakpointClear:
    case ActionId::BreakpointFunctionAdd:
    case ActionId::BreakpointFunctionRemove:
    case ActionId::BreakpointFunctionToggle:
    case ActionId::BreakpointFunctionCondition:
    case ActionId::BreakpointExceptionCondition:
    case ActionId::DebugLaunch:
    case ActionId::DebugRun:
      return SettingEnabled(operations_, "debug.enabled", false);
    case ActionId::DebugPaneToggle:
    case ActionId::DebugPaneShowCallStack:
    case ActionId::DebugPaneShowVariables:
    case ActionId::DebugPaneShowWatch:
    case ActionId::DebugPaneShowBreakpoints:
    case ActionId::DebugShowOutput:
      return SettingEnabled(operations_, "debug.enabled", false);
    case ActionId::DebugCopyValue:
    case ActionId::DebugAddToWatch: {
      if (!SettingEnabled(operations_, "debug.enabled", false)) {
        return false;
      }
      const auto& state = context_.current_project_state;
      const auto row_ready = [&](const auto& model) {
        const auto& rows = model.Rows();
        const std::size_t index = model.SelectedRow();
        if (index >= rows.size() || rows[index].is_placeholder || rows[index].is_show_more) {
          return false;
        }
        return id == ActionId::DebugCopyValue ? !rows[index].display_value.empty()
                                              : !rows[index].display_name.empty();
      };
      if (state.debug_pane.mode == DebugPaneMode::Variables) {
        return row_ready(state.debug_variables);
      }
      if (state.debug_pane.mode == DebugPaneMode::Watch) {
        return row_ready(state.debug_watch);
      }
      return false;
    }
    case ActionId::CloseActiveTab:
      return !context_.current_project_state.focused_group().open_tabs.empty();
    case ActionId::CloseAllTabs:
      return !context_.current_project_state.focused_group().open_tabs.empty();
    case ActionId::RevertFile:
      return !context_.current_project_state.root.empty() && active_editable_viewport != nullptr &&
             !active_editable_viewport->path().empty() && active_editable_viewport->dirty();
    case ActionId::SaveAll:
      for (const EditorGroup& group : context_.current_project_state.editor_groups) {
        for (const TabEntry& tab : group.open_tabs) {
          if (TabCoordinator::TabStateIsDirty(tab)) {
            return true;
          }
        }
      }
      return false;
    case ActionId::CloseOtherTabs:
      return context_.current_project_state.focused_group().open_tabs.size() > 1;
    case ActionId::CloseTabsToRight:
      return !context_.current_project_state.focused_group().open_tabs.empty() &&
             context_.current_project_state.focused_group().active_tab_index + 1 <
                 context_.current_project_state.focused_group().open_tabs.size();
    case ActionId::CloseTabsToLeft:
      return !context_.current_project_state.focused_group().open_tabs.empty() &&
             context_.current_project_state.focused_group().active_tab_index > 0;
    case ActionId::CompareHead:
    case ActionId::OpenSelectedTreeItem:
    case ActionId::OpenSelectedTreeItemInNewTab:
      return !context_.current_project_state.root.empty() &&
             ActiveTreeTargetKind() == TreeContextTargetKind::File;
    case ActionId::CompareFiles:
      return true;
    case ActionId::SelectForCompare:
    case ActionId::CompareWithClipboard:
      return (active_viewport != nullptr && !active_viewport->is_placeholder()) ||
             ActiveTreeTargetKind() == TreeContextTargetKind::File;
    case ActionId::CompareWithSelected:
      return context_.current_project_state.compare_selection.has_value() &&
             (active_viewport != nullptr ||
              ActiveTreeTargetKind() == TreeContextTargetKind::File);
    case ActionId::CreateDirectory:
    case ActionId::CreateFile: {
      if (context_.current_project_state.root.empty()) {
        return false;
      }
      const TreeContextTargetKind target = ActiveTreeTargetKind();
      return target == TreeContextTargetKind::Directory ||
             target == TreeContextTargetKind::Root ||
             target == TreeContextTargetKind::Background;
    }
    case ActionId::DeletePath:
    case ActionId::TreeCut:
    case ActionId::TreeCopy: {
      const TreeContextTargetKind target = ActiveTreeTargetKind();
      return !context_.current_project_state.root.empty() &&
             (target == TreeContextTargetKind::File || target == TreeContextTargetKind::Directory);
    }
    case ActionId::TreePaste:
      return !context_.current_project_state.root.empty() &&
             context_.current_project_state.sidebar.tree_clipboard.has_value();
    case ActionId::RenamePath: {
      if (context_.current_project_state.root.empty()) {
        return false;
      }
      const TreeContextTargetKind target = ActiveTreeTargetKind();
      return target == TreeContextTargetKind::File ||
             target == TreeContextTargetKind::Directory;
    }
    case ActionId::Compare:
    case ActionId::GitRefresh:
    case ActionId::GitSwitchBranch:
    case ActionId::GitWorktrees:
    case ActionId::GitCommits:
    case ActionId::GitCreateBranch:
    case ActionId::GitFetch:
    case ActionId::GitPull:
    case ActionId::GitPush:
    case ActionId::GitPublishBranch:
    case ActionId::GitSync:
    case ActionId::GitStash:
    case ActionId::GitStashPop:
    case ActionId::GitCommit:
    case ActionId::Merge:
    case ActionId::ReviewConflicts:
    case ActionId::ReviewBranch:
    case ActionId::ReviewCommit:
    case ActionId::ReviewCheckpoint:
    case ActionId::ReviewCheckpointMark:
    case ActionId::Open:
    case ActionId::ProjectClose:
    case ActionId::ProjectCopyAbsolutePath:
    case ActionId::ProjectSearch:
    case ActionId::ProjectReplace:
    case ActionId::ProjectSearchRefresh:
    case ActionId::ProjectSearchToggleCountAll:
    case ActionId::ProjectSearchToggleDetails:
    case ActionId::BrowseFolder:
    case ActionId::Tab:
    case ActionId::Term:
    case ActionId::TerminalFind:
    case ActionId::Tree:
    case ActionId::TreeRefresh:
      return !context_.current_project_state.root.empty();
    case ActionId::Reveal:
      return !context_.current_project_state.root.empty();
    case ActionId::RemoteOpenLocalTerminal:
      return context_.current_project_state.id.IsRemote();
    case ActionId::RemoteShowStatus:
    case ActionId::RemoteEndHostProcess:
    case ActionId::RemoteShowLog:
    case ActionId::RemoteStopServer:
    case ActionId::RemoteDisconnect:
    case ActionId::RemoteReconnect:
    case ActionId::RemoteForwardPort: {
      const RemoteHostService* hosts = operations_.remote_hosts ? operations_.remote_hosts() : nullptr;
      return hosts != nullptr && hosts->HasOpenProject();
    }
    case ActionId::RemoteUnforwardPort: {
      const RemoteHostService* hosts = operations_.remote_hosts ? operations_.remote_hosts() : nullptr;
      return hosts != nullptr && hosts->HasForwardedPort();
    }
    case ActionId::RemoteFileHistory:
      return context_.current_project_state.id.IsRemote() && !operations_.active_tab_path().empty();
    case ActionId::RemoteShowHostChanges:
    case ActionId::RemoteShowConflicts:
      return context_.current_project_state.id.IsRemote();
    case ActionId::RemoteRemoveMirror: {
      const RemoteHostService* hosts = operations_.remote_hosts ? operations_.remote_hosts() : nullptr;
      return hosts != nullptr ? hosts->HasRemovableMirror()
                              : operations_.has_remote_recent && operations_.has_remote_recent();
    }
    case ActionId::TabToGroup:
      return context_.current_project_state.editor_groups.size() > 1 &&
             !context_.current_project_state.focused_group().open_tabs.empty();
    case ActionId::TermClose:
    case ActionId::TerminalSelectAll:
    case ActionId::TerminalSplit:
    case ActionId::TerminalFocusPaneLeft:
    case ActionId::TerminalFocusPaneRight:
      return active_terminal_pane != nullptr;
    case ActionId::TerminalNext:
    case ActionId::TerminalPrev:
      return context_.current_project_state.terminal_tabs.size() > 1;
    case ActionId::TerminalNextAlert: {
      const auto rang = [](const ProjectWorkspaceState& state) {
        return std::ranges::any_of(state.terminal_tabs,
                                   [](const auto& tab) { return tab != nullptr && tab->has_unseen_bell; });
      };
      const auto& catalog = context_.project_catalog;
      for (std::size_t i = 0; i < catalog.entries.size(); ++i) {
        if (i != catalog.active_index && catalog.entries[i] != nullptr && rang(*catalog.entries[i])) {
          return true;
        }
      }
      return rang(context_.current_project_state);
    }
    case ActionId::TerminalRelaunch:
      return active_terminal_pane != nullptr && !active_terminal_pane->session.running() &&
             !active_terminal_pane->session.host_pending() &&
             !active_terminal_pane->launch_working_directory.empty();
    case ActionId::TerminalToggle:
    case ActionId::PanelToggle:
    case ActionId::PanelToggleMaximized:
      return !context_.current_project_state.root.empty();
    case ActionId::CopyLastTerminalCommand:
      return TerminalPanelService::HasLastCommand(context_.current_project_state);
    case ActionId::CopySelectionWithContext:
      return active_viewport != nullptr;
    case ActionId::CopyReference:
      return active_viewport != nullptr && !active_viewport->path().empty();
    case ActionId::TerminalInsertReference:
      return active_viewport != nullptr && !active_viewport->path().empty() &&
             context_.current_project_state.active_terminal_pane() != nullptr;
    case ActionId::CopySelection:
      return active_viewport != nullptr ||
             active_single_line_selection ||
             (context_.current_project_state.surface.focus == FocusTarget::Panel &&
              TerminalPanelService::HasSelection(context_.current_project_state));
    case ActionId::CutSelection:
      return active_editable_viewport != nullptr || active_single_line_selection;
    case ActionId::Redo:
    case ActionId::Undo:
    case ActionId::ShowHover:
      return active_editable_viewport != nullptr;
    case ActionId::SelectAll:
      return active_viewport != nullptr || IsSingleLineTextInputSurface(text_input_surface);
    case ActionId::PressKey:
    case ActionId::Pointer:
    case ActionId::Compose:
      return true;
    case ActionId::InsertText:
      [[fallthrough]];
    case ActionId::PasteClipboard:
      return active_editable_viewport != nullptr ||
             IsSingleLineTextInputSurface(text_input_surface) ||
             (context_.current_project_state.surface.focus == FocusTarget::Panel &&
              active_terminal_pane != nullptr);
    case ActionId::Goto:
    case ActionId::FocusEditorGroup:
      return operations_.active_tab_is_editor() || operations_.editor_group_count() > 1;
    case ActionId::NavigateBack:
      return !context_.current_project_state.root.empty() &&
             context_.current_project_state.navigation_history.CanGoBack();
    case ActionId::NavigateForward:
      return !context_.current_project_state.root.empty() &&
             context_.current_project_state.navigation_history.CanGoForward();
    case ActionId::SaveAs:
      return !context_.current_project_state.root.empty() && active_editable_viewport != nullptr;
    case ActionId::ReopenClosedEditor:
      return !context_.current_project_state.root.empty() &&
             !context_.current_project_state.recently_closed_editors.empty();
    case ActionId::Jump:
    case ActionId::ReplaceInBuffer:
    case ActionId::Reopen:
    case ActionId::Search:
      return operations_.active_tab_is_editor();
    case ActionId::SplitEditorRight:
    case ActionId::SplitEditorDown:
      return operations_.active_tab_is_editor() &&
             operations_.editor_group_count() < kMaxEditorGroups;
    case ActionId::FocusOtherGroup:
    case ActionId::FocusEditorGroupLeft:
    case ActionId::FocusEditorGroupRight:
    case ActionId::FocusEditorGroupUp:
    case ActionId::FocusEditorGroupDown:
    case ActionId::MoveEditorGroupLeft:
    case ActionId::MoveEditorGroupRight:
    case ActionId::MoveEditorGroupUp:
    case ActionId::MoveEditorGroupDown:
    case ActionId::CloseGroup:
      return operations_.editor_group_count() > 1;
    case ActionId::Save:
      return operations_.active_tab_is_editor() || operations_.active_tab_is_merge() ||
             (operations_.active_tab_is_compare() &&
              operations_.active_compare_tab() != nullptr &&
              operations_.active_compare_tab()->right_editable);
    case ActionId::Focus:
    case ActionId::FocusNextPart:
    case ActionId::FocusPreviousPart:
      return true;
    case ActionId::IndentWidth:
      return true;
    case ActionId::CopyAbsolutePath:
    case ActionId::ShowInFileExplorer:
      return !operations_.resolve_tree_action_path(ActionSource::ContextMenu).empty() ||
             !operations_.active_tab_path().empty();
    case ActionId::RevealInFileTree:
      return !context_.current_project_state.root.empty() &&
             ((context_.menu_state.tree_context_menu.open && !context_.menu_state.tree_context_menu.path.empty()) ||
              !operations_.active_tab_path().empty());
    case ActionId::CopyRelativePath: {
      const std::filesystem::path path =
          !operations_.resolve_tree_action_path(ActionSource::ContextMenu).empty()
              ? operations_.resolve_tree_action_path(ActionSource::ContextMenu)
              : operations_.active_tab_path();
      return !context_.current_project_state.root.empty() && !path.empty() &&
             path != context_.current_project_state.root;
    }
    case ActionId::SidebarWidth:
    case ActionId::SoftTabs:
    case ActionId::Wrap:
    case ActionId::TabSize:
    case ActionId::TestsDiscover:
    case ActionId::UiScale:
    case ActionId::ToggleLayoutMode:
    case ActionId::ToggleStatusBar:
      return true;
    case ActionId::ProjectNext:
    case ActionId::ProjectPrev:
      return !context_.current_project_state.root.empty() &&
             context_.project_catalog.entries.size() > 1;
    case ActionId::TabMove:
    case ActionId::TabSwitch:
      return !context_.current_project_state.root.empty() &&
             !context_.current_project_state.focused_group().open_tabs.empty();
    case ActionId::TestsRun:
      return !context_.current_project_state.sidebar.tests.entries.empty();
    case ActionId::LineEnding:
    case ActionId::Indentation:
      return active_editable_viewport != nullptr;
    case ActionId::ToggleLineComment:
    case ActionId::ToggleBlockComment:
      return active_editable_viewport != nullptr &&
             SettingEnabled(operations_, ShapingActionGateSettingKey(id), true);
    case ActionId::ColumnSelectUp:
    case ActionId::ColumnSelectDown:
    case ActionId::ColumnSelectLeft:
    case ActionId::ColumnSelectRight:
      return active_editable_viewport != nullptr;
    case ActionId::MoveLineUp:
    case ActionId::MoveLineDown:
    case ActionId::DuplicateLine:
    case ActionId::CopyLineUp:
    case ActionId::InsertLineBelow:
    case ActionId::InsertLineAbove:
    case ActionId::DeleteLine:
    case ActionId::IndentLines:
    case ActionId::OutdentLines:
    case ActionId::JoinLines:
      return active_editable_viewport != nullptr &&
             SettingEnabled(operations_, ShapingActionGateSettingKey(id), true);
    case ActionId::ExpandSelection:
    case ActionId::ShrinkSelection:
      return active_viewport != nullptr;
    case ActionId::SortLinesAscending:
    case ActionId::SortLinesDescending:
      return active_editable_viewport != nullptr &&
             SettingEnabled(operations_, ShapingActionGateSettingKey(id), true);
    case ActionId::AddCursorAtNextMatch:
    case ActionId::AddCursorAtAllMatches:
      return active_editable_viewport != nullptr &&
             SettingEnabled(operations_, "editor.multicursor.add_at_match.enabled", true);
    case ActionId::AddCursorAbove:
    case ActionId::AddCursorBelow:
    case ActionId::AddCursorsToLineEnds:
      return active_editable_viewport != nullptr;
    case ActionId::JumpToMatchingBracket:
      return active_viewport != nullptr;
    case ActionId::CompareToggleIgnoreWhitespace:
    case ActionId::CompareToggleShowWhitespace:
    case ActionId::CompareCopyHunkPatch:
    case ActionId::CompareCopyFilePatch:
    case ActionId::CompareCopyPath:
      return operations_.active_compare_tab != nullptr && operations_.active_compare_tab() != nullptr;
    case ActionId::MergeCopyIncoming:
    case ActionId::MergeCopyCurrent:
    case ActionId::MergeResetConflict: {
      const MergeTabState* tab =
          operations_.active_merge_tab != nullptr ? operations_.active_merge_tab() : nullptr;
      return tab != nullptr && !tab->conflicts.empty();
    }
    case ActionId::CompareNextReviewFile:
    case ActionId::ComparePreviousReviewFile: {
      const CompareTabState* tab =
          operations_.active_compare_tab != nullptr ? operations_.active_compare_tab() : nullptr;
      return tab != nullptr && tab->review_files.size() > 1;
    }
    case ActionId::CompareNextChange:
    case ActionId::ComparePreviousChange: {
      const CompareTabState* tab =
          operations_.active_compare_tab != nullptr ? operations_.active_compare_tab() : nullptr;
      return tab != nullptr && !tab->model.hunks.empty();
    }
    case ActionId::MarkBranchFileReviewed:
    case ActionId::UnmarkBranchFileReviewed:
    case ActionId::MarkBranchHunkReviewed:
    case ActionId::UnmarkBranchHunkReviewed:
    case ActionId::EditBranchReviewNote: {
      const CompareTabState* tab =
          operations_.active_compare_tab != nullptr ? operations_.active_compare_tab() : nullptr;
      return tab != nullptr && CompareTabTakesReviewMarks(*tab);
    }
    case ActionId::CopyBranchReviewNotes:
    case ActionId::TerminalInsertBranchReviewNotes: {
      if (id == ActionId::TerminalInsertBranchReviewNotes &&
          context_.current_project_state.active_terminal_pane() == nullptr) {
        return false;
      }
      return operations_.review_comment_count != nullptr &&
             operations_.review_comment_count(ReviewCommentMenuScope(context_.menu_state.tree_context_menu)) > 0;
    }
    case ActionId::ClearBranchReviewState:
    case ActionId::GitStageSelectedRanges:
    case ActionId::GitUnstageSelectedRanges:
    case ActionId::GitRevertSelectedRanges:
      return operations_.active_tab_is_compare();
    case ActionId::Fold:
    case ActionId::Unfold:
    case ActionId::ToggleFoldAtCursor:
    case ActionId::FoldAll:
    case ActionId::UnfoldAll:
      return active_viewport != nullptr &&
             SettingEnabled(operations_, "editor.fold.enabled", true);
    case ActionId::ToggleEditorFolding:
    case ActionId::ToggleEditorStickyScroll:
    case ActionId::ToggleEditorIndentGuides:
    case ActionId::ToggleEditorRenderWhitespace:
    case ActionId::ToggleEditorBracketMatchHighlight:
    case ActionId::ToggleEditorAutoClosePairs:
    case ActionId::ToggleEditorSurround:
    case ActionId::ToggleEditorSmartIndent:
    case ActionId::ToggleEditorToggleComment:
    case ActionId::ToggleEditorLineOps:
    case ActionId::ToggleEditorSortLines:
    case ActionId::ToggleEditorAddCursorAtMatch:
    case ActionId::ToggleEditorOccurrencesHighlight:
    case ActionId::ToggleEditorSearchCaseSensitive:
    case ActionId::ToggleEditorSnippets:
    case ActionId::ToggleEditorSaveTrim:
    case ActionId::ToggleEditorSaveEnsureNewline:
    case ActionId::ToggleEditorAutoDetectIndent:
      return true;
  }

  return true;
}

std::string_view ActionAvailability::PossibleDisabledReason(ActionId id) {
  switch (id) {
    case ActionId::RemoteShowStatus:
    case ActionId::RemoteEndHostProcess:
    case ActionId::RemoteShowLog:
    case ActionId::RemoteStopServer:
    case ActionId::RemoteDisconnect:
    case ActionId::RemoteReconnect:
    case ActionId::RemoteForwardPort:
      return "connect to a host first";
    case ActionId::RemoteUnforwardPort:
      return "no port is forwarded";
    case ActionId::RemoteFileHistory:
      return "open a file of a remote project";
    case ActionId::RemoteShowHostChanges:
    case ActionId::RemoteShowConflicts:
      return "not a remote project";
    case ActionId::RemoteRemoveMirror:
      return "no closed remote project";
    default:
      return {};
  }
}

std::string_view ActionAvailability::DisabledReason(ActionId id) const {
  const std::string_view reason = PossibleDisabledReason(id);
  return reason.empty() || IsEnabled(id) ? std::string_view() : reason;
}

}
