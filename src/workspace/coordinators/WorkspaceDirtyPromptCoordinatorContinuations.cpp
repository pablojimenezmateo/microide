// Save continuations: what runs once deferred (format-on-save) writes land — a
// tab's close, a rename/delete, a project close, a quit. See SaveContinuationState.h
// for why they are data and keyed by stable tab ids; this TU is the one place that
// runs or cancels them (TD-2026-09-28-304).
#include "workspace/coordinators/WorkspaceDirtyPromptCoordinator.h"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace microide::workspace {
namespace {

// Where a tab with this stable id sits in the active project, if anywhere.
struct TabLocation {
  std::size_t group_index = 0;
  std::size_t tab_index = 0;
};

std::optional<TabLocation> FindTab(const ProjectWorkspaceState& state, std::uint64_t id) {
  for (std::size_t g = 0; g < state.editor_groups.size(); ++g) {
    const auto& tabs = state.editor_groups[g].open_tabs;
    for (std::size_t t = 0; t < tabs.size(); ++t) {
      if (tabs[t].stable_id == id) {
        return TabLocation{g, t};
      }
    }
  }
  return std::nullopt;
}

std::string_view WaitRowKey(SaveContinuation::Kind kind) {
  return kind == SaveContinuation::Kind::Quit ? DirtyPromptCoordinator::kQuitWaitKey
                                              : DirtyPromptCoordinator::kCloseProjectWaitKey;
}

bool HasWaitRow(SaveContinuation::Kind kind) {
  return kind == SaveContinuation::Kind::Quit || kind == SaveContinuation::Kind::CloseProject;
}

}  // namespace

bool DirtyPromptCoordinator::SaveThenClose(std::size_t index) {
  const TabCoordinator::SaveForCloseResult result = editor_tabs_.SaveForClose(index);
  if (!result.saved) {
    return false;
  }
  if (result.deferred_tab_id != 0) {
    context_.save_continuations.Add(SaveContinuation{
        .kind = SaveContinuation::Kind::CloseTab,
        .waiting_tab_ids = {result.deferred_tab_id},
        .project_root = context_.current_project_state.root,
        .tab_id = result.deferred_tab_id,
    });
  }
  return true;
}

void DirtyPromptCoordinator::SettleSave(std::uint64_t tab_id, bool saved) {
  SaveContinuationQueue::Settled settled = context_.save_continuations.Complete(tab_id, saved);
  for (const SaveContinuation& continuation : settled.cancelled) {
    CancelContinuation(continuation, "a file could not be saved");
  }
  for (SaveContinuation& continuation : settled.ready) {
    RunContinuation(std::move(continuation));
  }
  RefreshWaitRows();
}

void DirtyPromptCoordinator::SettleOrphanedRun(std::uint64_t run_id) {
  for (const auto& entry : context_.project_catalog.entries) {
    if (entry == nullptr) {
      continue;
    }
    for (EditorGroup& group : entry->editor_groups) {
      for (TabEntry& tab : group.open_tabs) {
        if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value() ||
            !tab.editor_state->pending_format_save.Holds(run_id)) {
          continue;
        }
        tab.editor_state->pending_format_save.Disarm();
        if (operations_.notify) {
          operations_.notify(NotificationService::Request{
              .tone = NotificationService::Tone::Warning,
              .message = "Saving " + tab.editor_state->viewport.path().filename().string() +
                         " was interrupted by switching projects; it is still unsaved"});
        }
        SettleSave(tab.stable_id, false);
        return;
      }
    }
  }
  // Gone entirely. Any wait on a tab that no longer has a run in flight anywhere
  // can never be settled by a completion, so settle it as failed now.
  const auto in_flight = [this](std::uint64_t id) {
    const auto holds = [id](const ProjectWorkspaceState& state) {
      for (const EditorGroup& group : state.editor_groups) {
        for (const TabEntry& tab : group.open_tabs) {
          if (tab.stable_id == id && tab.editor_state.has_value() &&
              tab.editor_state->pending_format_save.armed()) {
            return true;
          }
        }
      }
      return false;
    };
    if (holds(context_.current_project_state)) {
      return true;
    }
    for (const auto& entry : context_.project_catalog.entries) {
      if (entry != nullptr && holds(*entry)) {
        return true;
      }
    }
    return false;
  };
  std::vector<std::uint64_t> orphaned;
  for (const SaveContinuation& continuation : context_.save_continuations.All()) {
    for (const std::uint64_t id : continuation.waiting_tab_ids) {
      if (!in_flight(id)) {
        orphaned.push_back(id);
      }
    }
  }
  for (const std::uint64_t id : orphaned) {
    SettleSave(id, false);
  }
}

