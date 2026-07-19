#pragma once

#include <cstddef>
#include <cstdint>
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
  // All-groups save used by the quit / close-project paths (VSCode "Save All").
  bool SaveDirtyGroupTabs(std::span<const GroupTabRef> refs);
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

  WorkspaceContext& context_;
  bool& quit_requested_;
  EditorTabService& editor_tabs_;
  PromptSurfaceService& prompt_surfaces_;
  Operations operations_;
};

}  // namespace microide::workspace
