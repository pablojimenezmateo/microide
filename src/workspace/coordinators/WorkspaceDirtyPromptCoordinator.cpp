#include "workspace/coordinators/WorkspaceDirtyPromptCoordinator.h"

#include <algorithm>
#include <filesystem>
#include <utility>
#include <vector>

#include "workspace/coordinators/WorkspacePathMutationCoordinator.h"
#include "workspace/services/PromptSurfaceService.h"
#include "workspace/shell/WorkspaceShell.h"

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
    // Blocking: the legacy id-less prompt cannot address a deferred tab (see
    // ConfirmCloseTabs), so it waits for each write.
    if (!editor_tabs_.Save(index, SaveMode::Blocking)) {
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
  // Restore the pre-prompt focus (matching ConfirmCloseProject). TabCoordinator::
  // Close only resets focus off the overlay on the active-tab / last-tab paths;
  // closing a *non-active* dirty tab leaves focus == Overlay, so with the overlay
  // now hidden every keystroke would route to the dead overlay handler and be
  // swallowed until the user clicked back into a surface. restore_focus fixes it.
  if (prompt.selected_action == 0) {
    // Save-then-close, which closes when the WRITE lands. It used to save in
    // SaveMode::Blocking and wait, so closing one dirty JS file froze the window
    // for as long as node took to start (TD-2026-09-28-304). The prompt is
    // dismissed either way — the decision has been made — but the tab survives a
    // refused save, as it did before.
    if (!SaveThenClose(*resolved)) {
      return;
    }
    prompt_surfaces_.DismissDirtyPrompt(true);
    return;
  }
  prompt_surfaces_.DismissDirtyPrompt(true);
  editor_tabs_.Close(*resolved);
}

void DirtyPromptCoordinator::ConfirmCloseTabs(const DirtyPromptState& prompt) {
  const bool saving = prompt.selected_action == 0;
  // Every dirty target is saved-then-closed, each closing when its OWN write
  // lands. Closing several dirty buffers used to run every formatter inline and
  // wait for all of them, so "Close All" over a handful of JS files froze the
  // window once per file (TD-2026-09-28-304).
  //
  // Per tab rather than all-or-nothing, which is also what VS Code does: a tab
  // whose save is refused (a participant rejected it, or the file changed on
  // disk) stays open with its contents while the others close, instead of
  // cancelling the whole operation after having already written some of them.
  //
  // Re-resolved before each one. Indices shift as tabs close, and they close in
  // whatever order the worker finishes their formatters, so an index captured up
  // front addresses a different tab by the time it is used (TD-2026-07-17-024 is
  // the same hazard with the prompt up).
  //
  // Deferring REQUIRES the stable ids. Without them the close pass below can only
  // address tabs by index, and it cannot tell a tab whose write is still in
  // flight from a clean one — so it would force-close the deferred tab and drop
  // the write. An id-less prompt therefore keeps the old blocking behaviour,
  // which is correct and merely slow.
  const bool can_defer = !prompt.dirty_tab_ids.empty();
  if (saving) {
    if (!can_defer) {
      if (!SaveDirtyTabs(ResolveFocusedTabIndices(prompt.dirty_tab_ids, prompt.dirty_tabs))) {
        return;
      }
    } else {
      for (const std::uint64_t id : prompt.dirty_tab_ids) {
        if (const std::optional<std::size_t> index = ResolveFocusedTabIndexById(id)) {
          SaveThenClose(*index);
        }
      }
    }
  }

  // See ConfirmCloseTab: restore focus so closing non-active dirty tabs cannot
  // strand keyboard input on the now-hidden overlay handler.
  prompt_surfaces_.DismissDirtyPrompt(true);

  // Whatever is left of the target set. On Save that is the CLEAN targets only:
  // a dirty one is owned by its SaveThenClose above, and force-closing it here
  // would either double-close (its id is gone, so the index would address a
  // different tab) or discard the edits of one whose save was refused.
  const auto is_dirty_target = [&prompt](std::uint64_t id) {
    return std::find(prompt.dirty_tab_ids.begin(), prompt.dirty_tab_ids.end(), id) !=
           prompt.dirty_tab_ids.end();
  };
  if (!prompt.target_tab_ids.empty()) {
    for (const std::uint64_t id : prompt.target_tab_ids) {
      if (saving && can_defer && is_dirty_target(id)) {
        continue;
      }
      if (const std::optional<std::size_t> index = ResolveFocusedTabIndexById(id)) {
        editor_tabs_.Close(*index);
      }
    }
    return;
  }
  std::vector<std::size_t> indices = prompt.target_tabs;
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
  const bool switched = !target_was_active && !original_active_root.empty();
  if (prompt.selected_action == 0) {
    // Closing a project flushes every dirty buffer across ALL its editor groups
    // (not just the focused group's tabs captured in prompt.dirty_tabs). The
    // target is active here (already-active, or just switched above). Deferred:
    // a formatter runs off the shell thread and the close waits for its write,
    // behind a progress row with Cancel, rather than the window waiting
    // (TD-2026-09-28-304).
    std::optional<std::vector<std::uint64_t>> waiting = DeferSaveDirtyGroupTabs();
    if (!waiting.has_value()) {
      if (switched) {
        SwitchProjectByRoot(original_active_root);
      }
      return;
    }
    if (!waiting->empty()) {
      prompt_surfaces_.DismissDirtyPrompt(false);
      const std::uint64_t id = context_.save_continuations.Add(SaveContinuation{
          .kind = SaveContinuation::Kind::CloseProject,
          .waiting_tab_ids = std::move(*waiting),
          .project_root = target_root,
          .return_root = original_active_root,
          .switched_to_project = switched,
      });
      if (const SaveContinuation* registered = context_.save_continuations.Find(id)) {
        ShowWaitRow(*registered);
      }
      return;
    }
  }

  prompt_surfaces_.DismissDirtyPrompt(false);
  const auto target_index = FindProjectIndexByRoot(target_root);
  if (!target_index.has_value()) {
    return;
  }
  operations_.close_project(*target_index);

  if (switched) {
    SwitchProjectByRoot(original_active_root);
  }
}

