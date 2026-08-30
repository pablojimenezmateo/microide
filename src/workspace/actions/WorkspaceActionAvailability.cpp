#include "workspace/actions/WorkspaceActionAvailability.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

#include "workspace/git/GitSidebarCommandCenter.h"
#include "workspace/lsp/LspFeatureFlags.h"
#include "workspace/SettingFlags.h"

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

// An LSP feature is available only when both the master switch (`lsp.enabled`) and
// the feature's own toggle are on. Absent getter (headless setups) => available.
bool LspFeatureAvailable(const ActionAvailability::Operations& operations,
                         std::string_view feature_id) {
  if (!operations.get_setting_value) {
    return true;
  }
  return LspFeatureEnabled(operations.get_setting_value, feature_id);
}

}  // namespace

ActionAvailability::ActionAvailability(const WorkspaceContext& context, Operations operations)
    : context_(context), operations_(std::move(operations)) {}

TreeContextTargetKind ActionAvailability::ActiveTreeTargetKind() const {
  return context_.menu_state.tree_context_menu.open ? context_.menu_state.tree_context_menu.target
                                                    : operations_.selected_tree_target_kind();
}

bool ActionAvailability::IsEnabled(ActionId id) const {
  const editor::TextViewport* active_viewport = operations_.active_navigable_viewport();
  const editor::TextViewport* active_editable_viewport = operations_.active_editable_viewport();
  const TerminalTabState* active_terminal_tab = operations_.active_terminal_tab();
  const TextInputSurface text_input_surface = operations_.current_text_input_surface();
  const bool active_single_line_selection = operations_.active_single_line_text_has_selection();
  switch (id) {
    case ActionId::CodeActions:
      return active_viewport != nullptr && operations_.active_code_actions_available() &&
             LspFeatureAvailable(operations_, "lsp.code_actions.enabled");
    case ActionId::FormatDocument:
      // LSP-only action; offered whenever a saved editable buffer is active and the
      // feature is enabled. The executor reports "no language server" on invocation.
      return active_editable_viewport != nullptr && !active_editable_viewport->path().empty() &&
             LspFeatureAvailable(operations_, "lsp.formatting.enabled");
    case ActionId::FormatJson:
      // Self-contained (no language server); available for any editable buffer,
      // including untitled ones. A path argument can still target another file.
      return active_editable_viewport != nullptr;
    case ActionId::RenameSymbol:
      return active_editable_viewport != nullptr && !active_editable_viewport->path().empty() &&
             LspFeatureAvailable(operations_, "lsp.rename.enabled");
    case ActionId::GoToTypeDefinition:
    case ActionId::GoToImplementation:
    case ActionId::GoToDeclaration:
      // Extended LSP navigation; offered when a saved editable buffer is active. The
      // executor reports "no language server" as feedback on invocation.
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
      // The last LSP action with no availability rule at all: it fell through the
      // switch to the trailing `return true`, so it read as enabled with no editor
      // open. Gated like RenameSymbol, which is what its executor already requires.
      return active_editable_viewport != nullptr && !active_editable_viewport->path().empty() &&
             LspFeatureAvailable(operations_, "lsp.call_hierarchy.enabled");
    case ActionId::GoToDefinition:
      return active_viewport != nullptr && operations_.active_definition_available() &&
             LspFeatureAvailable(operations_, "lsp.goto_definition.enabled");
    case ActionId::SignatureHelp:
      // Offered when an editor is focused and the feature is enabled; the provider
      // query (and its graceful "no signature help available" reject) runs on
      // invocation.
      return active_viewport != nullptr &&
             LspFeatureAvailable(operations_, "lsp.signature_help.enabled");
      return active_viewport != nullptr;
    case ActionId::Colorscheme:
    case ActionId::ToggleFullscreen:
    case ActionId::ToggleColorTheme:
    case ActionId::Files:
    case ActionId::OpenCommandPalette:
    case ActionId::OpenHelpAbout:
    case ActionId::OpenKeyboardShortcuts:
    case ActionId::OpenSettings:
    case ActionId::SetSetting:  // deterministic setting write; never gated
    case ActionId::PluginsReload:
    case ActionId::ProjectOpen:
    case ActionId::Quit:
    case ActionId::SidebarClose:
    case ActionId::SidebarHide:
    case ActionId::SidebarShow:
    case ActionId::SidebarToggle:
      return true;
    case ActionId::DebugToggleEnabled:
      // The master enable/disable toggle is itself never gated — it must work
      // precisely when the debugger is off so the user can turn it on.
      return true;
    case ActionId::StartDebugging:
    case ActionId::StopDebugging:
      // Gated on the master debugger toggle; the executor reports adapter/
      // session preconditions as command feedback.
      return SettingEnabled(operations_, "debug.enabled", false);
    case ActionId::DebugContinue:
    case ActionId::DebugStepOver:
    case ActionId::DebugStepIn:
    case ActionId::DebugStepOut:
      // Resume/step are valid only while the session is paused.
      return SettingEnabled(operations_, "debug.enabled", false) &&
             operations_.debug_session_active && operations_.debug_session_active() &&
             operations_.debug_session_stopped && operations_.debug_session_stopped();
    case ActionId::DebugReverseContinue:
    case ActionId::DebugStepBack:
      // Reverse execution: paused session AND a recording adapter that advertises
      // `supportsStepBack` (so these stay hidden/greyed for ordinary adapters).
      return SettingEnabled(operations_, "debug.enabled", false) &&
             operations_.debug_session_active && operations_.debug_session_active() &&
             operations_.debug_session_stopped && operations_.debug_session_stopped() &&
             operations_.debug_supports_reverse && operations_.debug_supports_reverse();
    case ActionId::DebugPause:
      // Pause is valid only while the session is running (active, not stopped).
      return SettingEnabled(operations_, "debug.enabled", false) &&
             operations_.debug_session_active && operations_.debug_session_active() &&
             !(operations_.debug_session_stopped && operations_.debug_session_stopped());
    case ActionId::DebugRestart:
      // Restart is valid whenever a session is active (running or stopped).
      return SettingEnabled(operations_, "debug.enabled", false) &&
             operations_.debug_session_active && operations_.debug_session_active();
    case ActionId::DebugSwitchSession:
      // Switching only makes sense with more than one live session.
      return SettingEnabled(operations_, "debug.enabled", false) &&
             operations_.debug_session_count && operations_.debug_session_count() > 1;
    case ActionId::DebugStopAllSessions:
      // Stopping everything only makes sense with at least one live session.
      return SettingEnabled(operations_, "debug.enabled", false) &&
             operations_.debug_session_active && operations_.debug_session_active();
    case ActionId::DebugConsoleRepl:
      // REPL evaluation needs a live session (frame 0 when running).
      return SettingEnabled(operations_, "debug.enabled", false) &&
             operations_.debug_session_active && operations_.debug_session_active();
    case ActionId::PickLaunchConfig:
      // The picker is useful whenever the debugger is enabled (it can launch).
      return SettingEnabled(operations_, "debug.enabled", false);
    case ActionId::DebugBreakpointEditCondition:
    case ActionId::DebugBreakpointEditHitCondition:
    case ActionId::DebugBreakpointEditLogMessage:
    case ActionId::DebugBreakpointClearCondition:
    case ActionId::DebugBreakpointToggleEnabled:
    case ActionId::DebugBreakpointRemove:
      // Breakpoint-gutter context-menu items; the menu only opens when the
      // debugger is enabled, and editing works with or without a live session.
      return SettingEnabled(operations_, "debug.enabled", false);
    case ActionId::GitOpenChanges:
    case ActionId::GitStageToggleEntry:
    case ActionId::GitDiscardEntry: {
      // Git sidebar entry context-menu items; gate on the selected entry's
      // action availability (e.g. Discard is disabled for the Outgoing section).
      const auto& git = context_.current_project_state.sidebar.git;
      if (git.selected_index >= git.entries.size()) {
        return false;
      }
      const GitSidebarActionAvailability availability = GitSidebarActionAvailabilityForEntry(
          git.entries[git.selected_index], git.repo_available, git.supports_mutations);
      if (id == ActionId::GitOpenChanges) {
        return availability.default_view;
      }
      if (id == ActionId::GitStageToggleEntry) {
        return availability.stage || availability.unstage;
      }
      return availability.discard;
    }
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
      // Headless breakpoint/launch control; valid whenever the debugger is on.
      return SettingEnabled(operations_, "debug.enabled", false);
    case ActionId::DebugPaneToggle:
    case ActionId::DebugPaneShowCallStack:
    case ActionId::DebugPaneShowVariables:
    case ActionId::DebugPaneShowWatch:
    case ActionId::DebugPaneShowBreakpoints:
    case ActionId::DebugShowOutput:
      // Right-side debug pane + bottom-panel debug output: available whenever the
      // debugger is enabled (the breakpoints/watch surfaces are useful before a
      // session starts; Show Output no-ops gracefully without a live session).
      return SettingEnabled(operations_, "debug.enabled", false);
    case ActionId::DebugCopyValue:
    case ActionId::DebugAddToWatch: {
      // Row menu items on the pane's two value surfaces. Enabled only when the
      // selected row actually carries the thing the item copies/watches, so the
      // menu never offers a no-op on a scope header or a "show more…" row.
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
      // Diffs two arbitrary paths (or files picked interactively); works even
      // with no project open, so it is always available.
      return true;
    case ActionId::SelectForCompare:
    case ActionId::CompareWithClipboard:
      // Need a resolvable "current side": an active editor buffer, or a file
      // targeted in the tree.
      return active_viewport != nullptr ||
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
    case ActionId::RenamePath: {
      if (context_.current_project_state.root.empty()) {
        return false;
      }
      const TreeContextTargetKind target = ActiveTreeTargetKind();
      return target == TreeContextTargetKind::File ||
             target == TreeContextTargetKind::Directory;
    }
    case ActionId::Compare:
    case ActionId::Find:
    case ActionId::GitRefresh:
    // Git write actions gate on "a project is open", exactly as GitRefresh does;
    // the executor reports "not a git repository" precisely on invocation rather
    // than the command silently vanishing from the palette in a non-git project.
    case ActionId::GitSwitchBranch:
    case ActionId::GitCreateBranch:
    case ActionId::GitFetch:
    case ActionId::GitPull:
    case ActionId::GitPush:
    case ActionId::GitPublishBranch:
    case ActionId::GitSync:
    case ActionId::GitStash:
    case ActionId::GitStashPop:
    case ActionId::Merge:
    case ActionId::ReviewConflicts:
    case ActionId::ReviewBranch:
    case ActionId::ReviewCommit:
    case ActionId::Open:
    case ActionId::ProjectClose:
    case ActionId::ProjectCopyAbsolutePath:
    case ActionId::ProjectSearch:
    case ActionId::Tab:
    case ActionId::Term:
    case ActionId::TerminalFind:
    case ActionId::Tree:
    case ActionId::TreeRefresh:
      return !context_.current_project_state.root.empty();
    case ActionId::TermClose:
      return active_terminal_tab != nullptr;
    case ActionId::CopyLastTerminalCommand:
      return active_terminal_tab != nullptr && operations_.has_last_terminal_command();
    case ActionId::CopySelectionWithContext:
      return active_viewport != nullptr;
    case ActionId::CopySelection:
      return active_viewport != nullptr ||
             active_single_line_selection ||
             (context_.current_project_state.surface.focus == FocusTarget::Panel &&
              operations_.terminal_has_selection());
    case ActionId::CutSelection:
      return active_editable_viewport != nullptr || active_single_line_selection;
    case ActionId::Redo:
    case ActionId::Undo:
      return active_editable_viewport != nullptr;
    case ActionId::SelectAll:
      return active_viewport != nullptr || IsSingleLineTextInputSurface(text_input_surface);
    case ActionId::InsertText:
      // `type <text>` targets the active editable viewport or a text-input
      // surface, same as a paste.
      [[fallthrough]];
    case ActionId::PasteClipboard:
      return active_editable_viewport != nullptr ||
             IsSingleLineTextInputSurface(text_input_surface) ||
             (context_.current_project_state.surface.focus == FocusTarget::Panel &&
              active_terminal_tab != nullptr);
    case ActionId::Goto:
    case ActionId::Jump:
    case ActionId::ReplaceInBuffer:
    case ActionId::Reopen:
    case ActionId::Search:
    case ActionId::SplitEditorRight:
    case ActionId::SplitEditorDown:
      // Available with an editor to split from, until the editor area is full.
      // Gates the tab/tree context menus and the split-right/split-down palette
      // entries alike.
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
      // Group focus/close only make sense once a second group exists.
      return operations_.editor_group_count() > 1;
    case ActionId::Save:
      return operations_.active_tab_is_editor() || operations_.active_tab_is_merge() ||
             (operations_.active_tab_is_compare() &&
              operations_.active_compare_tab() != nullptr &&
              operations_.active_compare_tab()->right_editable);
    case ActionId::Focus:
      return true;
    case ActionId::IndentWidth:
      return true;
    case ActionId::CopyAbsolutePath:
    case ActionId::ShowInFileExplorer:
      return !operations_.resolve_tree_action_path(ActionSource::ContextMenu).empty() ||
             !operations_.active_tab_path().empty();
    case ActionId::RevealInFileTree:
      // Acts on the row a context menu named, else the active editor tab; either
      // way it needs a project root to reveal within.
      return !context_.current_project_state.root.empty() &&
             (!operations_.row_context_menu_path().empty() ||
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
    case ActionId::ToggleLineComment:
    case ActionId::ToggleBlockComment:
      return active_editable_viewport != nullptr &&
             SettingEnabled(operations_, "editor.shaping.toggle_comment.enabled", true);
    case ActionId::ColumnSelectUp:
    case ActionId::ColumnSelectDown:
    case ActionId::ColumnSelectLeft:
    case ActionId::ColumnSelectRight:
      // No settings gate: column selection is caret movement, not a shaping edit,
      // so it follows the plain "is there something to move a caret in" rule.
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
      return active_editable_viewport != nullptr &&
             SettingEnabled(operations_, "editor.shaping.line_ops.enabled", true);
    case ActionId::SortLinesAscending:
    case ActionId::SortLinesDescending:
      return active_editable_viewport != nullptr &&
             SettingEnabled(operations_, "editor.shaping.sort_lines.enabled", true);
    case ActionId::AddCursorAtNextMatch:
    case ActionId::AddCursorAtAllMatches:
      return active_editable_viewport != nullptr &&
             SettingEnabled(operations_, "editor.multicursor.add_at_match.enabled", true);
    case ActionId::JumpToMatchingBracket:
      return active_viewport != nullptr;
    case ActionId::MarkBranchFileReviewed:
    case ActionId::UnmarkBranchFileReviewed:
    case ActionId::MarkBranchHunkReviewed:
    case ActionId::UnmarkBranchHunkReviewed:
    case ActionId::ClearBranchReviewState:
    case ActionId::EditBranchReviewNote:
      return operations_.active_tab_is_compare();
    case ActionId::Fold:
    case ActionId::Unfold:
    case ActionId::ToggleFoldAtCursor:
    case ActionId::FoldAll:
    case ActionId::UnfoldAll:
      return active_viewport != nullptr &&
             SettingEnabled(operations_, "editor.fold.enabled", true);
    // Editor-essentials capability toggles: always available; flipping them
    // requires no surrounding context.
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

}  // namespace microide::workspace
