#include "util/Log.h"
#include "workspace/coordinators/WorkspaceDirtyPromptCoordinator.h"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>
#include "util/InvokeIfSet.h"

namespace microide::workspace {
namespace {

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

}

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
  const auto in_flight = [this](std::uint64_t id) {
    const auto holds = [id](const ProjectWorkspaceState& state) {
      for (const EditorGroup& group : state.editor_groups) {
        for (const TabEntry& tab : group.open_tabs) {
          if (tab.stable_id == id && tab.editor_state.has_value() && tab.editor_state->save_pending()) {
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
      util::InvokeIfSet(operations_.dismiss_notification, kCloseProjectWaitKey);
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
  for (const GroupTabRef& ref : editor_tabs_.DirtyGroupTabs()) {
    if (!editor_tabs_.SaveGroupTab(ref.group_index, ref.tab_index, SaveMode::Deferred)) {
      return std::nullopt;
    }
    TabEntry& tab =
        context_.current_project_state.editor_groups[ref.group_index].open_tabs[ref.tab_index];
    if (tab.editor_state.has_value() && tab.editor_state->save_pending()) {
      deferred.push_back(EnsureTabStableId(tab));
    }
  }
  return deferred;
}

void DirtyPromptCoordinator::ContinueQuit(SaveContinuation step) {
  while (!step.remaining_roots.empty()) {
    const std::filesystem::path root = std::move(step.remaining_roots.front());
    step.remaining_roots.erase(step.remaining_roots.begin());
    if (root != context_.current_project_state.root && !SwitchProjectByRoot(root)) {
      continue;
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
  util::InvokeIfSet(operations_.dismiss_notification, kQuitWaitKey);
  ReturnTo(step.return_root);
  util::Log("microide quit: the unsaved-changes prompt, after Save All");
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

}