bool DirtyPromptCoordinator::CancelSaveContinuation(std::uint64_t id) {
  std::optional<SaveContinuation> removed = context_.save_continuations.Remove(id);
  if (!removed.has_value()) {
    return false;
  }
  // Abandon the writes still formatting: disarmed, their completions find no tab
  // and are dropped, so the buffers stay open and dirty — the user said Cancel,
  // and a save that lands after Cancel is a save they did not ask for any more.
  // Anything else waiting on those tabs (a tab close) is cancelled with them.
  if (removed->project_root == context_.current_project_state.root) {
    for (const std::uint64_t tab_id : removed->waiting_tab_ids) {
      if (const std::optional<TabLocation> where =
              FindTab(context_.current_project_state, tab_id)) {
        TabEntry& tab = context_.current_project_state.editor_groups[where->group_index]
                            .open_tabs[where->tab_index];
        if (tab.editor_state.has_value()) {
          tab.editor_state->pending_format_save.Disarm();
        }
      }
      for (const SaveContinuation& other :
           context_.save_continuations.Complete(tab_id, false).cancelled) {
        CancelContinuation(other, "its save was cancelled");
      }
    }
  }
  if (HasWaitRow(removed->kind) && operations_.dismiss_notification) {
    operations_.dismiss_notification(WaitRowKey(removed->kind));
  }
  ReturnTo(removed->return_root);
  if (operations_.notify) {
    operations_.notify(NotificationService::Request{
        .message = removed->kind == SaveContinuation::Kind::Quit ? "Quit cancelled"
                                                                 : "Close project cancelled"});
  }
  RefreshWaitRows();
  return true;
}

void DirtyPromptCoordinator::RunContinuation(SaveContinuation continuation) {
  // Never against another project: the tabs it waited on may share ids with
  // nothing here, and "close the project" must not close whichever is active.
  if (continuation.project_root != context_.current_project_state.root) {
    CancelContinuation(continuation, "the project was switched while it saved");
    return;
  }
  switch (continuation.kind) {
    case SaveContinuation::Kind::CloseTab:
      if (const std::optional<TabLocation> where =
              FindTab(context_.current_project_state, continuation.tab_id)) {
        editor_tabs_.CloseGroupTab(where->group_index, where->tab_index);
      }
      return;
    case SaveContinuation::Kind::PathMutation:
      if (continuation.path_mutation.has_value() && operations_.resume_path_mutation) {
        operations_.resume_path_mutation(std::move(*continuation.path_mutation));
      }
      return;
    case SaveContinuation::Kind::CloseProject: {
      if (operations_.dismiss_notification) {
        operations_.dismiss_notification(kCloseProjectWaitKey);
      }
      // A buffer edited while its formatter ran is dirty again: the close goes back
      // to asking rather than discarding it.
      if (!editor_tabs_.DirtyGroupTabs().empty()) {
        CancelContinuation(continuation, "a file was edited while it saved");
        return;
      }
      if (const std::optional<std::size_t> index =
              FindProjectIndexByRoot(continuation.project_root)) {
        operations_.close_project(*index);
      }
      if (continuation.switched_to_project) {
        ReturnTo(continuation.return_root);
      }
      return;
    }
    case SaveContinuation::Kind::Quit:
      ContinueQuit(std::move(continuation));
      return;
  }
}

