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
    std::function<void(NotificationService::Request)> notify;
    std::function<void(std::string_view)> dismiss_notification;
    std::function<void(PromptSurfaceState)> resume_path_mutation;
  };

  static constexpr std::string_view kQuitWaitKey = "save.wait.quit";
  static constexpr std::string_view kCloseProjectWaitKey = "save.wait.close-project";

  DirtyPromptCoordinator(WorkspaceContext& context,
                         bool& quit_requested,
                         EditorTabService& editor_tabs,
                         PromptSurfaceService& prompt_surfaces,
                         Operations operations);

  void Confirm();

  bool SaveThenClose(std::size_t index);

  void SettleSave(std::uint64_t tab_id, bool saved);
  void SettleOrphanedRun(std::uint64_t run_id);
  bool CancelSaveContinuation(std::uint64_t id);

 private:
  std::optional<std::size_t> FindProjectIndexByRoot(const std::filesystem::path& root) const;
  bool SaveDirtyTabs(std::span<const std::size_t> tab_indices);
  bool SwitchProjectByRoot(const std::filesystem::path& root);
  void ConfirmCloseTab(const DirtyPromptState& prompt);
  void ConfirmCloseTabs(const DirtyPromptState& prompt);
  void ConfirmCloseProject(const DirtyPromptState& prompt);
  void ConfirmQuit(const DirtyPromptState& prompt);
  void FocusTargetGroup(std::uint64_t tab_id);
  std::optional<std::size_t> ResolveFocusedTabIndexById(std::uint64_t id) const;
  std::vector<std::size_t> ResolveFocusedTabIndices(
      const std::vector<std::uint64_t>& ids,
      const std::vector<std::size_t>& fallback_indices) const;

  void RunContinuation(SaveContinuation continuation);
  void CancelContinuation(const SaveContinuation& continuation, std::string_view reason);
  std::optional<std::vector<std::uint64_t>> DeferSaveDirtyGroupTabs();
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

}
