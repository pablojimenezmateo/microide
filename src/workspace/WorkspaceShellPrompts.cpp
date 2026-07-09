#include "workspace/WorkspaceShell.h"

#include "editor/BreakpointStore.h"
#include "util/Parse.h"
#include "workspace/EditorTabService.h"
#include "workspace/WorkspaceActionCoordinator.h"
#include "workspace/GitSidebarCommandCenter.h"
#include "workspace/PromptSurfaceService.h"
#include "workspace/WorkspaceDirtyPromptCoordinator.h"
#include "workspace/WorkspaceMenuCoordinator.h"
#include "workspace/WorkspacePathMutationCoordinator.h"
#include "workspace/WorkspacePathUtils.h"

namespace microide::workspace {

PromptSurfaceService WorkspaceShell::MakePromptSurfaceService() {
  return PromptSurfaceService(
      context_.current_project_state,
      context_.prompts,
      PromptSurfaceService::Operations{
          .request_prompt_redraw = [this]() { RequestPromptRedraw(); },
      });
}

void WorkspaceShell::ShowDirtyPromptForTab(std::size_t index) {
  MakePromptSurfaceService().ShowDirtyPromptForTab(index);
}

void WorkspaceShell::ShowDirtyPromptForTabs(std::vector<std::size_t> target_tabs,
                                            std::vector<std::size_t> dirty_tabs) {
  MakePromptSurfaceService().ShowDirtyPromptForTabs(std::move(target_tabs), std::move(dirty_tabs));
}

void WorkspaceShell::ShowDirtyPromptForProject(std::size_t index) {
  if (index >= context_.project_catalog.entries.size()) {
    return;
  }

  // The empty-check and count span ALL editor groups: a project whose only unsaved
  // buffer lives in the non-focused split group must still prompt (else closing it
  // would silently discard that buffer). ConfirmCloseProject re-derives the full
  // all-groups set to save, so the focused-group `dirty_tabs` payload is display-only.
  const std::size_t dirty_count = DirtyEditorGroupTabsForProject(index).size();
  if (dirty_count == 0) {
    CloseProject(index);
    return;
  }

  const std::vector<std::size_t> dirty_tabs = DirtyEditorTabIndicesForProject(index);
  MakePromptSurfaceService().ShowDirtyPromptForProject(index, dirty_tabs, dirty_count);
}

void WorkspaceShell::ShowDirtyPromptForQuit() {
  // Count unsaved buffers across ALL editor groups so a tab dirtied only in the
  // non-focused split group is included (VSCode "Save All"). The dirty_tabs payload
  // stays focused-group: it is display-only, and ConfirmQuit re-derives the
  // all-groups set per project to actually save.
  std::size_t dirty_count = DirtyEditorGroupTabs().size();
  for (std::size_t i = 0; i < context_.project_catalog.entries.size(); ++i) {
    if (HasActiveProjectCatalogEntry() && i == context_.project_catalog.active_index) {
      continue;
    }
    dirty_count += DirtyEditorGroupTabsForProject(i).size();
  }

  MakePromptSurfaceService().ShowDirtyPromptForQuit(context_.current_project_state.focused_group().active_tab_index,
                                                    context_.project_catalog.active_index,
                                                    DirtyEditorTabIndices(),
                                                    dirty_count);
}

void WorkspaceShell::DismissDirtyPrompt(bool restore_focus) {
  MakePromptSurfaceService().DismissDirtyPrompt(restore_focus);
}

void WorkspaceShell::ConfirmDirtyPrompt() {
  EditorTabService editor_tabs = MakeEditorTabService();
  PromptSurfaceService prompt_surfaces = MakePromptSurfaceService();
  MakeDirtyPromptCoordinator(editor_tabs, prompt_surfaces).Confirm();
}

std::array<std::string, 3> WorkspaceShell::DirtyPromptActionLabels() const {
  if (context_.prompts.dirty.kind == DirtyPromptState::Kind::Quit ||
      context_.prompts.dirty.kind == DirtyPromptState::Kind::CloseTabs ||
      context_.prompts.dirty.kind == DirtyPromptState::Kind::CloseProject ||
      context_.prompts.dirty.kind == DirtyPromptState::Kind::RenamePath ||
      context_.prompts.dirty.kind == DirtyPromptState::Kind::DeletePath) {
    return {
        context_.prompts.dirty.dirty_count > 1 ? "Save all" : "Save",
        context_.prompts.dirty.dirty_count > 1 ? "Discard all" : "Discard",
        "Cancel",
    };
  }

  return {"Save", "Discard", "Cancel"};
}

std::string WorkspaceShell::DirtyPromptTitle() const {
  if (context_.prompts.dirty.kind == DirtyPromptState::Kind::Quit) {
    return "Unsaved changes before quit";
  }
  if (context_.prompts.dirty.kind == DirtyPromptState::Kind::CloseTabs) {
    return "Unsaved changes before closing tabs";
  }
  if (context_.prompts.dirty.kind == DirtyPromptState::Kind::CloseProject) {
    return "Unsaved changes before closing project";
  }
  if (context_.prompts.dirty.kind == DirtyPromptState::Kind::RenamePath) {
    return "Unsaved changes before rename";
  }
  if (context_.prompts.dirty.kind == DirtyPromptState::Kind::DeletePath) {
    return "Unsaved changes before delete";
  }
  return "Unsaved changes";
}

std::string WorkspaceShell::DirtyPromptMessage() const {
  if (context_.prompts.dirty.kind == DirtyPromptState::Kind::Quit) {
    const std::size_t dirty_count = context_.prompts.dirty.dirty_count;
    return dirty_count == 1 ? "Save the dirty tab before quitting microide?"
                            : "Save the " + std::to_string(dirty_count) +
                                  " dirty tabs before quitting microide?";
  }

  if (context_.prompts.dirty.kind == DirtyPromptState::Kind::CloseProject) {
    const std::filesystem::path project_root = ProjectCatalogRoot(context_.prompts.dirty.project_index);
    const std::string label = ProjectLabelForRoot(project_root);
    return context_.prompts.dirty.dirty_count == 1
               ? "Save the dirty tab before closing " + label + "?"
               : "Save the " + std::to_string(context_.prompts.dirty.dirty_count) +
                     " dirty tabs before closing " + label + "?";
  }

  if (context_.prompts.dirty.kind == DirtyPromptState::Kind::CloseTabs) {
    return context_.prompts.dirty.dirty_count == 1
               ? "Save the dirty tab before closing the selected tabs?"
               : "Save the " + std::to_string(context_.prompts.dirty.dirty_count) +
                     " dirty tabs before closing the selected tabs?";
  }

  if (context_.prompts.dirty.kind == DirtyPromptState::Kind::RenamePath ||
      context_.prompts.dirty.kind == DirtyPromptState::Kind::DeletePath) {
    const std::filesystem::path path =
        context_.prompts.dirty.path.empty() ? context_.prompts.surface.path : context_.prompts.dirty.path;
    const std::string label =
        path == context_.current_project_state.root ? ProjectLabel() : RelativePathLabel(context_.current_project_state.root, path);
    const std::string action =
        context_.prompts.dirty.kind == DirtyPromptState::Kind::RenamePath ? "renaming " : "deleting ";
    return context_.prompts.dirty.dirty_count == 1
               ? "Save the affected dirty editor before " + action + label + "?"
               : "Save the " + std::to_string(context_.prompts.dirty.dirty_count) +
                     " affected dirty editors before " + action + label + "?";
  }

  const std::size_t index = context_.prompts.dirty.tab_index;
  const std::string label = index < context_.current_project_state.focused_group().open_tabs.size() ? context_.current_project_state.focused_group().open_tabs[index].title : "this tab";
  return "Save changes to " + label + " before closing it?";
}

void WorkspaceShell::OpenPromptSurface(PromptSurfaceState::Action action,
                                       PromptSurfaceState::Kind kind,
                                       const std::filesystem::path& path,
                                       std::string input) {
  MakePromptSurfaceService().OpenPromptSurface(action, kind, path, std::move(input));
}

void WorkspaceShell::OpenExternalUrlPrompt(std::string url) {
  MakePromptSurfaceService().OpenExternalUrlPrompt(std::move(url));
}

void WorkspaceShell::DismissPromptSurface(bool restore_focus) {
  MakePromptSurfaceService().DismissPromptSurface(restore_focus);
}

std::string WorkspaceShell::PromptSurfaceTitle() const {
  switch (context_.prompts.surface.action) {
    case PromptSurfaceState::Action::CreateFile:
      return "New File";
    case PromptSurfaceState::Action::CreateDirectory:
      return "New Folder";
    case PromptSurfaceState::Action::RenamePath:
      return "Rename";
    case PromptSurfaceState::Action::DeletePath:
      return "Delete";
    case PromptSurfaceState::Action::DiscardGitChanges:
      return "Discard All Changes";
    case PromptSurfaceState::Action::DiscardGitEntry:
      return "Discard Git Changes";
    case PromptSurfaceState::Action::DiscardPatchPreview:
      return "Discard Patch";
    case PromptSurfaceState::Action::SetGitOutgoingBaseRef:
      return "Outgoing Base Ref";
    case PromptSurfaceState::Action::OpenExternalUrl:
      return "Open External Link";
    case PromptSurfaceState::Action::ConfirmCommitAmend:
      return "Amend Commit";
    case PromptSurfaceState::Action::ConfirmCommitNoVerify:
      return "Commit Without Hooks";
    case PromptSurfaceState::Action::SetBreakpointCondition:
      return "Breakpoint Condition";
    case PromptSurfaceState::Action::SetBreakpointHitCondition:
      return "Breakpoint Hit Count";
    case PromptSurfaceState::Action::SetBreakpointLogMessage:
      return "Breakpoint Log Message";
    case PromptSurfaceState::Action::AddWatchExpression:
      return "Add Watch";
    case PromptSurfaceState::Action::EditWatchExpression:
      return "Edit Watch";
    case PromptSurfaceState::Action::EvaluateReplInput:
      return "Debug Console";
    case PromptSurfaceState::Action::GoToLine:
      return "Go to Line";
    case PromptSurfaceState::Action::RenameSymbol:
      return "Rename Symbol";
    case PromptSurfaceState::Action::ConfirmRenameSave:
      return "Rename Across Files";
  }
  return "Prompt";
}

std::string WorkspaceShell::PromptSurfaceMessage() const {
  const std::string label =
      context_.prompts.surface.path == context_.current_project_state.root
          ? ProjectLabel()
          : RelativePathLabel(context_.current_project_state.root, context_.prompts.surface.path);
  switch (context_.prompts.surface.action) {
    case PromptSurfaceState::Action::CreateFile:
      return "Create inside " + (label.empty() ? ProjectLabel() : label) + ".";
    case PromptSurfaceState::Action::CreateDirectory:
      return "Create inside " + (label.empty() ? ProjectLabel() : label) + ".";
    case PromptSurfaceState::Action::RenamePath:
      return "Enter a new path for " + label + ".";
    case PromptSurfaceState::Action::DeletePath:
      return "Move " + label + " to trash?";
    case PromptSurfaceState::Action::DiscardGitChanges:
      return "Discard all tracked, untracked, and conflicted changes in " + ProjectLabel() + "?";
    case PromptSurfaceState::Action::DiscardGitEntry: {
      const auto entry_index = util::ParseSize(context_.prompts.surface.input.text());
      if (!entry_index.has_value() ||
          *entry_index >= context_.current_project_state.sidebar.git.entries.size()) {
        return "Discard changes for the selected Git sidebar row?";
      }
      return BuildGitDiscardPreviewSummary(
          context_.current_project_state.sidebar.git.entries[*entry_index], ProjectLabel());
    }
    case PromptSurfaceState::Action::DiscardPatchPreview:
      return context_.prompts.surface.detail.empty()
                 ? "Discard the selected changes from the working tree?"
                 : context_.prompts.surface.detail;
    case PromptSurfaceState::Action::SetGitOutgoingBaseRef:
      return "Compare outgoing files against this ref.";
    case PromptSurfaceState::Action::OpenExternalUrl:
      return "Open " + context_.prompts.surface.detail + " in your browser?";
    case PromptSurfaceState::Action::ConfirmCommitAmend:
    case PromptSurfaceState::Action::ConfirmCommitNoVerify:
      return context_.prompts.surface.detail;
    case PromptSurfaceState::Action::SetBreakpointCondition:
      return "Stop only when this expression is true (empty clears).";
    case PromptSurfaceState::Action::SetBreakpointHitCondition:
      return "Stop after this many hits, e.g. 5 or >10 (empty clears).";
    case PromptSurfaceState::Action::SetBreakpointLogMessage:
      return "Log this message instead of stopping; {expr} interpolates (empty clears).";
    case PromptSurfaceState::Action::AddWatchExpression:
      return "Evaluate this expression on every stop.";
    case PromptSurfaceState::Action::EditWatchExpression:
      return "Edit this watch expression (empty removes it).";
    case PromptSurfaceState::Action::EvaluateReplInput:
      return "Evaluate in the active session; the result prints to the console.";
    case PromptSurfaceState::Action::GoToLine:
      return "Enter a line[:column] to jump to.";
    case PromptSurfaceState::Action::RenameSymbol:
      return "Enter a new name for the symbol under the cursor.";
    case PromptSurfaceState::Action::ConfirmRenameSave:
      return context_.prompts.surface.detail;
  }
  return {};
}

std::string WorkspaceShell::PromptSurfaceDetail() const {
  switch (context_.prompts.surface.action) {
    case PromptSurfaceState::Action::SetGitOutgoingBaseRef:
    case PromptSurfaceState::Action::OpenExternalUrl:
      return context_.prompts.surface.detail;
    default:
      return {};
  }
}

std::vector<std::string> WorkspaceShell::PromptSurfaceActionLabels() const {
  switch (context_.prompts.surface.action) {
    case PromptSurfaceState::Action::CreateFile:
      return {"Create File", "Cancel"};
    case PromptSurfaceState::Action::CreateDirectory:
      return {"Create Folder", "Cancel"};
    case PromptSurfaceState::Action::RenamePath:
      return {"Rename", "Cancel"};
    case PromptSurfaceState::Action::DeletePath:
      return {"Delete", "Cancel"};
    case PromptSurfaceState::Action::DiscardGitChanges:
      return {"Discard All", "Cancel"};
    case PromptSurfaceState::Action::DiscardGitEntry:
      return {"Discard", "Cancel"};
    case PromptSurfaceState::Action::DiscardPatchPreview:
      return {"Discard", "Cancel"};
    case PromptSurfaceState::Action::SetGitOutgoingBaseRef:
      return {"Use Ref", "Cancel"};
    case PromptSurfaceState::Action::OpenExternalUrl:
      return {"Open Link", "Cancel"};
    case PromptSurfaceState::Action::ConfirmCommitAmend:
      return {"Amend", "Cancel"};
    case PromptSurfaceState::Action::ConfirmCommitNoVerify:
      return {"Commit", "Cancel"};
    case PromptSurfaceState::Action::SetBreakpointCondition:
    case PromptSurfaceState::Action::SetBreakpointHitCondition:
    case PromptSurfaceState::Action::SetBreakpointLogMessage:
      return {"Set", "Cancel"};
    case PromptSurfaceState::Action::AddWatchExpression:
      return {"Add", "Cancel"};
    case PromptSurfaceState::Action::EditWatchExpression:
      return {"Save", "Cancel"};
    case PromptSurfaceState::Action::EvaluateReplInput:
      return {"Evaluate", "Close"};
    case PromptSurfaceState::Action::GoToLine:
      return {"Go", "Cancel"};
    case PromptSurfaceState::Action::RenameSymbol:
      return {"Rename", "Cancel"};
    case PromptSurfaceState::Action::ConfirmRenameSave:
      return {"Rename & Save", "Cancel"};
  }
  return {"OK", "Cancel"};
}

std::filesystem::path WorkspaceShell::TreeMutationBasePath(ActionSource source) const {
  if (context_.current_project_state.root.empty()) {
    return {};
  }
  if (source == ActionSource::ContextMenu && context_.menu_state.tree_context_menu.open &&
      context_.menu_state.tree_context_menu.target == TreeContextTargetKind::Background) {
    return context_.current_project_state.root;
  }

  std::filesystem::path path = ResolveTreeActionPath(source);
  if (path.empty()) {
    return context_.current_project_state.root;
  }

  std::error_code error;
  if (std::filesystem::is_directory(path, error) && !error) {
    return path.lexically_normal();
  }
  return path.parent_path().lexically_normal();
}

bool WorkspaceShell::HasDirtyEditorTabsForPath(const std::filesystem::path& path,
                                               std::string* blocking_label) const {
  auto* shell = const_cast<WorkspaceShell*>(this);
  EditorTabService editor_tabs = shell->MakeEditorTabService();
  PromptSurfaceService prompt_surfaces = shell->MakePromptSurfaceService();
  return shell->MakePathMutationCoordinator(editor_tabs, prompt_surfaces)
      .HasDirtyEditorTabsForPath(path, blocking_label);
}

void WorkspaceShell::CloseOpenTabsForPath(const std::filesystem::path& path) {
  EditorTabService editor_tabs = MakeEditorTabService();
  PromptSurfaceService prompt_surfaces = MakePromptSurfaceService();
  MakePathMutationCoordinator(editor_tabs, prompt_surfaces).CloseOpenTabsForPath(path);
}

void WorkspaceShell::ConfirmPromptSurface(DirtyPathResolution resolution) {
  if (context_.prompts.surface_visible &&
      context_.prompts.surface.action == PromptSurfaceState::Action::OpenExternalUrl) {
    const std::string url = context_.prompts.surface.detail;
    const bool opened = !url.empty() && OpenExternalUrl(url);
    (void)opened;
    // Always restore focus to the surface that owned it before the prompt; the
    // success branch previously passed `!opened` (== false), stranding keyboard
    // focus on the dismissed prompt and leaving input dead until the next click.
    MakePromptSurfaceService().DismissPromptSurface(true);
    return;
  }
  if (context_.prompts.surface_visible &&
      context_.prompts.surface.action == PromptSurfaceState::Action::ConfirmRenameSave) {
    const bool confirmed = resolution != DirtyPathResolution::Discard;
    MakePromptSurfaceService().DismissPromptSurface(true);
    if (confirmed) {
      CommitPendingRenameSave();
    } else {
      DiscardPendingRenameSave();
    }
    return;
  }
  if (context_.prompts.surface_visible &&
      (context_.prompts.surface.action == PromptSurfaceState::Action::ConfirmCommitAmend ||
       context_.prompts.surface.action == PromptSurfaceState::Action::ConfirmCommitNoVerify)) {
    InitializeCommitWorkflowService();
    auto& workflow = context_.current_project_state.sidebar.git.commit_workflow;
    if (resolution != DirtyPathResolution::Discard) {
      commit_workflow_service_.ConfirmPendingOperation(workflow);
    } else {
      commit_workflow_service_.CancelPendingConfirmation(workflow);
    }
    MakePromptSurfaceService().DismissPromptSurface(false);
    context_.current_project_state.surface.focus = FocusTarget::Sidebar;
    return;
  }
  if (context_.prompts.surface_visible &&
      context_.prompts.surface.action == PromptSurfaceState::Action::SetGitOutgoingBaseRef) {
    const std::string ref = context_.prompts.surface.input.text();
    if (ref.empty()) {
      return;
    }
    MakePromptSurfaceService().DismissPromptSurface(false);
    SetGitOutgoingBaseChoice(OutgoingBaseChoice{
        .kind = OutgoingBaseChoice::Kind::SpecificRef,
        .custom_ref = ref,
    });
    context_.current_project_state.surface.focus = FocusTarget::Sidebar;
    return;
  }
  if (context_.prompts.surface_visible &&
      (context_.prompts.surface.action == PromptSurfaceState::Action::SetBreakpointCondition ||
       context_.prompts.surface.action == PromptSurfaceState::Action::SetBreakpointHitCondition ||
       context_.prompts.surface.action == PromptSurfaceState::Action::SetBreakpointLogMessage)) {
    CommitBreakpointModifierPrompt();
    return;
  }
  if (context_.prompts.surface_visible &&
      (context_.prompts.surface.action == PromptSurfaceState::Action::AddWatchExpression ||
       context_.prompts.surface.action == PromptSurfaceState::Action::EditWatchExpression)) {
    CommitWatchExpressionPrompt();
    return;
  }
  if (context_.prompts.surface_visible &&
      context_.prompts.surface.action == PromptSurfaceState::Action::EvaluateReplInput) {
    CommitDebugReplPrompt();
    return;
  }
  if (context_.prompts.surface_visible &&
      context_.prompts.surface.action == PromptSurfaceState::Action::GoToLine) {
    // Commit "Go to Line": reuse the ActionId::Goto path so the typed
    // line[:column] shares the same parsing/clamping as the `goto` command.
    const std::string spec = context_.prompts.surface.input.text();
    MakePromptSurfaceService().DismissPromptSurface(true);
    if (!spec.empty()) {
      ActionCoordinator(MakeActionContext()).Execute(ActionId::Goto, {spec}, ActionSource::Shortcut);
    }
    return;
  }
  if (context_.prompts.surface_visible &&
      context_.prompts.surface.action == PromptSurfaceState::Action::RenameSymbol) {
    // Commit "Rename Symbol": hand the typed name to the LSP rename path, which
    // renames the symbol at the (still-current) cursor across open buffers.
    const std::string new_name = context_.prompts.surface.input.text();
    MakePromptSurfaceService().DismissPromptSurface(true);
    if (!new_name.empty()) {
      MakeActionContext().RenameSymbol(new_name, nullptr);
    }
    return;
  }
  EditorTabService editor_tabs = MakeEditorTabService();
  PromptSurfaceService prompt_surfaces = MakePromptSurfaceService();
  MakePathMutationCoordinator(editor_tabs, prompt_surfaces).ConfirmPromptSurface(resolution);
}

void WorkspaceShell::OpenBreakpointContextMenu(const std::filesystem::path& path, std::size_t line,
                                               const SDL_FRect& anchor_rect) {
  // MATLAB-style: the gutter menu acts on an existing breakpoint only. Right-click
  // on a bare line is inert (left-click still sets a plain breakpoint).
  const editor::Breakpoint* existing = nullptr;
  if (const std::vector<editor::Breakpoint>* bps =
          context_.current_project_state.breakpoint_store.FindByPath(path);
      bps != nullptr) {
    for (const editor::Breakpoint& bp : *bps) {
      if (bp.line == line) {
        existing = &bp;
        break;
      }
    }
  }
  if (existing == nullptr) {
    return;
  }
  MakeMenuCoordinator().OpenTreeContextMenu(TreeContextTargetKind::BreakpointLine, path,
                                            anchor_rect, line);
  context_.menu_state.tree_context_menu.breakpoint_enabled = existing->enabled;
}

void WorkspaceShell::EditBreakpointModifierFromMenu(ActionId id) {
  const std::filesystem::path path = context_.menu_state.tree_context_menu.path;
  const std::size_t line = context_.menu_state.tree_context_menu.line;
  if (path.empty()) {
    return;
  }
  // Seed the prompt with the current field value, if a breakpoint exists.
  const editor::Breakpoint* existing = nullptr;
  if (const std::vector<editor::Breakpoint>* bps =
          context_.current_project_state.breakpoint_store.FindByPath(path);
      bps != nullptr) {
    for (const editor::Breakpoint& bp : *bps) {
      if (bp.line == line) {
        existing = &bp;
        break;
      }
    }
  }
  PromptSurfaceState::Action action = PromptSurfaceState::Action::SetBreakpointCondition;
  std::optional<std::string> current;
  switch (id) {
    case ActionId::DebugBreakpointEditCondition:
      action = PromptSurfaceState::Action::SetBreakpointCondition;
      current = existing != nullptr ? existing->condition : std::nullopt;
      break;
    case ActionId::DebugBreakpointEditHitCondition:
      action = PromptSurfaceState::Action::SetBreakpointHitCondition;
      current = existing != nullptr ? existing->hit_condition : std::nullopt;
      break;
    case ActionId::DebugBreakpointEditLogMessage:
      action = PromptSurfaceState::Action::SetBreakpointLogMessage;
      current = existing != nullptr ? existing->log_message : std::nullopt;
      break;
    default:
      return;
  }
  MakePromptSurfaceService().OpenPromptSurface(action, PromptSurfaceState::Kind::TextInput, path,
                                               current.value_or(std::string{}));
  context_.prompts.surface.target_line = line;
}

void WorkspaceShell::CommitBreakpointModifierPrompt() {
  const std::filesystem::path path = context_.prompts.surface.path;
  const std::size_t line = context_.prompts.surface.target_line;
  const std::string text = context_.prompts.surface.input.text();
  std::optional<std::string> value;
  if (!text.empty()) {
    value = text;
  }
  editor::BreakpointStore& store = context_.current_project_state.breakpoint_store;
  switch (context_.prompts.surface.action) {
    case PromptSurfaceState::Action::SetBreakpointCondition:
      store.SetCondition(path, line, std::move(value));
      break;
    case PromptSurfaceState::Action::SetBreakpointHitCondition:
      store.SetHitCondition(path, line, std::move(value));
      break;
    case PromptSurfaceState::Action::SetBreakpointLogMessage:
      store.SetLogMessage(path, line, std::move(value));
      break;
    default:
      break;
  }
  MakePromptSurfaceService().DismissPromptSurface(true);
  ResendBreakpointsForFile(path);
  RequestFocusedEditorRedraw();
}

void WorkspaceShell::RemoveBreakpointFromMenu() {
  const std::filesystem::path path = context_.menu_state.tree_context_menu.path;
  const std::size_t line = context_.menu_state.tree_context_menu.line;
  if (path.empty()) {
    return;
  }
  context_.current_project_state.breakpoint_store.Remove(path, line);
  ResendBreakpointsForFile(path);
  RequestFocusedEditorRedraw();
}

void WorkspaceShell::BreakpointQuickActionFromMenu(ActionId id) {
  const std::filesystem::path path = context_.menu_state.tree_context_menu.path;
  const std::size_t line = context_.menu_state.tree_context_menu.line;
  if (path.empty()) {
    return;
  }
  editor::BreakpointStore& store = context_.current_project_state.breakpoint_store;
  switch (id) {
    case ActionId::DebugBreakpointToggleEnabled:
      store.ToggleEnabled(path, line);
      break;
    case ActionId::DebugBreakpointClearCondition:
      // Clears only the condition; hit-count / log-message modifiers are kept.
      store.SetCondition(path, line, std::nullopt);
      break;
    default:
      return;
  }
  ResendBreakpointsForFile(path);
  RequestFocusedEditorRedraw();
}

void WorkspaceShell::OpenWatchExpressionPrompt(std::optional<std::size_t> index) {
  const std::vector<std::string>& expressions =
      context_.current_project_state.debug_watch.Expressions();
  std::string seed;
  PromptSurfaceState::Action action = PromptSurfaceState::Action::AddWatchExpression;
  if (index.has_value() && *index < expressions.size()) {
    action = PromptSurfaceState::Action::EditWatchExpression;
    seed = expressions[*index];
  }
  // The watch list has no associated path; reuse the prompt's root path so the
  // surface still resolves a sensible label, and carry the index in target_line.
  MakePromptSurfaceService().OpenPromptSurface(action, PromptSurfaceState::Kind::TextInput,
                                               context_.current_project_state.root, std::move(seed));
  context_.prompts.surface.target_line = index.value_or(0);
}

void WorkspaceShell::CommitWatchExpressionPrompt() {
  const std::string text = context_.prompts.surface.input.text();
  const bool editing =
      context_.prompts.surface.action == PromptSurfaceState::Action::EditWatchExpression;
  const std::size_t index = context_.prompts.surface.target_line;
  MakePromptSurfaceService().DismissPromptSurface(true);
  if (editing) {
    debug_service_.EditWatch(index, text);  // empty text removes the expression
  } else if (!text.empty()) {
    debug_service_.AddWatch(text);
  }
  RequestBottomPanelRedraw();
}

void WorkspaceShell::OpenDebugReplPrompt() {
  // The REPL has no associated path; reuse the project root so the surface still
  // resolves a sensible label.
  MakePromptSurfaceService().OpenPromptSurface(PromptSurfaceState::Action::EvaluateReplInput,
                                               PromptSurfaceState::Kind::TextInput,
                                               context_.current_project_state.root, std::string{});
}

void WorkspaceShell::CommitDebugReplPrompt() {
  const std::string text = context_.prompts.surface.input.text();
  MakePromptSurfaceService().DismissPromptSurface(true);
  if (text.empty()) {
    // Empty input closes the REPL loop rather than evaluating nothing.
    return;
  }
  debug_service_.EvaluateRepl(text);
  // Re-open the prompt so the user can keep evaluating without re-triggering the
  // command (REPL-like). A no-active-session evaluate is a no-op in the service.
  OpenDebugReplPrompt();
}

}  // namespace microide::workspace
