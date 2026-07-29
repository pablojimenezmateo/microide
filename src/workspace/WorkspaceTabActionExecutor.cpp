#include "workspace/WorkspaceActionCoordinator.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "workspace/WorkspaceActionRequests.h"
#include "workspace/WorkspacePathUtils.h"
#include "workspace/WorkspaceSidebarRegistry.h"

namespace microide::workspace {

ActionCoordinator::DispatchResult ActionCoordinator::ExecuteTab(ActionId id,
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
      const std::optional<OpenPathRequest> request =
          context_.HasProjectRoot()
              ? BuildOpenPathRequest(args, context_.ProjectRoot())
              : BuildOpenPathRequest(args, std::filesystem::path{});
      if (!request.has_value()) {
        // Bare `open` from a UI surface (File > Open File…, Ctrl+O, the welcome
        // screen's Open File action) means "ask me for a path" — the same contract
        // `project-open` has always had. It used to reject with "open requires a
        // path", so the menu entry, its ellipsis and the welcome button were all
        // dead ends.
        //
        // A *typed* `open` keeps rejecting: the user is already at a command line
        // with the path in reach, and popping a native dialog out of a text command
        // would be both surprising and untestable (the headless control channel
        // drives this same path).
        if (source == ActionSource::Command) {
          return reject("open requires a path");
        }
        switch (context_.OpenNativeFilePicker()) {
          case ProjectOpenPickerResult::Launched:
          case ProjectOpenPickerResult::AlreadyOpen:
            return DispatchResult::Handled;
          case ProjectOpenPickerResult::Unavailable:
            // No native picker: seed the command palette with the verb so the user
            // only types the path, mirroring the project-open fallback.
            context_.OpenCommandPalette("open ");
            return DispatchResult::Handled;
        }
        return DispatchResult::Handled;
      }
      if (!context_.HasProjectRoot()) {
        return reject("No active project");
      }
      std::string error_message;
      if (!context_.OpenPath(request->path, &error_message)) {
        return reject(error_message);
      }
      return DispatchResult::Handled;
    }
    case ActionId::OpenSelectedTreeItem:
    case ActionId::OpenSelectedTreeItemInNewTab: {
      if (!context_.HasProjectRoot()) {
        return reject("No active project");
      }
      const std::filesystem::path path = context_.ResolveTreeActionPath(source);
      if (path.empty()) {
        return reject("No path selected");
      }
      if (id == ActionId::OpenSelectedTreeItemInNewTab) {
        if (!context_.OpenPathInNewTab(path)) {
          return reject("Failed to open file in a new tab: " + path.string());
        }
      } else {
        std::string error_message;
        if (!context_.OpenPath(path, &error_message)) {
          return reject(error_message);
        }
      }
      return DispatchResult::Handled;
    }
    case ActionId::Tab:
      if (!context_.HasProjectRoot()) {
        return reject("No active project");
      }
      {
        const TabPathsRequest request = BuildTabPathsRequest(args, context_.ProjectRoot());
        if (request.open_untitled) {
          context_.OpenUntitledTab();
          return DispatchResult::Handled;
        }

        for (const std::filesystem::path& path : request.paths) {
          if (!context_.OpenPathInNewTab(path)) {
            return reject("Failed to open file in a new tab: " + path.string());
          }
        }

        return DispatchResult::Handled;
      }
    case ActionId::TabSwitch: {
      if (!context_.HasProjectRoot()) {
        return reject("No active project");
      }
      std::string error_message;
      const TabSwitchRequest request = BuildTabSwitchRequest(args);
      const std::optional<std::size_t> tab_index =
          context_.FindTabIndexBySpecifier(request.specifier, &error_message);
      if (!tab_index.has_value()) {
        return reject(error_message.empty() ? "No matching tab" : error_message);
      }
      context_.ActivateTab(*tab_index);
      return DispatchResult::Handled;
    }
    case ActionId::TabMove:
      if (!context_.HasProjectRoot()) {
        return reject("No active project");
      }
      if (!context_.HasOpenTabs()) {
        return reject("No open tabs");
      }
      {
        const std::optional<TabMoveRequest> request = BuildTabMoveRequest(args);
        if (!request.has_value()) {
          return reject("tabmove requires a tab slot or relative offset");
        }
        const int current_slot = static_cast<int>(context_.ActiveTabIndex()) + 1;
        const int requested_slot = request->relative ? current_slot + request->slot : request->slot;
        const int clamped_slot =
            std::clamp(requested_slot, 1, static_cast<int>(context_.OpenTabCount()));
        context_.MoveActiveTabTo(static_cast<std::size_t>(clamped_slot - 1));
        return DispatchResult::Handled;
      }
    case ActionId::Reopen:
      if (!context_.HasProjectRoot()) {
        return reject("No active project");
      }
      context_.ReopenActiveTab();
      return DispatchResult::Handled;
    case ActionId::Save:
      if (!context_.HasProjectRoot()) {
        return reject("No active project");
      }
      if (context_.SaveTab(context_.ActiveTabIndex())) {
        if (source == ActionSource::Shortcut) {
          context_.ResetCaretBlink();
        }
      } else {
        return reject("Save failed");
      }
      return DispatchResult::Handled;
    case ActionId::SplitEditorRight:
    case ActionId::SplitEditorDown: {
      if (!context_.HasProjectRoot()) {
        return reject("No active project");
      }
      const EditorSplitOrientation orientation = id == ActionId::SplitEditorDown
                                                     ? EditorSplitOrientation::Horizontal
                                                     : EditorSplitOrientation::Vertical;
      // Tree context-menu split: open the right-clicked file into the new group.
      // (Editor tab/body menus dispatch with ActionSource::Menu and skip this.)
      if (source == ActionSource::ContextMenu) {
        const std::filesystem::path tree_path = context_.ResolveTreeActionPath(source);
        if (!tree_path.empty()) {
          std::string error_message;
          if (!context_.SplitEditorGroup(orientation, tree_path, &error_message)) {
            return reject(error_message);
          }
          return DispatchResult::Handled;
        }
      }
      const TabPathsRequest request = BuildTabPathsRequest(args, context_.ProjectRoot());

      if (request.open_untitled || request.paths.empty()) {
        std::string error_message;
        if (!context_.SplitEditorGroup(orientation, {}, &error_message)) {
          return reject(error_message);
        }
        return DispatchResult::Handled;
      }

      for (const std::filesystem::path& path : request.paths) {
        std::string error_message;
        if (!context_.SplitEditorGroup(orientation, path, &error_message)) {
          return reject(error_message);
        }
      }
      return DispatchResult::Handled;
    }
    case ActionId::FocusOtherGroup:
      context_.FocusOtherGroup();
      return DispatchResult::Handled;
    case ActionId::CloseGroup:
      context_.CloseEditorGroup();
      return DispatchResult::Handled;
    case ActionId::CloseActiveTab:
      if (context_.HasOpenTabs()) {
        context_.RequestCloseTab(context_.ActiveTabIndex());
      }
      return DispatchResult::Handled;
    case ActionId::CloseAllTabs:
      context_.CloseAllTabs();
      return DispatchResult::Handled;
    case ActionId::CloseOtherTabs: {
      std::vector<std::size_t> indices;
      if (context_.HasOpenTabs()) {
        indices.reserve(context_.OpenTabCount() - 1);
      }
      for (std::size_t i = 0; i < context_.OpenTabCount(); ++i) {
        if (i != context_.ActiveTabIndex()) {
          indices.push_back(i);
        }
      }
      context_.RequestCloseTabs(std::move(indices));
      return DispatchResult::Handled;
    }
    case ActionId::CloseTabsToRight: {
      std::vector<std::size_t> indices;
      for (std::size_t i = context_.ActiveTabIndex() + 1; i < context_.OpenTabCount(); ++i) {
        indices.push_back(i);
      }
      context_.RequestCloseTabs(std::move(indices));
      return DispatchResult::Handled;
    }
    case ActionId::CloseTabsToLeft: {
      std::vector<std::size_t> indices;
      indices.reserve(context_.ActiveTabIndex());
      for (std::size_t i = 0; i < context_.ActiveTabIndex(); ++i) {
        indices.push_back(i);
      }
      context_.RequestCloseTabs(std::move(indices));
      return DispatchResult::Handled;
    }
    case ActionId::CopyRelativePath:
    case ActionId::CopyAbsolutePath: {
      if (!context_.HasProjectRoot()) {
        return reject("No active project");
      }
      const std::filesystem::path path = context_.ActiveTabPath();
      if (path.empty()) {
        return reject("Active tab has no path");
      }

      std::string clipboard_text;
      if (id == ActionId::CopyRelativePath) {
        clipboard_text = RelativePathLabel(context_.ProjectRoot(), path);
        if (clipboard_text.empty() ||
            clipboard_text == path.lexically_normal().generic_string()) {
          return reject("Unable to resolve a relative path for the active tab");
        }
      } else {
        clipboard_text = path.lexically_normal().string();
      }

      context_.WriteClipboardText(clipboard_text);
      return DispatchResult::Handled;
    }
    case ActionId::RevealInFileTree: {
      if (!context_.HasProjectRoot()) {
        return reject("No active project");
      }
      // A context menu supplies its own row's path (the editor tab menu leaves it
      // empty and falls back to the active tab), so the same item can reveal a
      // search hit, a problem or a test from the sidebar list it was opened on.
      std::filesystem::path path = context_.RowContextMenuPath();
      if (path.empty()) {
        path = context_.ActiveTabPath();
      }
      if (path.empty()) {
        return reject("Nothing to reveal");
      }
      // Full VSCode-style reveal: open the sidebar on the Tree view (switching from
      // Search/Git if needed), then force-expand ancestors + select + scroll to the file.
      context_.ShowSidebarView(SidebarViewInfo{.id = "tree",
                                               .label = "Tree",
                                               .mode = SidebarMode::Tree},
                               context_.ProjectRoot(), {});
      if (!context_.RevealPathInTree(path)) {
        return reject("File is not in the project tree");
      }
      return DispatchResult::Handled;
    }
    case ActionId::MarkBranchFileReviewed:
      context_.MarkBranchFileReviewed();
      return DispatchResult::Handled;
    case ActionId::UnmarkBranchFileReviewed:
      context_.UnmarkBranchFileReviewed();
      return DispatchResult::Handled;
    case ActionId::MarkBranchHunkReviewed:
      context_.MarkBranchHunkReviewed();
      return DispatchResult::Handled;
    case ActionId::UnmarkBranchHunkReviewed:
      context_.UnmarkBranchHunkReviewed();
      return DispatchResult::Handled;
    case ActionId::ClearBranchReviewState:
      context_.ClearBranchReviewState();
      return DispatchResult::Handled;
    case ActionId::EditBranchReviewNote: {
      std::string note_text;
      if (!args.empty()) {
        note_text = args.front();
        for (std::size_t i = 1; i < args.size(); ++i) {
          note_text.push_back(' ');
          note_text += args[i];
        }
      }
      context_.EditBranchReviewNote(note_text);
      return DispatchResult::Handled;
    }
    default:
      return DispatchResult::Unhandled;
  }
}

}  // namespace microide::workspace
