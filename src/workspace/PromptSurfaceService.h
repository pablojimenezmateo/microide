#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "workspace/WorkspacePromptState.h"

namespace microide::workspace {

class ProjectWorkspaceState;

class PromptSurfaceService {
 public:
  struct Operations {
    std::function<void()> request_prompt_redraw;
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
  void DismissDirtyPrompt(bool restore_focus);
  void DismissPromptSurface(bool restore_focus);
  void OpenPromptSurface(PromptSurfaceState::Action action,
                         PromptSurfaceState::Kind kind,
                         const std::filesystem::path& path,
                         std::string input);
  void OpenExternalUrlPrompt(std::string url);

 private:
  ProjectWorkspaceState& state_;
  PromptState& prompts_;
  Operations operations_;
};

}  // namespace microide::workspace
