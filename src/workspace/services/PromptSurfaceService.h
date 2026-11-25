#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "workspace/state/WorkspacePromptState.h"

namespace microide::workspace {

struct ProjectWorkspaceState;

class PromptSurfaceService {
 public:
  struct Operations {
    std::function<void()> request_frame;
  };

  PromptSurfaceService(ProjectWorkspaceState& state, PromptState& prompts, Operations operations);

  void ShowDirtyPromptForTab(std::size_t index);
  void ShowDirtyPromptForTabs(std::vector<std::size_t> target_tabs,
                              std::vector<std::size_t> dirty_tabs);
  void ShowDirtyPromptForProject(std::size_t index,
                                 std::vector<std::size_t> dirty_tabs,
                                 std::size_t dirty_count);
  void ShowDirtyPromptForQuit(std::size_t active_tab_index,
                              std::size_t active_project_index,
                              std::vector<std::size_t> dirty_tabs,
                              std::size_t dirty_count);
  void ShowDirtyPathPrompt(DirtyPromptState::Kind kind,
                           std::vector<std::size_t> dirty_tabs,
                           std::size_t dirty_count,
                           const std::filesystem::path& path);
  void DismissDirtyPrompt(bool restore_focus);
  void OnTabClosed(std::uint64_t stable_id);
  FocusTarget FocusAfterPrompt(FocusTarget previous) const;
  void DismissPromptSurface(bool restore_focus);
  void CancelOpenPrompts();
  void OpenPromptSurface(PromptSurfaceState::Action action,
                         PromptSurfaceState::Kind kind,
                         const std::filesystem::path& path,
                         std::string input);
  struct ReviewCommentPrompt {
    std::filesystem::path path;
    std::string text;
    compare::BranchReviewTargetIdentity target;
    std::optional<compare::BranchReviewHunkIdentity> hunk;
    std::optional<compare::BranchReviewHunkIdentity> replaced_hunk;
    std::string detail;
  };
  void OpenReviewCommentPrompt(ReviewCommentPrompt prompt);
  void OpenExternalUrlPrompt(std::string url);
  void OpenConfirmCommandPrompt(std::string command_line, std::string question);

 private:
  void ShowDirtyPrompt(DirtyPromptState prompt);
  std::uint64_t EnsureFocusedTabStableId(std::size_t index);
  std::vector<std::uint64_t> StableIdsForFocusedTabs(const std::vector<std::size_t>& indices);

  ProjectWorkspaceState& state_;
  PromptState& prompts_;
  Operations operations_;
};

}
