#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

#include "compare/CompareReviewTypes.h"
#include "workspace/render/CompareReviewControls.h"
#include "workspace/services/PromptSurfaceService.h"
#include "workspace/state/WorkspacePromptState.h"
#include "workspace/state/WorkspaceProjectState.h"
#include "workspace/state/WorkspaceTabState.h"

namespace microide::workspace {

class CompareReviewService {
 public:
  struct Operations {
    std::function<CompareTabState*()> active_compare_tab;
    std::function<void()> persist;
    std::function<void()> request_frame;
    std::function<void()> request_sidebar_redraw;
    std::function<void(PromptSurfaceService::ReviewCommentPrompt)> open_comment_prompt;
    std::function<void(int)> jump_compare_hunk;
  };

  CompareReviewService(ProjectWorkspaceState& state, const PromptState& prompts, Operations operations);

  void SetSelectedHunkReviewed(bool reviewed);
  void SetActiveFileReviewed(bool reviewed);
  void EditSelectedNote(const std::string& note_text);
  void ClearReview();

  void SetHunkReviewed(CompareTabState& compare_tab, int hunk, bool reviewed);
  void SetFileReviewed(CompareTabState& compare_tab, bool reviewed);
  void OpenCommentPrompt(CompareTabState& compare_tab, int note);
  void CommitCommentPrompt();
  void DeleteComment(CompareTabState& compare_tab, int note);
  bool RunControl(CompareTabState& compare_tab, const CompareControlHit& hit);

  std::optional<compare::BranchReviewTargetIdentity> ExportTarget() const;
  std::size_t CommentCount(const std::filesystem::path& scope = {}) const;
  std::string CommentsAsReferences(const std::filesystem::path& scope = {}) const;

 private:
  void MarksChanged();

  CompareTabState* ActiveTab() const;
  CompareTabState* ActiveReviewTab() const;

  ProjectWorkspaceState& state_;
  const PromptState& prompts_;
  Operations operations_;
};

std::optional<compare::BranchReviewTargetIdentity> ReviewExportTarget(const ProjectWorkspaceState& state,
                                                                      const CompareTabState* active_compare_tab);
std::size_t ReviewCommentCount(const ProjectWorkspaceState& state,
                               const CompareTabState* active_compare_tab,
                               const std::filesystem::path& scope);

}
