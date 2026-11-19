#include "workspace/git/CommitPick.h"

#include <string>
#include <utility>

#include "util/StringUtil.h"
#include "workspace/WorkspaceUiText.h"

namespace microide::workspace {

std::vector<QuickPickItem> BuildCommitPickItems(std::span<const project::GitCommitEntry> commits) {
  std::vector<QuickPickItem> items;
  items.reserve(commits.size());
  for (const project::GitCommitEntry& commit : commits) {
    if (commit.hash.empty()) {
      continue;
    }
    QuickPickItem item;
    // The full hash: an abbreviation that is unique today need not stay so.
    item.value = "review-commit " + commit.hash;
    item.primary_label = commit.subject.empty() ? commit.short_hash : commit.subject;
    item.secondary_label = commit.short_hash;
    if (!commit.relative_date.empty()) {
      AppendHintSegment(item.secondary_label, commit.relative_date);
    }
    if (!commit.author.empty()) {
      AppendHintSegment(item.secondary_label, commit.author);
    }
    item.search_text = util::ToLowerAscii(item.primary_label + " " + item.secondary_label);
    items.push_back(std::move(item));
  }
  return items;
}

}  // namespace microide::workspace
