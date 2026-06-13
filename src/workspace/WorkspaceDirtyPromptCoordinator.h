#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "workspace/EditorTabService.h"
#include "workspace/WorkspaceContext.h"

namespace microide::workspace {

class PromptSurfaceService;

class DirtyPromptCoordinator {
 public:
  struct Operations {
    std::function<void(bool)> confirm_path_prompt;
    std::function<bool(std::size_t, bool)> switch_project;
    std::function<void(std::size_t)> close_project;
  };

  DirtyPromptCoordinator(WorkspaceContext& context,
                         bool& quit_requested,
                         EditorTabService& editor_tabs,
                         PromptSurfaceService& prompt_surfaces,
                         Operations operations);

  void Confirm();

 private:
  std::optional<std::size_t> FindProjectIndexByRoot(const std::filesystem::path& root) const;
  bool SaveDirtyTabs(std::span<const std::size_t> tab_indices);
  bool SwitchProjectByRoot(const std::filesystem::path& root);
  void ConfirmCloseTab(const DirtyPromptState& prompt);
  void ConfirmCloseTabs(const DirtyPromptState& prompt);
  void ConfirmCloseProject(const DirtyPromptState& prompt);
  void ConfirmQuit(const DirtyPromptState& prompt);

  WorkspaceContext& context_;
  bool& quit_requested_;
  EditorTabService& editor_tabs_;
  PromptSurfaceService& prompt_surfaces_;
  Operations operations_;
};

}  // namespace microide::workspace
