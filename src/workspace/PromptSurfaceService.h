#pragma once

#include <filesystem>
#include <functional>
#include <string>

#include "workspace/WorkspacePromptState.h"

namespace microide::workspace {

class ProjectWorkspaceState;

class PromptSurfaceService {
 public:
  struct Operations {
    std::function<void()> request_prompt_redraw;
  };

  PromptSurfaceService(ProjectWorkspaceState& state, PromptState& prompts, Operations operations);

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
