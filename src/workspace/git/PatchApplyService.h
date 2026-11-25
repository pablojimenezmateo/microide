#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>

#include "platform/ProcessLauncher.h"
#include "project/PatchApplyTypes.h"
#include "project/ProjectBackgroundExecutor.h"
#include "util/MainThreadMailbox.h"
#include "workspace/state/WorkspaceTabState.h"

namespace microide::workspace {

class GitRepositoryService;

class PatchApplyService {
 public:
  struct Callbacks {
    std::function<project::GitRepositoryState()> current_repository_state;
    std::function<const platform::ProcessLauncher*()> project_launcher;
    std::function<void()> request_git_refresh;
    std::function<void(const std::filesystem::path&)> refresh_compare_tab_for_path;
    std::function<void(const std::filesystem::path&)> invalidate_editor_blame_path;
    std::function<void(const std::filesystem::path&)> reload_clean_editor_tabs_for_path;
    std::function<void(std::string_view)> set_command_feedback;
    std::function<std::filesystem::path()> front_project_root;
    std::function<bool(const std::filesystem::path& project_root)> is_in_front;
    std::function<std::string(const std::filesystem::path& project_root)> project_label;
    std::function<void(const std::filesystem::path& project_root)> defer_to_project;
    std::function<void(project::PatchApplyPreview preview)> open_discard_preview_prompt;
  };

  PatchApplyService(project::ProjectBackgroundExecutor& background_executor,
                    GitRepositoryService& git_repository_service);

  void SetCallbacks(Callbacks callbacks);

  void SetCompletionWakeEvent(std::uint32_t event_type);
  void DrainCompletions();
  int PendingCompletionCount() const { return completion_mailbox_.PendingCount(); }

  static bool CanApplyPatchToCompareTab(const CompareTabState& compare_tab,
                                        project::PatchOperationKind operation);

  bool RequestStageHunk(CompareTabState& compare_tab);
  bool RequestStageSelectedLines(CompareTabState& compare_tab);
  bool RequestUnstageHunk(CompareTabState& compare_tab);
  bool RequestUnstageSelectedLines(CompareTabState& compare_tab);
  bool RequestDiscardHunkPreview(CompareTabState& compare_tab);
  bool RequestDiscardSelectedLinesPreview(CompareTabState& compare_tab);

  bool ConfirmPendingDiscard();
  void CancelPendingDiscard();

  std::optional<project::PatchApplyRequest> BuildRequestForTesting(
      CompareTabState& compare_tab, project::PatchOperationKind operation, bool line_scope) const {
    return BuildRequest(compare_tab, operation, line_scope);
  }

 private:
  struct PendingDiscard {
    project::PatchApplyRequest request;
    std::string patch_text;
  };

  struct ResolvedPatch {
    project::PatchApplyRequest request;
    std::string patch_text;
  };

  std::optional<ResolvedPatch> ResolvePatch(CompareTabState& compare_tab,
                                            project::PatchOperationKind operation,
                                            bool line_scope,
                                            std::string_view unavailable_detail,
                                            std::string_view empty_patch_detail);
  std::optional<project::PatchApplyRequest> BuildRequest(CompareTabState& compare_tab,
                                                         project::PatchOperationKind operation,
                                                         bool line_scope) const;
  std::optional<std::string> BuildPatchForRequest(const project::PatchApplyRequest& request,
                                                  const compare::CompareModel& model) const;
  void DispatchApply(project::PatchApplyRequest request, std::string patch_text);
  bool ResolveAndDispatch(CompareTabState& compare_tab, project::PatchOperationKind kind,
                          bool selected_lines, std::string_view unavailable_detail,
                          std::string_view empty_patch_detail);
  bool ResolveAndPreviewDiscard(CompareTabState& compare_tab, project::PatchOperationKind kind,
                                bool selected_lines, std::string_view unavailable_detail,
                                std::string_view empty_patch_detail, std::string summary);
  void ReportResult(const project::PatchApplyResult& result, std::string_view project_suffix = {});
  bool RejectIgnoreWhitespaceApply(const CompareTabState& compare_tab);

  project::ProjectBackgroundExecutor& background_executor_;
  GitRepositoryService& git_repository_service_;
  Callbacks callbacks_;
  util::MainThreadMailbox completion_mailbox_;
  mutable std::mutex mutex_;
  std::optional<PendingDiscard> pending_discard_;
};

}
