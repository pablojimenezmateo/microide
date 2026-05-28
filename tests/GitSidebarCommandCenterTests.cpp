#include "TestSupport.h"

#include "workspace/GitSidebarCommandCenter.h"
#include "workspace/WorkspaceGitSidebarPresentation.h"

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
  Expect(view_model.commit_summary_line == "Commit ready  |  c commit",
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
  Expect(view_model.sections.size() >= 3 &&
             view_model.sections[2].empty_label == "No unstaged changes",
         "refreshing should not replace the unstaged empty-state label");
}

}  // namespace

void RegisterGitSidebarCommandCenterTests(std::vector<TestCase>& tests) {
  AddTest(tests, "GitSidebarCommandCenter/SectionClassification",
          TestGitSidebarSectionClassification);
  AddTest(tests, "GitSidebarCommandCenter/ViewModelGrouping", TestGitSidebarViewModelGrouping);
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
}

}  // namespace microide::tests
