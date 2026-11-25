#include "workspace/git/WorktreePick.h"

#include <filesystem>
#include <string>
#include <utility>

#include "util/StringUtil.h"
#include "workspace/WorkspaceCommandParsing.h"
#include "workspace/WorkspaceUiText.h"

namespace microide::workspace {

std::vector<QuickPickItem> BuildWorktreePickItems(std::span<const project::GitWorktree> worktrees,
                                                  std::string_view current_root,
                                                  std::string_view open_command,
                                                  std::string_view target_prefix) {
  std::filesystem::path current = std::filesystem::path(current_root).lexically_normal();
  if (!current.has_filename()) {
    current = current.parent_path();
  }
  std::vector<QuickPickItem> items;
  items.reserve(worktrees.size());
  for (const project::GitWorktree& worktree : worktrees) {
    if (worktree.bare || worktree.prunable || worktree.path.empty()) {
      continue;
    }
    const std::filesystem::path path = std::filesystem::path(worktree.path).lexically_normal();
    QuickPickItem item;
    item.value = std::string(open_command) + " " + QuoteCommandArg(std::string(target_prefix) + path.string());
    item.primary_label = path.filename().string();
    if (item.primary_label.empty()) {
      item.primary_label = path.string();
    }
    item.secondary_label =
        !worktree.branch.empty() ? worktree.branch : "detached " + worktree.head.substr(0, 7);
    if (worktree.state.has_value()) {
      if (worktree.state->changed > 0) {
        AppendHintSegment(item.secondary_label, std::to_string(worktree.state->changed) + " changed");
      }
      if (worktree.state->ahead > 0) {
        AppendHintSegment(item.secondary_label, std::to_string(worktree.state->ahead) + "\xe2\x86\x91");
      }
      if (worktree.state->behind > 0) {
        AppendHintSegment(item.secondary_label, std::to_string(worktree.state->behind) + "\xe2\x86\x93");
      }
    }
    AppendHintSegment(item.secondary_label, path.parent_path().string());
    if (worktree.locked) {
      AppendHintSegment(item.secondary_label, "locked");
    }
    if (path == current) {
      AppendHintSegment(item.secondary_label, "this project");
    }
    item.search_text = util::ToLowerAscii(item.primary_label + " " + item.secondary_label);
    items.push_back(std::move(item));
  }
  return items;
}

}
