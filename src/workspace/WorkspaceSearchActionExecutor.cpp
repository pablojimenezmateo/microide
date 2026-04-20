#include "workspace/WorkspaceActionCoordinator.h"

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "workspace/WorkspaceActionRequests.h"
#include "workspace/WorkspaceCommandParsing.h"

namespace microide::workspace {

WorkspaceShell::ActionCoordinator::DispatchResult WorkspaceShell::ActionCoordinator::ExecuteSearch(
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
    case ActionId::Term:
      if (shell_.project_root_.empty()) {
        return reject("No active project");
      }
      shell_.OpenTerminal(JoinCommandArguments(args, 0));
      return DispatchResult::Handled;
    case ActionId::Find:
      if (shell_.project_root_.empty()) {
        return reject("No active project");
      }
      shell_.file_index_.Refresh();
      shell_.file_finder_.SetIndex(&shell_.file_index_);
      shell_.file_finder_.SetQuery(JoinCommandArguments(args, 0));
      shell_.ShowOverlay(OverlayMode::FileFinder);
      return DispatchResult::Handled;
    case ActionId::Files: {
      const FilesRequest request = BuildFilesRequest(args);
      if (!request.project_root.empty() &&
          !shell_.OpenProjectTab(request.project_root, true, true)) {
        return reject("Failed to open project: " + request.project_root.string());
      }
      if (source == ActionSource::Shortcut && shell_.overlay_state_.visible) {
        shell_.DismissOverlay();
        return DispatchResult::Handled;
      }
      if (source != ActionSource::Shortcut && shell_.project_root_.empty()) {
        return reject("No active project");
      }
      shell_.ShowOverlay(OverlayMode::FileFinder);
      shell_.file_index_.Refresh();
      shell_.file_finder_.SetIndex(&shell_.file_index_);
      shell_.file_finder_.SetQuery("");
      return DispatchResult::Handled;
    }
    case ActionId::ProjectSearch:
      if (shell_.project_root_.empty()) {
        return reject("No active project");
      }
      shell_.ShowSearchSidebar(JoinCommandArguments(args, 0), true);
      return DispatchResult::Handled;
    case ActionId::Search:
      if (shell_.project_root_.empty()) {
        return reject("No active project");
      }
      if (shell_.ActiveTabIsCompare() || shell_.ActiveTabIsMerge()) {
        return reject("search is unavailable in compare and merge tabs");
      }
      shell_.OpenBufferSearch();
      shell_.overlay_workflow_.buffer_search.query = JoinCommandArguments(args, 0);
      shell_.RefreshBufferSearch();
      return DispatchResult::Handled;
    case ActionId::ReplaceInBuffer:
      shell_.OpenBufferReplace();
      return DispatchResult::Handled;
    case ActionId::Compare: {
      if (shell_.project_root_.empty()) {
        return reject("No active project");
      }
      const CompareRequest request = BuildCompareRequest(args, shell_.project_root_);
      std::filesystem::path path = request.path;
      if (path.empty() && source == ActionSource::ContextMenu) {
        path = shell_.ResolveTreeActionPath(source);
      } else if (path.empty() && !shell_.text_viewport_.path().empty()) {
        path = shell_.text_viewport_.path().lexically_normal();
      } else if (path.empty() && shell_.sidebar_state_.visible &&
                 shell_.sidebar_state_.mode == SidebarMode::Tree) {
        const auto& entries = shell_.directory_tree_.entries();
        if (shell_.directory_tree_.selected_index() < entries.size() &&
            !entries[shell_.directory_tree_.selected_index()].is_directory) {
          path = entries[shell_.directory_tree_.selected_index()].path.lexically_normal();
        }
      }

      if (path.empty()) {
        return reject("No file selected for compare");
      }
      if (!std::filesystem::exists(path)) {
        return reject("Compare path does not exist: " + path.string());
      }

      shell_.OpenComparePickerForPath(path, request.commit_spec);
      return DispatchResult::Handled;
    }
    case ActionId::CompareHead: {
      if (shell_.project_root_.empty()) {
        return reject("No active project");
      }
      const std::filesystem::path path = shell_.ResolveTreeActionPath(source);
      if (path.empty()) {
        return reject("No file selected for compare-head");
      }
      if (!std::filesystem::exists(path)) {
        return reject("Compare path does not exist: " + path.string());
      }
      shell_.overlay_workflow_.compare_picker.path = path.lexically_normal();
      shell_.OpenComparison(project::GitCommitEntry{
          .hash = "HEAD",
          .short_hash = "HEAD",
          .subject = "HEAD",
      });
      return DispatchResult::Handled;
    }
    case ActionId::Merge: {
      if (shell_.project_root_.empty()) {
        return reject("No active project");
      }
      const std::optional<MergeRequest> request = BuildMergeRequest(args, shell_.project_root_);
      if (!request.has_value()) {
        return reject("merge requires base, incoming, current, and optional output paths");
      }
      if (!std::filesystem::exists(request->base_path) ||
          !std::filesystem::exists(request->incoming_path) ||
          !std::filesystem::exists(request->current_path)) {
        return reject("merge requires existing base, incoming, and current files");
      }
      shell_.OpenMergeEditor(request->base_path, request->incoming_path, request->current_path,
                             request->output_path);
      return DispatchResult::Handled;
    }
    default:
      return DispatchResult::Unhandled;
  }
}

}  // namespace microide::workspace
