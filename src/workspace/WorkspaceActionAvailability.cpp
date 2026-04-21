#include "workspace/WorkspaceActionAvailability.h"

#include <filesystem>
#include <utility>

namespace microide::workspace {

ActionAvailability::ActionAvailability(const WorkspaceContext& context, Operations operations)
    : context_(context), operations_(std::move(operations)) {}

TreeContextTargetKind ActionAvailability::ActiveTreeTargetKind() const {
  return context_.menu_state.tree_context_menu.open ? context_.menu_state.tree_context_menu.target
                                                    : operations_.selected_tree_target_kind();
}

bool ActionAvailability::IsEnabled(ActionId id) const {
  const editor::TextViewport* active_viewport = operations_.active_editable_viewport();
  const TerminalTabState* active_terminal_tab = operations_.active_terminal_tab();
  switch (id) {
    case ActionId::AuthLogin:
    case ActionId::AuthRefresh:
    case ActionId::AuthLogout:
    case ActionId::DebugStart:
    case ActionId::McpTool:
    case ActionId::ShowChat:
    case ActionId::ShowOutput:
    case ActionId::Tasks:
      return !context_.current_project_state.root.empty();
    case ActionId::CodeActions:
    case ActionId::Completion:
    case ActionId::InlineCompletion:
      return active_viewport != nullptr;
    case ActionId::Colorscheme:
    case ActionId::Files:
    case ActionId::OpenCommandPrompt:
    case ActionId::PluginsReload:
    case ActionId::ProjectOpen:
    case ActionId::Quit:
    case ActionId::SidebarClose:
    case ActionId::SidebarHide:
    case ActionId::SidebarShow:
    case ActionId::SidebarToggle:
      return true;
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
      return active_viewport != nullptr && active_viewport->has_selection();
    case ActionId::CopySelection:
      return (active_viewport != nullptr && active_viewport->has_selection()) ||
             (context_.current_project_state.surface.focus == FocusTarget::Panel &&
              operations_.terminal_has_selection());
    case ActionId::CutSelection:
    case ActionId::Redo:
    case ActionId::SelectAll:
    case ActionId::Undo:
      return active_viewport != nullptr;
    case ActionId::PasteClipboard:
      return active_viewport != nullptr ||
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
      return !operations_.resolve_tree_action_path(ActionSource::ContextMenu).empty();
    case ActionId::CopyRelativePath: {
      const std::filesystem::path path =
          operations_.resolve_tree_action_path(ActionSource::ContextMenu);
      return !context_.current_project_state.root.empty() && !path.empty() &&
             path != context_.current_project_state.root;
    }
    case ActionId::SidebarWidth:
    case ActionId::SoftTabs:
    case ActionId::TabSize:
    case ActionId::TestsDiscover:
    case ActionId::UiScale:
      return true;
    case ActionId::DebugStop:
      return context_.current_project_state.debug_session.running;
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
  }

  return true;
}

}  // namespace microide::workspace
