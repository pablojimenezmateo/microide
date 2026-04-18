#include "workspace/WorkspaceActionCoordinator.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "workspace/WorkspaceActionRequests.h"

namespace microide::workspace {

WorkspaceShell::ActionCoordinator::DispatchResult WorkspaceShell::ActionCoordinator::ExecuteTab(
    ActionId id,
    const std::vector<std::string>& args,
    ActionSource source,
    std::string* rejection_feedback) {
  const auto reject = [&](std::string feedback) {
    if (rejection_feedback != nullptr) {
      *rejection_feedback = std::move(feedback);
    }
    return DispatchResult::Rejected;
  };

  switch (id) {
    case ActionId::Open: {
      if (shell_.project_root_.empty()) {
        return reject("No active project");
      }
      const std::optional<OpenPathRequest> request = BuildOpenPathRequest(args, shell_.project_root_);
      if (!request.has_value()) {
        return reject("open requires a path");
      }
      const std::filesystem::path& path = request->path;

      auto* editor_tab = shell_.ActiveEditorTab();
      if (editor_tab != nullptr && editor_tab->views.size() > 1) {
        editor::TextViewport opened_view;
        if (!opened_view.OpenFile(path)) {
          return reject("Failed to open file: " + path.string());
        }
        if (!shell_.ReplaceActiveEditorView(opened_view)) {
          return reject("Failed to replace the active split with: " + path.string());
        }
        return DispatchResult::Handled;
      }
      shell_.OpenFile(path);
      return DispatchResult::Handled;
    }
    case ActionId::OpenSelectedTreeItem:
    case ActionId::OpenSelectedTreeItemInNewTab: {
      if (shell_.project_root_.empty()) {
        return reject("No active project");
      }
      const std::filesystem::path path = shell_.ResolveTreeActionPath(source);
      if (path.empty()) {
        return reject("No path selected");
      }
      if (id == ActionId::OpenSelectedTreeItemInNewTab) {
        if (!shell_.OpenFileInNewTab(path)) {
          return reject("Failed to open file in a new tab: " + path.string());
        }
      } else {
        shell_.OpenFile(path);
      }
      return DispatchResult::Handled;
    }
    case ActionId::Tab:
      if (shell_.project_root_.empty()) {
        return reject("No active project");
      }
      {
        const TabPathsRequest request = BuildTabPathsRequest(args, shell_.project_root_);
        if (request.open_untitled) {
          shell_.OpenUntitledTab();
          return DispatchResult::Handled;
        }

        for (const std::filesystem::path& path : request.paths) {
          if (!shell_.OpenFileInNewTab(path)) {
            return reject("Failed to open file in a new tab: " + path.string());
          }
        }

        return DispatchResult::Handled;
      }
    case ActionId::TabSwitch: {
      if (shell_.project_root_.empty()) {
        return reject("No active project");
      }
      std::string error_message;
      const TabSwitchRequest request = BuildTabSwitchRequest(args);
      const std::optional<std::size_t> tab_index =
          shell_.FindTabIndexBySpecifier(request.specifier, &error_message);
      if (!tab_index.has_value()) {
        return reject(error_message.empty() ? "No matching tab" : error_message);
      }
      shell_.ActivateTab(*tab_index);
      return DispatchResult::Handled;
    }
    case ActionId::TabMove:
      if (shell_.project_root_.empty()) {
        return reject("No active project");
      }
      if (shell_.open_tabs_.empty()) {
        return reject("No open tabs");
      }
      {
        const std::optional<TabMoveRequest> request = BuildTabMoveRequest(args);
        if (!request.has_value()) {
          return reject("tabmove requires a tab slot or relative offset");
        }
        const int current_slot = static_cast<int>(shell_.active_tab_index_) + 1;
        const int requested_slot = request->relative ? current_slot + request->slot : request->slot;
        const int clamped_slot =
            std::clamp(requested_slot, 1, static_cast<int>(shell_.open_tabs_.size()));
        shell_.MoveActiveTabTo(static_cast<std::size_t>(clamped_slot - 1));
        return DispatchResult::Handled;
      }
    case ActionId::Reopen:
      if (shell_.project_root_.empty()) {
        return reject("No active project");
      }
      shell_.ReopenActiveTab();
      return DispatchResult::Handled;
    case ActionId::Save:
      if (shell_.project_root_.empty()) {
        return reject("No active project");
      }
      if (shell_.SaveTab(shell_.active_tab_index_)) {
        if (source == ActionSource::Shortcut) {
          shell_.ResetCaretBlink();
        }
      } else {
        return reject("Save failed");
      }
      return DispatchResult::Handled;
    case ActionId::Vsplit: {
      if (shell_.project_root_.empty()) {
        return reject("No active project");
      }
      const EditorSplitOrientation orientation = EditorSplitOrientation::Vertical;
      const TabPathsRequest request = BuildTabPathsRequest(args, shell_.project_root_);

      if (request.open_untitled) {
        shell_.SplitActiveEditor(orientation);
        return DispatchResult::Handled;
      }

      for (const std::filesystem::path& path : request.paths) {
        editor::TextViewport opened_view;
        if (!opened_view.OpenFile(path)) {
          return reject("Failed to open file: " + path.string());
        }
        if (!shell_.SplitActiveEditor(orientation)) {
          return reject("Failed to split the active editor");
        }
        if (!shell_.ReplaceActiveEditorView(opened_view)) {
          return reject("Failed to replace the active split with: " + path.string());
        }
      }

      return DispatchResult::Handled;
    }
    case ActionId::Unsplit:
      shell_.UnsplitActiveEditor();
      return DispatchResult::Handled;
    case ActionId::SplitNext:
      shell_.CycleEditorSplit(1);
      return DispatchResult::Handled;
    case ActionId::SplitPrev:
      shell_.CycleEditorSplit(-1);
      return DispatchResult::Handled;
    case ActionId::SplitFirst:
      shell_.ActivateOrderedEditorSplit(0);
      return DispatchResult::Handled;
    case ActionId::SplitLast: {
      auto* editor_tab = shell_.ActiveEditorTab();
      const std::size_t last_index =
          editor_tab == nullptr || editor_tab->views.empty() ? 0 : editor_tab->views.size() - 1;
      shell_.ActivateOrderedEditorSplit(last_index);
      return DispatchResult::Handled;
    }
    case ActionId::CloseActiveTab:
      if (!shell_.open_tabs_.empty()) {
        shell_.RequestCloseTab(shell_.active_tab_index_);
      }
      return DispatchResult::Handled;
    case ActionId::CloseAllTabs:
      shell_.CloseAllTabs();
      return DispatchResult::Handled;
    case ActionId::CloseOtherTabs: {
      std::vector<std::size_t> indices;
      if (!shell_.open_tabs_.empty()) {
        indices.reserve(shell_.open_tabs_.size() - 1);
      }
      for (std::size_t i = 0; i < shell_.open_tabs_.size(); ++i) {
        if (i != shell_.active_tab_index_) {
          indices.push_back(i);
        }
      }
      shell_.RequestCloseTabs(std::move(indices));
      return DispatchResult::Handled;
    }
    case ActionId::CloseTabsToRight: {
      std::vector<std::size_t> indices;
      for (std::size_t i = shell_.active_tab_index_ + 1; i < shell_.open_tabs_.size(); ++i) {
        indices.push_back(i);
      }
      shell_.RequestCloseTabs(std::move(indices));
      return DispatchResult::Handled;
    }
    case ActionId::CloseTabsToLeft: {
      std::vector<std::size_t> indices;
      indices.reserve(shell_.active_tab_index_);
      for (std::size_t i = 0; i < shell_.active_tab_index_; ++i) {
        indices.push_back(i);
      }
      shell_.RequestCloseTabs(std::move(indices));
      return DispatchResult::Handled;
    }
    default:
      return DispatchResult::Unhandled;
  }
}

}  // namespace microide::workspace