void DirtyPromptCoordinator::ConfirmQuit(const DirtyPromptState& prompt) {
  prompt_surfaces_.DismissDirtyPrompt(false);
  if (prompt.selected_action != 0) {
    quit_requested_ = true;
    return;
  }
  // Save All, then quit — one project at a time, each waiting for its own writes
  // (ContinueQuit). Only projects with unsaved buffers are visited:
  // DirtyGroupTabsForProject inspects a catalog entry's in-memory tabs (all
  // groups) without activating it, so a clean project costs no persist-out /
  // load-in round trip.
  SaveContinuation step{
      .kind = SaveContinuation::Kind::Quit,
      .return_root = context_.current_project_state.root,
  };
  for (std::size_t i = 0; i < context_.project_catalog.entries.size(); ++i) {
    const std::filesystem::path root = context_.ProjectCatalogRoot(i);
    if (!root.empty() && !editor_tabs_.DirtyGroupTabsForProject(i).empty()) {
      step.remaining_roots.push_back(root);
    }
  }
  ContinueQuit(std::move(step));
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
          .notify = [this](NotificationService::Request request) { Notify(std::move(request)); },
          .dismiss_notification =
              [this](std::string_view key) {
                if (notification_service_.DismissKey(key)) {
                  RequestFullRedraw();
                }
              },
          .resume_path_mutation =
              [this](PromptSurfaceState pending) {
                // Built fresh here: the factory's own parameters are not captured.
                EditorTabService& tabs = MakeEditorTabService();
                PromptSurfaceService& surfaces = MakePromptSurfaceService();
                MakePathMutationCoordinator(tabs, surfaces)
                    .ResumeDeferredPathMutation(std::move(pending));
              },
      });
}

}  // namespace microide::workspace
