#include "workspace/WorkspaceActionCoordinator.h"

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "workspace/WorkspaceActionRequests.h"
#include "workspace/WorkspaceCommandParsing.h"

namespace microide::workspace {

ActionCoordinator::DispatchResult ActionCoordinator::ExecuteSearch(
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
      if (!context_.HasProjectRoot()) {
        return reject("No active project");
      }
      context_.OpenTerminal(JoinCommandArguments(args, 0));
      return DispatchResult::Handled;
    case ActionId::TermClose:
      if (!context_.CloseActiveTerminal()) {
        return reject("No terminal to close");
      }
      return DispatchResult::Handled;
    case ActionId::TerminalFind:
      if (!context_.OpenTerminalFind(JoinCommandArguments(args, 0))) {
        return reject("No terminal to search");
      }
      return DispatchResult::Handled;
    case ActionId::Find:
      if (!context_.HasProjectRoot()) {
        return reject("No active project");
      }
      context_.ShowFileFinderWithQuery(JoinCommandArguments(args, 0));
      return DispatchResult::Handled;
    case ActionId::Files: {
      const FilesRequest request = BuildFilesRequest(args);
      if (!request.project_root.empty() && !context_.OpenProject(request.project_root, true, true)) {
        return reject("Failed to open project: " + request.project_root.string());
      }
      if (source == ActionSource::Shortcut && context_.OverlayVisible()) {
        context_.DismissOverlay();
        return DispatchResult::Handled;
      }
      if (source != ActionSource::Shortcut && !context_.HasProjectRoot()) {
        return reject("No active project");
      }
      context_.ShowFileFinder();
      return DispatchResult::Handled;
    }
    case ActionId::ProjectSearch:
      if (!context_.HasProjectRoot()) {
        return reject("No active project");
      }
      context_.ShowProjectSearchSidebar(JoinCommandArguments(args, 0));
      return DispatchResult::Handled;
    case ActionId::Search:
      if (!context_.HasProjectRoot()) {
        return reject("No active project");
      }
      if (context_.ActiveTabIsCompare() || context_.ActiveTabIsMerge()) {
        return reject("search is unavailable in compare and merge tabs");
      }
      context_.OpenBufferSearch(JoinCommandArguments(args, 0));
      return DispatchResult::Handled;
    case ActionId::ReplaceInBuffer:
      context_.OpenBufferReplace();
      return DispatchResult::Handled;
    case ActionId::Compare: {
      if (!context_.HasProjectRoot()) {
        return reject("No active project");
      }
      const CompareRequest request = BuildCompareRequest(args, context_.ProjectRoot());
      const std::filesystem::path path = context_.ResolveComparePath(request.path, source);

      if (path.empty()) {
        return reject("No file selected for compare");
      }
      std::error_code compare_exists_error;
      if (!std::filesystem::exists(path, compare_exists_error) || compare_exists_error) {
        return reject("Compare path does not exist: " + path.string());
      }

      context_.OpenComparePickerForPath(path, request.commit_spec);
      return DispatchResult::Handled;
    }
    case ActionId::CompareHead: {
      if (!context_.HasProjectRoot()) {
        return reject("No active project");
      }
      const std::filesystem::path path = context_.ResolveTreeActionPath(source);
      if (path.empty()) {
        return reject("No file selected for compare-head");
      }
      std::error_code head_exists_error;
      if (!std::filesystem::exists(path, head_exists_error) || head_exists_error) {
        return reject("Compare path does not exist: " + path.string());
      }
      context_.OpenHeadComparison(path);
      return DispatchResult::Handled;
    }
    case ActionId::CompareFiles: {
      // Works with no project open — arbitrary/outside-project paths allowed.
      const std::optional<CompareFilesRequest> request = BuildCompareFilesRequest(
          args, context_.HasProjectRoot() ? context_.ProjectRoot() : std::filesystem::path{});
      if (!request.has_value()) {
        return reject("compare-files requires exactly two paths: <left> <right>");
      }
      if (const std::string error =
              context_.CompareFiles(request->left_path, request->right_path);
          !error.empty()) {
        return reject(error);
      }
      return DispatchResult::Handled;
    }
    case ActionId::SelectForCompare: {
      if (const std::string error = context_.SelectForCompare(source); !error.empty()) {
        return reject(error);
      }
      return DispatchResult::Handled;
    }
    case ActionId::CompareWithSelected: {
      if (const std::string error = context_.CompareWithSelected(source); !error.empty()) {
        return reject(error);
      }
      return DispatchResult::Handled;
    }
    case ActionId::CompareWithClipboard: {
      if (const std::string error = context_.CompareWithClipboard(source); !error.empty()) {
        return reject(error);
      }
      return DispatchResult::Handled;
    }
    case ActionId::Merge: {
      if (!context_.HasProjectRoot()) {
        return reject("No active project");
      }
      const std::optional<MergeRequest> request = BuildMergeRequest(args, context_.ProjectRoot());
      if (!request.has_value()) {
        return reject("merge requires base, incoming, current, and optional output paths");
      }
      std::error_code merge_exists_error;
      if (!std::filesystem::exists(request->base_path, merge_exists_error) ||
          merge_exists_error ||
          !std::filesystem::exists(request->incoming_path, merge_exists_error) ||
          merge_exists_error ||
          !std::filesystem::exists(request->current_path, merge_exists_error) ||
          merge_exists_error) {
        return reject("merge requires existing base, incoming, and current files");
      }
      context_.OpenMergeEditor(request->base_path, request->incoming_path, request->current_path,
                               request->output_path);
      return DispatchResult::Handled;
    }
    case ActionId::ReviewConflicts: {
      if (!context_.HasProjectRoot()) {
        return reject("No active project");
      }
      const ReviewOpenOutcome outcome = context_.ReviewConflicts();
      if (!outcome.ok) {
        return reject(outcome.message);
      }
      return DispatchResult::Handled;
    }
    case ActionId::ReviewBranch: {
      if (!context_.HasProjectRoot()) {
        return reject("No active project");
      }
      const ReviewOpenOutcome outcome =
          context_.ReviewBranch(args.empty() ? std::string{} : args.front());
      if (!outcome.ok) {
        return reject(outcome.message);
      }
      return DispatchResult::Handled;
    }
    case ActionId::ReviewCommit: {
      if (!context_.HasProjectRoot()) {
        return reject("No active project");
      }
      const ReviewOpenOutcome outcome =
          context_.ReviewCommit(args.empty() ? std::string{} : args.front());
      if (!outcome.ok) {
        return reject(outcome.message);
      }
      return DispatchResult::Handled;
    }
    default:
      return DispatchResult::Unhandled;
  }
}

}  // namespace microide::workspace
