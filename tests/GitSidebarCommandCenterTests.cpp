#include "TestSupport.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "workspace/git/GitSidebarCommandCenter.h"
#include "workspace/git/WorkspaceGitSidebarPresentation.h"

namespace microide::tests {
namespace {

using microide::project::GitFileStatus;
using microide::workspace::BuildGitDiscardPreviewSummary;
using microide::workspace::BuildGitSidebarLineSpecs;
using microide::workspace::BuildGitSidebarViewModel;
using microide::workspace::ClassifyGitSidebarSection;
using microide::workspace::GitSidebarActionAvailabilityForEntry;
using microide::workspace::GitSidebarActionId;
using microide::workspace::GitSidebarDisabledActionMessage;
using microide::workspace::GitSidebarEntry;
using microide::workspace::GitSidebarLineKind;
using microide::workspace::GitSidebarState;
using microide::workspace::GitSidebarViewModel;
using microide::compare::BranchReviewStateService;

void TestGitSidebarSectionClassification() {
  Expect(ClassifyGitSidebarSection(true, false, GitFileStatus::Modified) ==
             GitSidebarEntry::Section::Conflicts,
         "conflicted entries should land in Conflicts");
  Expect(ClassifyGitSidebarSection(false, true, GitFileStatus::Modified) ==
             GitSidebarEntry::Section::Staged,
         "staged entries should land in Staged");
  Expect(ClassifyGitSidebarSection(false, false, GitFileStatus::Untracked) ==
             GitSidebarEntry::Section::Untracked,
         "untracked entries should land in Untracked");
  Expect(ClassifyGitSidebarSection(false, false, GitFileStatus::Modified) ==
             GitSidebarEntry::Section::Changed,
         "modified unstaged entries should land in Changed");
}

void TestGitSidebarViewModelGrouping() {
  GitSidebarState git_state;
  git_state.repo_available = true;
  git_state.branch_label = "feature/sidebar";
  git_state.upstream_label = "origin/feature/sidebar";
  git_state.ahead = 2;
  git_state.behind = 1;
  git_state.entries = {
      GitSidebarEntry{.section = GitSidebarEntry::Section::Conflicts,
                      .path = "src/conflict.cpp",
                      .relative_path = "src/conflict.cpp",
                      .status = GitFileStatus::Conflicted,
                      .conflicted = true,
                      .provider_id = {},
                      .provider_label = {},
                      .supports_stage = true,
                      .supports_discard = true},
      GitSidebarEntry{.section = GitSidebarEntry::Section::Staged,
                      .path = "src/staged.cpp",
                      .relative_path = "src/staged.cpp",
                      .status = GitFileStatus::Modified,
                      .staged = true,
                      .provider_id = {},
                      .provider_label = {},
                      .supports_stage = false,
                      .supports_discard = true},
      GitSidebarEntry{.section = GitSidebarEntry::Section::Changed,
                      .path = "src/changed.cpp",
                      .relative_path = "src/changed.cpp",
                      .status = GitFileStatus::Modified,
                      .provider_id = {},
                      .provider_label = {},
                      .supports_stage = true,
                      .supports_discard = true},
      GitSidebarEntry{.section = GitSidebarEntry::Section::Untracked,
                      .path = "notes.txt",
                      .relative_path = "notes.txt",
                      .status = GitFileStatus::Untracked,
                      .provider_id = {},
                      .provider_label = {},
                      .supports_stage = true,
                      .supports_discard = true},
      GitSidebarEntry{.section = GitSidebarEntry::Section::Outgoing,
                      .path = "src/outgoing.cpp",
                      .relative_path = "src/outgoing.cpp",
                      .status = GitFileStatus::Modified,
                      .provider_id = {},
                      .provider_label = {},
                      .supports_stage = false,
                      .supports_discard = false},
  };

  const BranchReviewStateService branch_review;
  const GitSidebarViewModel view_model =
      BuildGitSidebarViewModel(git_state, std::filesystem::path{"/tmp/project"}, branch_review);
  Expect(view_model.sections.size() == 5,
         "grouped git sidebar should expose all workflow sections");
  Expect(view_model.sections[0].header_label == "Conflicts (1)",
         "conflict section header should include count");
  Expect(view_model.sections[1].rows.size() == 1 && view_model.sections[1].rows[0].actions.unstage,
         "staged rows should expose unstage");
  Expect(view_model.sections[1].rows[0].primary_action_label == "diff review",
         "staged rows should advertise diff review as the default action");
  Expect(view_model.sections[2].header_label == "Unstaged (1)",
         "unstaged section header should use explicit unstaged wording");
  Expect(view_model.sections[3].rows[0].actions.stage &&
             view_model.sections[3].rows[0].actions.discard,
         "untracked rows should expose stage and discard");
  Expect(!view_model.sections[4].rows[0].actions.discard,
         "outgoing rows should not expose discard");
  Expect(view_model.workflow_summary_line.find("1 conflict") != std::string::npos &&
             view_model.workflow_summary_line.find("1 staged") != std::string::npos,
         "workflow summary should surface the active git work buckets");
  Expect(view_model.commit_summary_line.find("blocked") != std::string::npos,
         "conflicts should block commit readiness messaging");
  Expect(view_model.selection_summary_line.find("merge resolver") != std::string::npos,
         "selected conflict row should advertise merge as the primary action");
  Expect(view_model.selection_action_line.find("m merge") != std::string::npos,
         "selected conflict row should surface merge action hints");

  const auto lines = BuildGitSidebarLineSpecs(view_model);
  Expect(lines.size() == 14,
         "line specs should include section headers plus tree directory and file rows");
  Expect(lines[0].kind == GitSidebarLineKind::Header,
         "first grouped line should be a section header");
  Expect(lines[1].kind == GitSidebarLineKind::Directory && lines[1].label == "src",
         "grouped line specs should include a directory row before nested file entries");
  Expect(lines[2].kind == GitSidebarLineKind::Entry && lines[2].depth == 1,
         "nested file entries should be indented under their directory");
}

// TD-2026-08-06-159: the sidebar tree is grouped on '/'-separated generic text
// instead of `std::filesystem::path` algebra, and each row's key is computed once
// rather than derived from scratch on both sides of every sort comparison.
//
// The reference below is the ORIGINAL path-algebra formulation, spelled out here
// rather than called through, so this compares the new grouping against an
// independent implementation instead of against itself. It must stay written in
// terms of `parent_path()` / `lexically_normal()` / `generic_string()`.
namespace tree_reference {

std::vector<std::string> ParentPathSegments(const std::filesystem::path& relative_path) {
  std::vector<std::string> segments;
  const std::filesystem::path parent = relative_path.parent_path().lexically_normal();
  for (const auto& segment : parent) {
    const std::string label = segment.generic_string();
    if (label.empty() || label == "." || label == "/") {
      continue;
    }
    segments.push_back(label);
  }
  return segments;
}

std::string DirectoryNodeKey(int section, const std::filesystem::path& path) {
  return std::to_string(section) + "|" + path.lexically_normal().generic_string();
}

// (kind, label, tree_node_key, depth, entry_index) per emitted line, in order.
struct Line {
  int kind = 0;
  std::string label;
  std::string tree_node_key;
  int depth = 0;
  int entry_index = -1;
  bool operator==(const Line&) const = default;
};

struct Node {
  std::filesystem::path path;
  std::map<std::string, Node> directories;
  std::vector<const microide::workspace::GitSidebarRowViewModel*> files;
};

void Emit(const Node& node, int section, int depth, std::vector<Line>* out) {
  for (const auto& [label, child] : node.directories) {
    out->push_back(Line{.kind = static_cast<int>(microide::workspace::GitSidebarLineKind::Directory),
                        .label = label,
                        .tree_node_key = DirectoryNodeKey(section, child.path),
                        .depth = depth});
    Emit(child, section, depth + 1, out);
  }
  for (const auto* row : node.files) {
    std::string leaf = row->primary_label;
    if (leaf.empty()) {
      const std::filesystem::path normalized =
          std::filesystem::path(row->relative_path).lexically_normal();
      leaf = normalized.filename().string();
      if (leaf.empty()) {
        leaf = normalized.empty() ? "." : normalized.generic_string();
      }
    }
    out->push_back(Line{.kind = static_cast<int>(microide::workspace::GitSidebarLineKind::Entry),
                        .label = leaf,
                        .tree_node_key = {},
                        .depth = depth,
                        .entry_index = row->entry_index});
  }
}

std::vector<Line> Build(const microide::workspace::GitSidebarSectionViewModel& section) {
  std::vector<const microide::workspace::GitSidebarRowViewModel*> sorted;
  for (const auto& row : section.rows) sorted.push_back(&row);
  std::sort(sorted.begin(), sorted.end(),
            [](const microide::workspace::GitSidebarRowViewModel* lhs,
               const microide::workspace::GitSidebarRowViewModel* rhs) {
              const std::string l =
                  std::filesystem::path(lhs->relative_path).lexically_normal().generic_string();
              const std::string r =
                  std::filesystem::path(rhs->relative_path).lexically_normal().generic_string();
              if (l != r) return l < r;
              return lhs->entry_index < rhs->entry_index;
            });
  Node root;
  for (const auto* row : sorted) {
    Node* node = &root;
    int depth = 0;
    for (const std::string& segment :
         ParentPathSegments(std::filesystem::path(row->relative_path).lexically_normal())) {
      if (depth >= 64) break;
      Node& child = node->directories[segment];
      if (child.path.empty()) child.path = node->path / segment;
      node = &child;
      ++depth;
    }
    node->files.push_back(row);
  }
  std::vector<Line> out;
  Emit(root, static_cast<int>(section.section), 0, &out);
  return out;
}

}  // namespace tree_reference

void TestGitSidebarTreeGroupingMatchesPathAlgebra() {
  using microide::workspace::GitSidebarRowViewModel;
  using microide::workspace::GitSidebarSectionViewModel;

  // Awkward shapes on purpose: unnormalized dot components, a "//" run, a
  // parent-climb, an absolute path, a trailing separator, a bare filename, two
  // rows whose keys tie (so the entry_index tiebreak is exercised), and a path
  // past the 64-level depth cap.
  std::string deep;
  for (int i = 0; i < 80; ++i) deep += "d" + std::to_string(i) + "/";
  deep += "leaf.cpp";
  const std::vector<std::string> paths = {
      "src/z.cpp",   "src/./a.cpp",  "src//b.cpp", "src/sub/../c.cpp", "/abs/d.cpp",
      "bare.cpp",    "src/sub/e.cpp", "dir/",      "src/z.cpp",        deep,
      "../up.cpp",   "./dot.cpp",
  };

  GitSidebarSectionViewModel section;
  section.section = GitSidebarEntry::Section::Changed;
  section.header_label = "Changed";
  for (std::size_t i = 0; i < paths.size(); ++i) {
    GitSidebarRowViewModel row;
    row.entry_index = static_cast<int>(i);
    // `GitSidebarRowViewModel::relative_path` is normalized generic text by
    // contract (TD-2026-08-11-183): every ingress normalizes before the row is
    // built — MakeGitRepositoryPathIdentity for status entries,
    // CollectGitBranchOutgoingFiles' lexically_normal() for outgoing ones — and
    // the grouping keys are views into it rather than normalized copies. So
    // normalize here too, exactly where the shipping path does it, and let the
    // awkward spellings above go on exercising normalization *into* the grouping.
    row.relative_path = std::filesystem::path(paths[i]).lexically_normal().generic_string();
    // Left deliberately empty so FileLeafLabel's derive-from-path branch runs;
    // in the shipping path BuildGitSidebarViewModel always fills it.
    section.rows.push_back(row);
  }
  GitSidebarViewModel view_model;
  view_model.sections = {section};
  view_model.sections[0].show_header = false;

  const auto produced = BuildGitSidebarLineSpecs(view_model);
  const auto expected = tree_reference::Build(view_model.sections[0]);
  Expect(produced.size() == expected.size(),
         "the tree emits the same number of lines as the path-algebra reference (" +
             std::to_string(produced.size()) + " vs " + std::to_string(expected.size()) + ")");
  const std::size_t common = std::min(produced.size(), expected.size());
  for (std::size_t i = 0; i < common; ++i) {
    const tree_reference::Line actual{.kind = static_cast<int>(produced[i].kind),
                                      .label = produced[i].label,
                                      .tree_node_key = produced[i].tree_node_key,
                                      .depth = produced[i].depth,
                                      .entry_index = produced[i].entry_index};
    Expect(actual == expected[i],
           "line " + std::to_string(i) + " matches the reference (got label='" + actual.label +
               "' key='" + actual.tree_node_key + "' depth=" + std::to_string(actual.depth) +
               " entry=" + std::to_string(actual.entry_index) + "; want label='" +
               expected[i].label + "' key='" + expected[i].tree_node_key +
               "' depth=" + std::to_string(expected[i].depth) + " entry=" +
               std::to_string(expected[i].entry_index) + ")");
  }
  Expect(expected.size() > 20, "the fixture must actually produce a tree, not one flat row");
}

// TD-2026-07-17A-008: entry rows carry a render-ready display_label with the
// branch-review "[<marker>] " prefix precomputed, so the sidebar render TU no longer
// assembles it per paint. A marker-less row's display_label equals its leaf label.
void TestGitSidebarEntryDisplayLabelCarriesReviewMarker() {
  using microide::workspace::GitSidebarRowViewModel;
  using microide::workspace::GitSidebarSectionViewModel;

  GitSidebarViewModel view_model;
  GitSidebarSectionViewModel section;
  section.section = GitSidebarEntry::Section::Changed;
  section.header_label = "Changed (2)";
  GitSidebarRowViewModel marked;
  marked.entry_index = 0;
  marked.relative_path = "src/marked.cpp";
  marked.primary_label = "marked.cpp";
  marked.review_marker_label = "M";
  GitSidebarRowViewModel plain;
  plain.entry_index = 1;
  plain.relative_path = "src/plain.cpp";
  plain.primary_label = "plain.cpp";
  section.rows = {marked, plain};
  view_model.sections = {section};

  const auto lines = BuildGitSidebarLineSpecs(view_model);
  const microide::workspace::GitSidebarLineSpec* marked_line = nullptr;
  const microide::workspace::GitSidebarLineSpec* plain_line = nullptr;
  for (const auto& line : lines) {
    if (line.kind != GitSidebarLineKind::Entry) {
      continue;
    }
    if (line.entry_index == 0) {
      marked_line = &line;
    } else if (line.entry_index == 1) {
      plain_line = &line;
    }
  }
  Expect(marked_line != nullptr && plain_line != nullptr, "both entry rows should be built");
  Expect(marked_line->display_label == "[M] marked.cpp",
         "a review-marked row precomputes the bracketed marker prefix in display_label");
  Expect(marked_line->label == "marked.cpp",
         "the plain label stays the leaf name (used for navigation/selection)");
  Expect(plain_line->display_label == "plain.cpp",
         "a marker-less row's display_label equals its leaf label");
}

void TestConflictRowsDisableDirectStageUnstage() {
  GitSidebarEntry conflict_entry{
      .section = GitSidebarEntry::Section::Conflicts,
      .path = "src/conflict.cpp",
      .relative_path = "src/conflict.cpp",
      .status = GitFileStatus::Conflicted,
      .conflicted = true,
      .staged = true,
      .provider_id = {},
      .provider_label = {},
      .supports_stage = true,
      .supports_discard = true,
  };
  const auto availability =
      GitSidebarActionAvailabilityForEntry(conflict_entry, true, true);
  Expect(availability.default_view && availability.merge && availability.diff,
         "conflict rows should open review and merge workflows");
  Expect(!availability.stage && !availability.unstage,
         "unresolved conflict rows must not expose direct stage/unstage");
  Expect(availability.discard && availability.open_file,
         "conflict rows should still allow discard and open-file actions");
  Expect(GitSidebarDisabledActionMessage(GitSidebarActionId::Stage, conflict_entry, true, true) ==
             "Stage is unavailable for this row",
         "disabled stage should explain why");
  Expect(GitSidebarDisabledActionMessage(GitSidebarActionId::Unstage, conflict_entry, true, true) ==
             "Unstage is unavailable for this row",
         "disabled unstage should explain why");
}

void TestConflictDefaultViewRoutesToMerge() {
  GitSidebarEntry conflict_entry{
      .section = GitSidebarEntry::Section::Conflicts,
      .path = "src/conflict.cpp",
      .relative_path = "src/conflict.cpp",
      .conflicted = true,
      .provider_id = {},
      .provider_label = {},
  };
  const auto availability =
      GitSidebarActionAvailabilityForEntry(conflict_entry, true, true);
  Expect(availability.default_view && availability.merge,
         "primary conflict action should prefer the merge resolver");
}

void TestGitSidebarActionAvailabilityMessages() {
  GitSidebarEntry untracked_entry{
      .section = GitSidebarEntry::Section::Untracked,
      .path = "new.cpp",
      .relative_path = "new.cpp",
      .status = GitFileStatus::Untracked,
      .provider_id = {},
      .provider_label = {},
      .supports_stage = true,
      .supports_discard = true,
  };
  const auto availability =
      GitSidebarActionAvailabilityForEntry(untracked_entry, true, true);
  Expect(availability.stage && availability.discard && !availability.unstage,
         "untracked availability should allow stage and discard only");
  Expect(availability.open_file && !availability.diff,
         "untracked rows should open in the editor rather than a diff tab");
  Expect(GitSidebarDisabledActionMessage(GitSidebarActionId::Unstage, untracked_entry, true, true) ==
             "Unstage is unavailable for this row",
         "disabled unstage should explain why");

  GitSidebarEntry outgoing_entry{
      .section = GitSidebarEntry::Section::Outgoing,
      .path = "src/outgoing.cpp",
      .relative_path = "src/outgoing.cpp",
      .provider_id = {},
      .provider_label = {},
  };
  Expect(!GitSidebarActionAvailabilityForEntry(outgoing_entry, true, true).stage,
         "outgoing rows should not be stageable from the sidebar");
}

void TestGitDiscardPreviewSummaryMentionsUntrackedPolicy() {
  GitSidebarEntry entry{
      .section = GitSidebarEntry::Section::Untracked,
      .path = "scratch.txt",
      .relative_path = "scratch.txt",
      .provider_id = {},
      .provider_label = {},
  };
  const std::string summary = BuildGitDiscardPreviewSummary(entry, "fixture");
  Expect(summary.find("untracked") != std::string::npos,
         "discard preview should identify untracked removal");
  Expect(summary.find("trash") != std::string::npos,
         "discard preview should mention file-operation policy");
}

void TestCommitReadySummaryAppearsWithoutConflicts() {
  GitSidebarState git_state;
  git_state.repo_available = true;
  git_state.entries = {
      GitSidebarEntry{.section = GitSidebarEntry::Section::Staged,
                      .path = "src/staged.cpp",
                      .relative_path = "src/staged.cpp",
                      .status = GitFileStatus::Modified,
                      .staged = true,
                      .provider_id = {},
                      .provider_label = {},
                      .supports_stage = false,
                      .supports_discard = true},
      GitSidebarEntry{.section = GitSidebarEntry::Section::Changed,
                      .path = "src/changed.cpp",
                      .relative_path = "src/changed.cpp",
                      .status = GitFileStatus::Modified,
                      .provider_id = {},
                      .provider_label = {},
                      .supports_stage = true,
                      .supports_discard = true},
  };
  git_state.selected_index = 1;

  const BranchReviewStateService branch_review;
  const GitSidebarViewModel view_model =
      BuildGitSidebarViewModel(git_state, std::filesystem::path{"/tmp/project"}, branch_review);
  Expect(view_model.commit_summary_line == "Commit ready · c commit",
         "staged changes without conflicts should surface commit readiness");
  Expect(view_model.selection_action_line.find("s stage") != std::string::npos &&
             view_model.selection_action_line.find("c commit") != std::string::npos,
         "selected unstaged rows should expose both row actions and commit readiness");
}

void TestRefreshingDoesNotReplaceUnstagedEmptyLabel() {
  GitSidebarState git_state;
  git_state.repo_available = true;
  git_state.refreshing = true;
  git_state.snapshot_stale = true;

  const BranchReviewStateService branch_review;
  const GitSidebarViewModel view_model =
      BuildGitSidebarViewModel(git_state, std::filesystem::path{"/tmp/project"}, branch_review);
  Expect(view_model.stale_banner.empty(),
         "refreshing should move snapshot activity off the summary banner");
  Expect(!view_model.sections.empty() && !view_model.sections[0].show_header &&
             view_model.sections[0].empty_label == "No changes",
         "refreshing should not replace the clean-tree empty-state label");
}

// ---------------------------------------------------------------------------
// CachedGitSidebarPresentation memo: an in-depth suite whose central guarantee is
// that the memo can never return output that differs from a fresh
// BuildGitSidebarViewModel + BuildGitSidebarLines. Because the memo keys on a
// captured snapshot of the git-state / branch-review inputs, any input the key
// forgets would make `cached != uncached` after that input is mutated -- which the
// equivalence fuzz below turns into a hard failure. So a future field that starts
// feeding the view model but is not added to the key is caught here, not shipped.
// ---------------------------------------------------------------------------

using microide::compare::MakeBranchReviewTargetIdentity;
using microide::workspace::BuildGitSidebarLines;
using microide::workspace::CachedGitSidebarPresentation;
using microide::workspace::GitSidebarLine;
using microide::workspace::GitSidebarPresentation;
using microide::workspace::GitSidebarPresentationCacheHitsForTesting;
using microide::workspace::GitSidebarPresentationCacheMissesForTesting;
using microide::workspace::GitSidebarViewModel;
using microide::workspace::ResetGitSidebarPresentationCacheForTesting;

// Serialize every user-visible field of the presentation so a single string
// comparison catches a divergence in ANY field (a missed cache-key input surfaces
// as a stale digest).
std::string DigestPresentation(const GitSidebarViewModel& vm,
                               const std::vector<GitSidebarLine>& lines) {
  std::string d;
  const auto add = [&](std::string_view label, std::string_view value) {
    d += label;
    d += '=';
    d += value;
    d += '|';
  };
  const auto addb = [&](std::string_view label, bool value) {
    add(label, value ? "1" : "0");
  };
  for (const std::string& s : vm.summary_lines) add("summary", s);
  add("workflow", vm.workflow_summary_line);
  add("commit_summary", vm.commit_summary_line);
  add("selection_summary", vm.selection_summary_line);
  add("selection_action", vm.selection_action_line);
  add("stale", vm.stale_banner);
  add("error", vm.error_banner);
  addb("show_commit", vm.show_commit_button);
  addb("commit_ready", vm.commit_ready);
  add("commit_blocked", vm.commit_blocked_reason);
  addb("refreshing", vm.refreshing);
  for (const auto& section : vm.sections) {
    add("sect", std::to_string(static_cast<int>(section.section)));
    add("header", section.header_label);
    addb("show_header", section.show_header);
    add("empty", section.empty_label);
    for (const auto& row : section.rows) {
      add("row_idx", std::to_string(row.entry_index));
      add("row_path", row.relative_path);
      add("row_primary", row.primary_label);
      add("row_secondary", row.secondary_label);
      add("row_marker", row.review_marker_label);
      add("row_action", row.primary_action_label);
      add("row_status", std::to_string(static_cast<int>(row.status)));
      addb("row_stage_btn", row.show_stage_button);
      addb("row_discard_btn", row.show_discard_button);
    }
  }
  for (const GitSidebarLine& line : lines) {
    add("line_kind", std::to_string(static_cast<int>(line.kind)));
    add("line_sect", std::to_string(static_cast<int>(line.section)));
    add("line_label", line.label);
    add("line_display_label", line.display_label);
    add("line_key", line.tree_node_key);
    addb("line_expanded", line.expanded);
    add("line_depth", std::to_string(line.depth));
    add("line_entry", std::to_string(line.entry_index));
  }
  return d;
}

std::string DigestUncached(const GitSidebarState& git, const std::filesystem::path& root,
                           const BranchReviewStateService& branch_review) {
  const GitSidebarViewModel vm = BuildGitSidebarViewModel(git, root, branch_review);
  const std::vector<GitSidebarLine> lines =
      BuildGitSidebarLines(vm, &git.collapsed_directory_keys);
  return DigestPresentation(vm, lines);
}

std::string DigestCached(const GitSidebarState& git, const std::filesystem::path& root,
                         const BranchReviewStateService& branch_review) {
  const GitSidebarPresentation& p = CachedGitSidebarPresentation(git, root, branch_review);
  return DigestPresentation(p.view_model, p.lines);
}

GitSidebarState MakePopulatedGitState() {
  GitSidebarState git;
  git.repo_available = true;
  git.supports_mutations = true;
  git.base_ref = "origin/main";
  git.base_label = "vs origin/main";
  git.branch_label = "feature/x";
  git.snapshot_generation = 1;
  git.entries = {
      GitSidebarEntry{.section = GitSidebarEntry::Section::Conflicts,
                      .path = "src/c.cpp", .relative_path = "src/c.cpp",
                      .status = GitFileStatus::Conflicted, .conflicted = true},
      GitSidebarEntry{.section = GitSidebarEntry::Section::Staged,
                      .path = "src/s.cpp", .relative_path = "src/s.cpp",
                      .status = GitFileStatus::Modified, .staged = true},
      GitSidebarEntry{.section = GitSidebarEntry::Section::Changed,
                      .path = "dir/a/changed.cpp", .relative_path = "dir/a/changed.cpp",
                      .status = GitFileStatus::Modified},
      GitSidebarEntry{.section = GitSidebarEntry::Section::Untracked,
                      .path = "notes.txt", .relative_path = "notes.txt",
                      .status = GitFileStatus::Untracked},
      GitSidebarEntry{.section = GitSidebarEntry::Section::Outgoing,
                      .path = "src/out.cpp", .relative_path = "src/out.cpp",
                      .status = GitFileStatus::Modified},
  };
  return git;
}

// Deterministic LCG (Date/random are unavailable in this environment).
struct CacheRng {
  std::uint64_t state;
  std::uint32_t Next() {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<std::uint32_t>(state >> 33);
  }
  std::size_t Below(std::size_t n) { return n == 0 ? 0 : Next() % n; }
};

// CENTRAL GUARANTEE: across a long random walk of mutations to every input, the
// memoized presentation must always equal a fresh build. A key that forgets any
// field would return a stale digest here.
void TestGitSidebarCacheMatchesUncachedAcrossMutations() {
  const std::filesystem::path root = "/tmp/project";
  for (std::uint64_t seed : {1ULL, 7ULL, 42ULL, 1337ULL, 0xBEEFULL}) {
    ResetGitSidebarPresentationCacheForTesting();
    CacheRng rng{seed};
    GitSidebarState git = MakePopulatedGitState();
    BranchReviewStateService branch_review;
    const auto review_target = [&]() {
      return MakeBranchReviewTargetIdentity(root, git.base_ref, "HEAD", git.base_ref,
                                            git.snapshot_generation);
    };
    for (int step = 0; step < 300; ++step) {
      // Cached and uncached must agree on EVERY iteration.
      Expect(DigestCached(git, root, branch_review) == DigestUncached(git, root, branch_review),
             "cached presentation must equal a fresh build after every mutation");
      switch (rng.Below(16)) {
        case 0: git.refreshing = !git.refreshing; break;
        case 1: git.snapshot_stale = !git.snapshot_stale; break;
        case 2: git.repo_available = !git.repo_available; break;
        case 3: git.supports_mutations = !git.supports_mutations; break;
        case 4: git.commit_workflow.open = !git.commit_workflow.open; break;
        case 5: git.commit_workflow.operation_in_flight =
                    !git.commit_workflow.operation_in_flight; break;
        case 6: git.selected_index = rng.Below(git.entries.size() + 2); break;
        case 7: git.base_ref = rng.Below(2) ? "origin/main" : "origin/dev"; break;
        case 8: git.base_label = rng.Below(2) ? "vs origin/main" : "vs dev"; break;
        case 9: git.error = rng.Below(2) ? "" : "boom"; break;
        case 10: git.refresh_error = rng.Below(2) ? "" : "fetch failed"; break;
        case 11: {  // mutate entries + bump generation (mirrors a refresh apply)
          ++git.snapshot_generation;
          if (rng.Below(2) && !git.entries.empty()) {
            git.entries.pop_back();
          } else {
            git.entries.push_back(GitSidebarEntry{
                .section = GitSidebarEntry::Section::Changed,
                .path = "dir/b/f" + std::to_string(step) + ".cpp",
                .relative_path = "dir/b/f" + std::to_string(step) + ".cpp",
                .status = GitFileStatus::Modified});
          }
          break;
        }
        case 12: {  // collapse toggle
          const std::string key = "dir/a";
          if (git.collapsed_directory_keys.count(key)) {
            git.collapsed_directory_keys.erase(key);
          } else {
            git.collapsed_directory_keys.insert(key);
          }
          break;
        }
        case 13: {  // branch-review mutation affecting the Outgoing marker
          branch_review.MarkFileReviewed(review_target(), "src/out.cpp");
          break;
        }
        case 14: {  // branch-review clear
          branch_review.ClearTarget(review_target());
          break;
        }
        case 15:  // no-repo clear: entries emptied; size drop is the key signal
          git.entries.clear();
          break;
      }
    }
  }
}

// Mutating each fingerprint input from a fixed baseline forces a rebuild (miss);
// this pins that no relevant field is silently absent from the key.
void TestGitSidebarCacheInvalidatesOnEveryInput() {
  const std::filesystem::path root = "/tmp/project";
  BranchReviewStateService shared_review;
  const auto baseline = [] { return MakePopulatedGitState(); };
  const auto missed_on = [&](const std::function<void(GitSidebarState&)>& mutate,
                             std::string_view what) {
    ResetGitSidebarPresentationCacheForTesting();
    GitSidebarState git = baseline();
    (void)CachedGitSidebarPresentation(git, root, shared_review);  // prime (miss #1)
    const std::uint64_t misses_before = GitSidebarPresentationCacheMissesForTesting();
    mutate(git);
    (void)CachedGitSidebarPresentation(git, root, shared_review);
    Expect(GitSidebarPresentationCacheMissesForTesting() == misses_before + 1,
           std::string("cache must rebuild after mutating ").append(what).c_str());
  };
  missed_on([](GitSidebarState& g) { g.refreshing = !g.refreshing; }, "refreshing");
  missed_on([](GitSidebarState& g) { g.snapshot_stale = !g.snapshot_stale; }, "snapshot_stale");
  missed_on([](GitSidebarState& g) { g.repo_available = !g.repo_available; }, "repo_available");
  missed_on([](GitSidebarState& g) { g.supports_mutations = !g.supports_mutations; },
            "supports_mutations");
  missed_on([](GitSidebarState& g) { g.commit_workflow.open = !g.commit_workflow.open; },
            "commit_workflow.open");
  missed_on([](GitSidebarState& g) { g.commit_workflow.operation_in_flight = true; },
            "commit_workflow.operation_in_flight");
  missed_on([](GitSidebarState& g) { g.selected_index = 3; }, "selected_index");
  missed_on([](GitSidebarState& g) { g.base_ref = "origin/other"; }, "base_ref");
  missed_on([](GitSidebarState& g) { g.base_label = "other"; }, "base_label");
  missed_on([](GitSidebarState& g) { g.error = "err"; }, "error");
  missed_on([](GitSidebarState& g) { g.refresh_error = "err"; }, "refresh_error");
  missed_on([](GitSidebarState& g) { ++g.snapshot_generation; }, "snapshot_generation");
  missed_on([](GitSidebarState& g) { g.entries.pop_back(); }, "entries.size");
  missed_on([](GitSidebarState& g) { g.collapsed_directory_keys.insert("dir/a"); },
            "collapsed_directory_keys");
}

// Unchanged state hits; a branch-review revision bump (from a review mutation)
// misses even though no git-state field moved.
void TestGitSidebarCacheHitsAndBranchReviewRevision() {
  const std::filesystem::path root = "/tmp/project";
  ResetGitSidebarPresentationCacheForTesting();
  GitSidebarState git = MakePopulatedGitState();
  BranchReviewStateService branch_review;

  (void)CachedGitSidebarPresentation(git, root, branch_review);  // miss #1
  for (int i = 0; i < 5; ++i) {
    (void)CachedGitSidebarPresentation(git, root, branch_review);  // hits
  }
  Expect(GitSidebarPresentationCacheMissesForTesting() == 1, "unchanged state should miss once");
  Expect(GitSidebarPresentationCacheHitsForTesting() == 5, "repeat calls should all hit");

  const std::string before = DigestCached(git, root, branch_review);
  branch_review.MarkFileReviewed(
      MakeBranchReviewTargetIdentity(root, git.base_ref, "HEAD", git.base_ref,
                                     git.snapshot_generation),
      "src/out.cpp");
  Expect(GitSidebarPresentationCacheMissesForTesting() == 1,
         "the mutation itself must not touch the cache");
  const std::string after = DigestCached(git, root, branch_review);
  Expect(GitSidebarPresentationCacheMissesForTesting() == 2,
         "a branch-review revision bump must invalidate the git sidebar cache");
  Expect(before != after,
         "reviewing the Outgoing file must change the rendered presentation");
  Expect(DigestCached(git, root, branch_review) == DigestUncached(git, root, branch_review),
         "post-review cached output must still match a fresh build");
}

}  // namespace

void RegisterGitSidebarCommandCenterTests(std::vector<TestCase>& tests) {
  AddTest(tests, "GitSidebarCommandCenter/SectionClassification",
          TestGitSidebarSectionClassification);
  AddTest(tests, "GitSidebarCommandCenter/ViewModelGrouping", TestGitSidebarViewModelGrouping);
  AddTest(tests, "GitSidebarCommandCenter/TreeGroupingMatchesPathAlgebra",
          TestGitSidebarTreeGroupingMatchesPathAlgebra);
  AddTest(tests, "GitSidebarCommandCenter/EntryDisplayLabelCarriesReviewMarker",
          TestGitSidebarEntryDisplayLabelCarriesReviewMarker);
  AddTest(tests, "GitSidebarCommandCenter/ConflictRowsDisableStageUnstage",
          TestConflictRowsDisableDirectStageUnstage);
  AddTest(tests, "GitSidebarCommandCenter/ConflictDefaultViewRoutesToMerge",
          TestConflictDefaultViewRoutesToMerge);
  AddTest(tests, "GitSidebarCommandCenter/ActionAvailabilityMessages",
          TestGitSidebarActionAvailabilityMessages);
  AddTest(tests, "GitSidebarCommandCenter/DiscardPreviewSummary",
          TestGitDiscardPreviewSummaryMentionsUntrackedPolicy);
  AddTest(tests, "GitSidebarCommandCenter/CommitReadySummary",
          TestCommitReadySummaryAppearsWithoutConflicts);
  AddTest(tests, "GitSidebarCommandCenter/RefreshingKeepsUnstagedEmptyLabel",
          TestRefreshingDoesNotReplaceUnstagedEmptyLabel);
  AddTest(tests, "GitSidebarCommandCenter/CacheMatchesUncachedAcrossMutations",
          TestGitSidebarCacheMatchesUncachedAcrossMutations);
  AddTest(tests, "GitSidebarCommandCenter/CacheInvalidatesOnEveryInput",
          TestGitSidebarCacheInvalidatesOnEveryInput);
  AddTest(tests, "GitSidebarCommandCenter/CacheHitsAndBranchReviewRevision",
          TestGitSidebarCacheHitsAndBranchReviewRevision);
}

}  // namespace microide::tests
