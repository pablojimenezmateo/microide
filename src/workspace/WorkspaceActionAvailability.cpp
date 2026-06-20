#include "workspace/WorkspaceActionAvailability.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

namespace microide::workspace {

namespace {

bool SettingEnabled(const ActionAvailability::Operations& operations,
                    std::string_view id,
                    bool default_value) {
  if (!operations.get_setting_value) {
    return default_value;
  }
  const auto value = operations.get_setting_value(id);
  if (!value.has_value()) {
    return default_value;
  }
  return !(*value == "false" || *value == "0" || *value == "off");
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
      return active_viewport != nullptr && operations_.active_code_actions_available();
    case ActionId::Completion:
      return active_viewport != nullptr && operations_.active_completion_available();
    case ActionId::InsertSnippet:
      return active_editable_viewport != nullptr &&
             SettingEnabled(operations_, "editor.snippets.enabled", true) &&
             !active_editable_viewport->path().empty();
    case ActionId::FindReferences:
      return active_viewport != nullptr && operations_.active_references_available();
    case ActionId::GoToDefinition:
      return active_viewport != nullptr && operations_.active_definition_available();
    case ActionId::InlineCompletion:
      return active_viewport != nullptr;
    case ActionId::Colorscheme:
    case ActionId::Files:
    case ActionId::OpenCommandPrompt:
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
    case ActionId::CloseActiveTab:
      return !context_.current_project_state.open_tabs.empty();
    case ActionId::CloseAllTabs:
      return !context_.current_project_state.open_tabs.empty();
    case ActionId::CloseOtherTabs:
      return context_.current_project_state.open_tabs.size() > 1;
    case ActionId::CloseTabsToRight:
      return !context_.current_project_state.open_tabs.empty() &&
             context_.current_project_state.active_tab_index + 1 <
                 context_.current_project_state.open_tabs.size();
    case ActionId::CloseTabsToLeft:
      return !context_.current_project_state.open_tabs.empty() &&
             context_.current_project_state.active_tab_index > 0;
    case ActionId::CompareHead:
    case ActionId::OpenSelectedTreeItem:
    case ActionId::OpenSelectedTreeItemInNewTab:
      return !context_.current_project_state.root.empty() &&
             ActiveTreeTargetKind() == TreeContextTargetKind::File;
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
    case ActionId::Merge:
    case ActionId::Open:
    case ActionId::ProjectClose:
    case ActionId::ProjectSearch:
    case ActionId::Tab:
    case ActionId::Term:
    case ActionId::Tree:
    case ActionId::TreeRefresh:
      return !context_.current_project_state.root.empty();
    case ActionId::CopyLastTerminalCommand:
      return active_terminal_tab != nullptr &&
             operations_.last_terminal_command_text().has_value();
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
      return active_viewport != nullptr ||
             text_input_surface == TextInputSurface::PromptInput ||
             text_input_surface == TextInputSurface::Command ||
             text_input_surface == TextInputSurface::FileFinder ||
             text_input_surface == TextInputSurface::BufferSearch ||
             text_input_surface == TextInputSurface::BufferReplaceSearch ||
             text_input_surface == TextInputSurface::BufferReplaceReplace ||
             text_input_surface == TextInputSurface::ProjectSearchOverlay ||
             text_input_surface == TextInputSurface::CommitPicker ||
             text_input_surface == TextInputSurface::SidebarSearchQuery ||
             text_input_surface == TextInputSurface::SidebarSearchReplace;
    case ActionId::PasteClipboard:
      return active_editable_viewport != nullptr ||
             text_input_surface == TextInputSurface::PromptInput ||
             text_input_surface == TextInputSurface::Command ||
             text_input_surface == TextInputSurface::FileFinder ||
             text_input_surface == TextInputSurface::BufferSearch ||
             text_input_surface == TextInputSurface::BufferReplaceSearch ||
             text_input_surface == TextInputSurface::BufferReplaceReplace ||
             text_input_surface == TextInputSurface::ProjectSearchOverlay ||
             text_input_surface == TextInputSurface::CommitPicker ||
             text_input_surface == TextInputSurface::SidebarSearchQuery ||
             text_input_surface == TextInputSurface::SidebarSearchReplace ||
             (context_.current_project_state.surface.focus == FocusTarget::Panel &&
              active_terminal_tab != nullptr);
    case ActionId::Goto:
    case ActionId::Jump:
    case ActionId::ReplaceInBuffer:
    case ActionId::Reopen:
    case ActionId::Search:
    case ActionId::SplitFirst:
    case ActionId::SplitLast:
    case ActionId::SplitNext:
    case ActionId::SplitPrev:
    case ActionId::Unsplit:
    case ActionId::Vsplit:
      return operations_.active_tab_is_editor();
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
             !context_.current_project_state.open_tabs.empty();
    case ActionId::TestsRun:
      return !context_.current_project_state.sidebar.tests.entries.empty();
    case ActionId::ToggleLineComment:
    case ActionId::ToggleBlockComment:
      return active_editable_viewport != nullptr &&
             SettingEnabled(operations_, "editor.shaping.toggle_comment.enabled", true);
    case ActionId::MoveLineUp:
    case ActionId::MoveLineDown:
    case ActionId::DuplicateLine:
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
    case ActionId::MarkBranchHunkReviewed:
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