void DirtyPromptCoordinator::CancelContinuation(const SaveContinuation& continuation,
                                                std::string_view reason) {
  const auto warn = [this](std::string message) {
    if (operations_.notify) {
      operations_.notify(NotificationService::Request{
          .tone = NotificationService::Tone::Warning, .message = std::move(message)});
    }
  };
  switch (continuation.kind) {
    case SaveContinuation::Kind::CloseTab:
      // The failed save has said why; the tab staying open with its contents is
      // the whole of the answer.
      return;
    case SaveContinuation::Kind::PathMutation:
      warn("The rename/delete was not applied: " + std::string(reason));
      return;
    case SaveContinuation::Kind::CloseProject:
    case SaveContinuation::Kind::Quit:
      if (operations_.dismiss_notification) {
        operations_.dismiss_notification(WaitRowKey(continuation.kind));
      }
      if (continuation.kind == SaveContinuation::Kind::Quit ||
          continuation.switched_to_project) {
        ReturnTo(continuation.return_root);
      }
      warn((continuation.kind == SaveContinuation::Kind::Quit ? "Quit cancelled: "
                                                               : "Close project cancelled: ") +
           std::string(reason));
      return;
  }
}

std::optional<std::vector<std::uint64_t>> DirtyPromptCoordinator::DeferSaveDirtyGroupTabs() {
  std::vector<std::uint64_t> deferred;
  // Indices stay valid across the loop: a save, deferred or not, closes nothing.
  for (const GroupTabRef& ref : editor_tabs_.DirtyGroupTabs()) {
    if (!editor_tabs_.SaveGroupTab(ref.group_index, ref.tab_index, SaveMode::Deferred)) {
      return std::nullopt;
    }
    TabEntry& tab =
        context_.current_project_state.editor_groups[ref.group_index].open_tabs[ref.tab_index];
    if (tab.editor_state.has_value() && tab.editor_state->pending_format_save.armed()) {
      deferred.push_back(EnsureTabStableId(tab));
    }
  }
  return deferred;
}

void DirtyPromptCoordinator::ContinueQuit(SaveContinuation step) {
  // One project at a time, staying on it until its writes land: a completion is
  // applied to the ACTIVE project's tabs, so switching on before they land would
  // strand them (they would be reported as interrupted, and the quit cancelled).
  while (!step.remaining_roots.empty()) {
    const std::filesystem::path root = std::move(step.remaining_roots.front());
    step.remaining_roots.erase(step.remaining_roots.begin());
    if (root != context_.current_project_state.root && !SwitchProjectByRoot(root)) {
      continue;  // the project went away while the quit waited
    }
    std::optional<std::vector<std::uint64_t>> waiting = DeferSaveDirtyGroupTabs();
    if (!waiting.has_value()) {
      CancelContinuation(step, "a file could not be saved");
      return;
    }
    if (!waiting->empty()) {
      step.project_root = root;
      step.total_tab_count = waiting->size();
      step.waiting_tab_ids = std::move(*waiting);
      const std::uint64_t id = context_.save_continuations.Add(std::move(step));
      if (const SaveContinuation* registered = context_.save_continuations.Find(id)) {
        ShowWaitRow(*registered);
      }
      return;
    }
  }
  if (operations_.dismiss_notification) {
    operations_.dismiss_notification(kQuitWaitKey);
  }
  ReturnTo(step.return_root);
  quit_requested_ = true;
}

void DirtyPromptCoordinator::ShowWaitRow(const SaveContinuation& continuation) {
  if (!HasWaitRow(continuation.kind) || !operations_.notify) {
    return;
  }
  const std::size_t waiting = continuation.waiting_tab_ids.size();
  const std::size_t total = std::max(continuation.total_tab_count, waiting);
  const std::string files = waiting == 1 ? "1 file" : std::to_string(waiting) + " files";
  NotificationService::Request request{
      .key = std::string(WaitRowKey(continuation.kind)),
      .message = "Saving " + files +
                 (continuation.kind == SaveContinuation::Kind::Quit ? " before quitting…"
                                                                    : " before closing the project…"),
      .sticky = true,
      .progress = total == 0 ? 0.0f
                             : static_cast<float>(total - waiting) / static_cast<float>(total),
  };
  request.actions.push_back(NotificationAction{
      .label = "Cancel",
      .id = ActionId::CancelSaveWait,
      .args = {std::to_string(continuation.id)},
  });
  operations_.notify(std::move(request));
}

void DirtyPromptCoordinator::RefreshWaitRows() {
  for (const SaveContinuation& continuation : context_.save_continuations.All()) {
    ShowWaitRow(continuation);
  }
}

void DirtyPromptCoordinator::ReturnTo(const std::filesystem::path& root) {
  if (!root.empty() && root != context_.current_project_state.root) {
    SwitchProjectByRoot(root);
  }
}

}  // namespace microide::workspace
