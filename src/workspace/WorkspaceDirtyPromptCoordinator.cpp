#include "workspace/WorkspaceDirtyPromptCoordinator.h"

#include <algorithm>
#include <filesystem>
#include <utility>
#include <vector>

#include "workspace/PromptSurfaceService.h"
#include "workspace/WorkspaceShell.h"

namespace microide::workspace {

DirtyPromptCoordinator::DirtyPromptCoordinator(WorkspaceContext& context,
                                               bool& quit_requested,
                                               EditorTabService& editor_tabs,
                                               PromptSurfaceService& prompt_surfaces,
                                               Operations operations)
    : context_(context),
      quit_requested_(quit_requested),
      editor_tabs_(editor_tabs),
      prompt_surfaces_(prompt_surfaces),
      operations_(std::move(operations)) {}

void DirtyPromptCoordinator::Confirm() {
  if (!context_.prompts.dirty_visible) {
    return;
  }

  const DirtyPromptState prompt = context_.prompts.dirty;
  if (prompt.selected_action == 2) {
    prompt_surfaces_.DismissDirtyPrompt(true);
    return;
  }

  if (prompt.kind == DirtyPromptState::Kind::RenamePath ||
      prompt.kind == DirtyPromptState::Kind::DeletePath) {
    operations_.confirm_path_prompt(prompt.selected_action == 0);
    return;
  }

  switch (prompt.kind) {
    case DirtyPromptState::Kind::CloseTab:
      ConfirmCloseTab(prompt);
      return;
    case DirtyPromptState::Kind::CloseTabs:
      ConfirmCloseTabs(prompt);
      return;
    case DirtyPromptState::Kind::CloseProject:
      ConfirmCloseProject(prompt);
      return;
    case DirtyPromptState::Kind::Quit:
      ConfirmQuit(prompt);
      return;
    case DirtyPromptState::Kind::RenamePath:
    case DirtyPromptState::Kind::DeletePath:
      return;
  }
}

std::optional<std::size_t> DirtyPromptCoordinator::FindProjectIndexByRoot(
    const std::filesystem::path& root) const {
  if (root.empty()) {
    return std::nullopt;
  }
  for (std::size_t i = 0; i < context_.project_catalog.entries.size(); ++i) {
    if (context_.ProjectCatalogRoot(i) == root) {
      return i;
    }
  }
  return std::nullopt;
}

bool DirtyPromptCoordinator::SaveDirtyTabs(std::span<const std::size_t> tab_indices) {
  for (std::size_t index : tab_indices) {
    if (!editor_tabs_.Save(index)) {
      return false;
    }
  }
  return true;
}

bool DirtyPromptCoordinator::SaveDirtyGroupTabs(std::span<const GroupTabRef> refs) {
  for (const GroupTabRef& ref : refs) {
    if (!editor_tabs_.SaveGroupTab(ref.group_index, ref.tab_index)) {
      return false;
    }
  }
  return true;
}

bool DirtyPromptCoordinator::SwitchProjectByRoot(const std::filesystem::path& root) {
  const auto index = FindProjectIndexByRoot(root);
  return index.has_value() && operations_.switch_project(*index, false);
}

std::optional<std::size_t> DirtyPromptCoordinator::ResolveFocusedTabIndexById(
    std::uint64_t id) const {
  if (id == 0) {
    return std::nullopt;
  }
  const auto& tabs = context_.current_project_state.focused_group().open_tabs;
  for (std::size_t i = 0; i < tabs.size(); ++i) {
    if (tabs[i].stable_id == id) {
      return i;
    }
  }
  return std::nullopt;
}

std::vector<std::size_t> DirtyPromptCoordinator::ResolveFocusedTabIndices(
    const std::vector<std::uint64_t>& ids,
    const std::vector<std::size_t>& fallback_indices) const {
  if (ids.empty()) {
    // No ids captured (id-less legacy prompt): use the stored indices as-is.
    return fallback_indices;
  }
  std::vector<std::size_t> indices;
  indices.reserve(ids.size());
  for (std::uint64_t id : ids) {
    if (const std::optional<std::size_t> resolved = ResolveFocusedTabIndexById(id)) {
      indices.push_back(*resolved);
    }
  }
  return indices;
}

void DirtyPromptCoordinator::ConfirmCloseTab(const DirtyPromptState& prompt) {
  // Resolve the stored stable id to the CURRENT focused-group index: a tab that
  // closed/reordered while the modal prompt was up must never be mis-saved/closed
  // (TD-2026-07-17-024). Fall back to the stored index only for an id-less prompt.
  const std::optional<std::size_t> resolved =
      prompt.tab_id != 0 ? ResolveFocusedTabIndexById(prompt.tab_id)
                         : std::optional<std::size_t>(prompt.tab_index);
  if (!resolved.has_value()) {
    // The target tab has already been closed — nothing to save or close.
    prompt_surfaces_.DismissDirtyPrompt(true);
    return;
  }
  if (prompt.selected_action == 0 && !editor_tabs_.Save(*resolved)) {
    return;
  }
  // Restore the pre-prompt focus (matching ConfirmCloseProject). TabCoordinator::
  // Close only resets focus off the overlay on the active-tab / last-tab paths;
  // closing a *non-active* dirty tab leaves focus == Overlay, so with the overlay
  // now hidden every keystroke would route to the dead overlay handler and be
  // swallowed until the user clicked back into a surface. restore_focus fixes it.
  prompt_surfaces_.DismissDirtyPrompt(true);
  editor_tabs_.Close(*resolved);
}

void DirtyPromptCoordinator::ConfirmCloseTabs(const DirtyPromptState& prompt) {
  // Resolve stored stable ids to CURRENT focused-group indices (dropping tabs that
  // closed while the prompt was up) so the save/close acts on the intended tabs, not
  // whatever now occupies the captured indices (TD-2026-07-17-024).
  const std::vector<std::size_t> dirty_indices =
      ResolveFocusedTabIndices(prompt.dirty_tab_ids, prompt.dirty_tabs);
  if (prompt.selected_action == 0 && !SaveDirtyTabs(dirty_indices)) {
    return;
  }

  // See ConfirmCloseTab: restore focus so closing non-active dirty tabs cannot
  // strand keyboard input on the now-hidden overlay handler.
  prompt_surfaces_.DismissDirtyPrompt(true);
  std::vector<std::size_t> indices =
      ResolveFocusedTabIndices(prompt.target_tab_ids, prompt.target_tabs);
  std::sort(indices.begin(), indices.end());
  indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
  // Close highest-index-first so each close cannot shift a not-yet-closed lower index.
  for (std::size_t i = indices.size(); i > 0; --i) {
    editor_tabs_.Close(indices[i - 1]);
  }
}

void DirtyPromptCoordinator::ConfirmCloseProject(const DirtyPromptState& prompt) {
  if (prompt.project_index >= context_.project_catalog.entries.size()) {
    prompt_surfaces_.DismissDirtyPrompt(true);
    return;
  }

  const bool target_was_active =
      context_.HasActiveProjectCatalogEntry() &&
      prompt.project_index == context_.project_catalog.active_index;
  const std::filesystem::path original_active_root = context_.current_project_state.root;
  const std::filesystem::path target_root = context_.ProjectCatalogRoot(prompt.project_index);

  if (prompt.selected_action == 0 && !target_was_active && !context_.current_project_state.root.empty()) {
    if (!SwitchProjectByRoot(target_root)) {
      prompt_surfaces_.DismissDirtyPrompt(true);
      return;
    }
  }
  // Closing a project flushes every dirty buffer across ALL its editor groups
  // (not just the focused group's tabs captured in prompt.dirty_tabs). The target
  // is active here (already-active, or just switched above), so DirtyGroupTabs()
  // reads the correct project.
  if (prompt.selected_action == 0 && !SaveDirtyGroupTabs(editor_tabs_.DirtyGroupTabs())) {
    if (!target_was_active && !original_active_root.empty()) {
      SwitchProjectByRoot(original_active_root);
    }
    return;
  }

  prompt_surfaces_.DismissDirtyPrompt(false);
  const auto target_index = FindProjectIndexByRoot(target_root);
  if (!target_index.has_value()) {
    return;
  }
  operations_.close_project(*target_index);

  if (!target_was_active && !original_active_root.empty()) {
    SwitchProjectByRoot(original_active_root);
  }
}

void DirtyPromptCoordinator::ConfirmQuit(const DirtyPromptState& prompt) {
  const std::filesystem::path original_active_root = context_.current_project_state.root;
  if (prompt.selected_action == 0) {
    std::vector<std::filesystem::path> project_roots;
    project_roots.reserve(context_.project_catalog.entries.size());
    for (std::size_t i = 0; i < context_.project_catalog.entries.size(); ++i) {
      const std::filesystem::path root = context_.ProjectCatalogRoot(i);
      if (!root.empty()) {
        project_roots.push_back(root);
      }
    }

    for (const auto& root : project_roots) {
      // Only projects with unsaved buffers need saving. DirtyGroupTabsForProject
      // inspects the catalog entry's in-memory tabs (all groups) without activating
      // it, so we avoid a full persist-out/load-in round trip for every clean project.
      const auto index = FindProjectIndexByRoot(root);
      if (!index.has_value() || editor_tabs_.DirtyGroupTabsForProject(*index).empty()) {
        continue;
      }
      if (!SwitchProjectByRoot(root)) {
        continue;
      }
      if (!SaveDirtyGroupTabs(editor_tabs_.DirtyGroupTabs())) {
        if (!original_active_root.empty()) {
          SwitchProjectByRoot(original_active_root);
        }
        return;
      }
    }

    if (!original_active_root.empty()) {
      SwitchProjectByRoot(original_active_root);
    }
  }

  prompt_surfaces_.DismissDirtyPrompt(false);
  quit_requested_ = true;
}

DirtyPromptCoordinator WorkspaceShell::MakeDirtyPromptCoordinator(
    EditorTabService& editor_tabs,
    PromptSurfaceService& prompt_surfaces) {
  return DirtyPromptCoordinator(
      context_,
      quit_requested_,
      editor_tabs,
      prompt_surfaces,
      DirtyPromptCoordinator::Operations{
          .confirm_path_prompt =
              [this](bool save_changes) {
                ConfirmPromptSurface(save_changes ? DirtyPathResolution::Save
                                                 : DirtyPathResolution::Discard);
              },
          .switch_project =
              [this](std::size_t index, bool log_feedback) {
                return SwitchProject(index, log_feedback);
              },
          .close_project = [this](std::size_t index) { CloseProject(index); },
      });
}

}  // namespace microide::workspace
