#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>

#include "project/PatchApplyTypes.h"
#include "project/ProjectBackgroundExecutor.h"
#include "workspace/WorkspaceTabState.h"

namespace microide::workspace {

class GitRepositoryService;

class PatchApplyService {
 public:
  struct Callbacks {
    std::function<project::GitRepositoryState()> current_repository_state;
    std::function<void()> request_git_refresh;
    std::function<void(const std::filesystem::path&)> refresh_compare_tab_for_path;
    std::function<void(const std::filesystem::path&)> invalidate_editor_blame_path;
    std::function<void(const std::filesystem::path&)> reload_clean_editor_tabs_for_path;
    std::function<void(std::string_view)> set_command_feedback;
    std::function<void(project::PatchApplyPreview preview)> open_discard_preview_prompt;
  };

  PatchApplyService(project::ProjectBackgroundExecutor& background_executor,
                    GitRepositoryService& git_repository_service);

  void SetCallbacks(Callbacks callbacks);

  bool CanApplyPatchToCompareTab(const CompareTabState& compare_tab,
                                 project::PatchOperationKind operation) const;

  bool RequestStageHunk(CompareTabState& compare_tab);
  bool RequestStageSelectedLines(CompareTabState& compare_tab);
  bool RequestUnstageHunk(CompareTabState& compare_tab);
  bool RequestUnstageSelectedLines(CompareTabState& compare_tab);
  bool RequestDiscardHunkPreview(CompareTabState& compare_tab);
  bool RequestDiscardSelectedLinesPreview(CompareTabState& compare_tab);

  bool ConfirmPendingDiscard();
  void CancelPendingDiscard();

#ifdef MICROIDE_TESTING
  project::PatchApplyResult ApplyPatchSynchronouslyForTesting(project::PatchApplyRequest request,
                                                              std::string patch_text);
#endif

 private:
  struct PendingDiscard {
    project::PatchApplyRequest request;
    std::string patch_text;
  };

  std::optional<project::PatchApplyRequest> BuildRequest(CompareTabState& compare_tab,
                                                         project::PatchOperationKind operation,
                                                         bool line_scope) const;
  std::optional<std::string> BuildPatchForRequest(const project::PatchApplyRequest& request) const;
  void DispatchApply(project::PatchApplyRequest request, std::string patch_text);
  void PublishResult(project::PatchApplyResult result,
                     const project::PatchApplyRequest& request);
  void ReportResult(const project::PatchApplyResult& result);

  project::ProjectBackgroundExecutor& background_executor_;
  GitRepositoryService& git_repository_service_;
  Callbacks callbacks_;
  mutable std::mutex mutex_;
  std::optional<PendingDiscard> pending_discard_;
  std::uint64_t operation_generation_ = 0;
};

}  // namespace microide::workspace
