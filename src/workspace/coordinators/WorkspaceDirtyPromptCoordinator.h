#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "workspace/services/EditorTabService.h"
#include "workspace/services/NotificationService.h"
#include "workspace/state/SaveContinuationState.h"
#include "workspace/WorkspaceContext.h"

namespace microide::workspace {

class PromptSurfaceService;

class DirtyPromptCoordinator {
 public:
  struct Operations {
    std::function<void(bool)> confirm_path_prompt;
    std::function<bool(std::size_t, bool)> switch_project;
    std::function<void(std::size_t)> close_project;
    // Post or update a notification row (the quit / close-project progress row).
    std::function<void(NotificationService::Request)> notify;
    std::function<void(std::string_view)> dismiss_notification;
    // Replay a rename/delete once its writes landed (PathMutationCoordinator).
    std::function<void(PromptSurfaceState)> resume_path_mutation;
  };

  // Notification keys of the two wait rows, so a test or the control channel can
  // find them.
  static constexpr std::string_view kQuitWaitKey = "save.wait.quit";
  static constexpr std::string_view kCloseProjectWaitKey = "save.wait.close-project";

  DirtyPromptCoordinator(WorkspaceContext& context,
                         bool& quit_requested,
                         EditorTabService& editor_tabs,
                         PromptSurfaceService& prompt_surfaces,
                         Operations operations);

  void Confirm();

  // Save `index` and close it once the write lands: at once when nothing was
  // deferred, as a CloseTab save continuation when a formatter is running.
  // False when the save was refused (the tab stays open).
  bool SaveThenClose(std::size_t index);

  // A tab's deferred save finished (ApplyDeferredSaveFormat), or will never
  // finish (its tab vanished): run what was waiting on it, or cancel it.
  void SettleSave(std::uint64_t tab_id, bool saved);
  // A formatter run finished whose tab is not in the active project: the user
  // switched projects while it ran, or closed the tab. The write is abandoned
  // (the buffer stays dirty in its project, and the user is told), and whatever
  // waited on it is cancelled rather than left waiting forever.
  void SettleOrphanedRun(std::uint64_t run_id);
  // The Cancel button of a quit / close-project wait. The saves still formatting
  // are abandoned — their buffers stay open and dirty — and nothing further runs.
  bool CancelSaveContinuation(std::uint64_t id);

 private:
  std::optional<std::size_t> FindProjectIndexByRoot(const std::filesystem::path& root) const;
  bool SaveDirtyTabs(std::span<const std::size_t> tab_indices);
  bool SwitchProjectByRoot(const std::filesystem::path& root);
  void ConfirmCloseTab(const DirtyPromptState& prompt);
  void ConfirmCloseTabs(const DirtyPromptState& prompt);
  void ConfirmCloseProject(const DirtyPromptState& prompt);
  void ConfirmQuit(const DirtyPromptState& prompt);
  // Resolve a stable focused-group tab id (TD-2026-07-17-024) to its current index;
  // nullopt if the tab has since closed. Keeps CloseTab/CloseTabs from acting on a
  // stale index after a close/reorder while the modal prompt was up.
  std::optional<std::size_t> ResolveFocusedTabIndexById(std::uint64_t id) const;
  // Resolve a list of ids to current indices (dropping closed tabs). Falls back to
  // `fallback_indices` only when no ids were captured (id-less legacy prompt).
  std::vector<std::size_t> ResolveFocusedTabIndices(
      const std::vector<std::uint64_t>& ids,
      const std::vector<std::size_t>& fallback_indices) const;

  // Save continuations (WorkspaceDirtyPromptCoordinatorContinuations.cpp).
  void RunContinuation(SaveContinuation continuation);
  void CancelContinuation(const SaveContinuation& continuation, std::string_view reason);
  // Defer-save every dirty tab of the active project. The ids of tabs whose
  // write is still formatting; nullopt when a save was refused.
  std::optional<std::vector<std::uint64_t>> DeferSaveDirtyGroupTabs();
  // Save the next project in `step.remaining_roots`, or — when none is left —
  // return to `step.return_root` and quit.
  void ContinueQuit(SaveContinuation step);
  void ShowWaitRow(const SaveContinuation& continuation);
  void RefreshWaitRows();
  void ReturnTo(const std::filesystem::path& root);

  WorkspaceContext& context_;
  bool& quit_requested_;
  EditorTabService& editor_tabs_;
  PromptSurfaceService& prompt_surfaces_;
  Operations operations_;
};

}  // namespace microide::workspace
