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
  const editor::TextViewport* active_viewport = operations_.active_navigable_viewport();
  const editor::TextViewport* active_editable_viewport = operations_.active_editable_viewport();
  const TerminalTabState* active_terminal_tab = operations_.active_terminal_tab();
  const TextInputSurface text_input_surface = operations_.current_text_input_surface();
  const bool active_single_line_selection = operations_.active_single_line_text_has_selection();
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
      return active_viewport != nullptr && operations_.active_code_actions_available();
    case ActionId::Completion:
      return active_viewport != nullptr && operations_.active_completion_available();
    case ActionId::FindReferences:
      return active_viewport != nullptr && operations_.active_references_available();
    case ActionId::GoToDefinition:
      return active_viewport != nullptr && operations_.active_definition_available();
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
             text_input_surface == TextInputSurface::ChatComposer ||
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
             text_input_surface == TextInputSurface::ChatComposer ||
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
