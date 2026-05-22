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
                      .relative_path = "src/conflict.cpp",
                      .status = GitFileStatus::Conflicted,
                      .conflicted = true,
                      .supports_stage = true,
                      .supports_discard = true},
      GitSidebarEntry{.section = GitSidebarEntry::Section::Staged,
                      .relative_path = "src/staged.cpp",
                      .status = GitFileStatus::Modified,
                      .staged = true,
                      .supports_stage = false,
                      .supports_discard = true},
      GitSidebarEntry{.section = GitSidebarEntry::Section::Changed,
                      .relative_path = "src/changed.cpp",
                      .status = GitFileStatus::Modified,
                      .supports_stage = true,
                      .supports_discard = true},
      GitSidebarEntry{.section = GitSidebarEntry::Section::Untracked,
                      .relative_path = "notes.txt",
                      .status = GitFileStatus::Untracked,
                      .supports_stage = true,
                      .supports_discard = true},
      GitSidebarEntry{.section = GitSidebarEntry::Section::Outgoing,
                      .relative_path = "src/outgoing.cpp",
                      .status = GitFileStatus::Modified,
                      .supports_stage = false,
                      .supports_discard = false},
  };

  const GitSidebarViewModel view_model = BuildGitSidebarViewModel(git_state);
  Expect(view_model.sections.size() == 5,
         "grouped git sidebar should expose all workflow sections");
  Expect(view_model.sections[0].header_label == "Conflicts (1)",
         "conflict section header should include count");
  Expect(view_model.sections[1].rows.size() == 1 && view_model.sections[1].rows[0].actions.unstage,
         "staged rows should expose unstage");
  Expect(view_model.sections[3].rows[0].actions.stage &&
             view_model.sections[3].rows[0].actions.discard,
         "untracked rows should expose stage and discard");
  Expect(!view_model.sections[4].rows[0].actions.discard,
         "outgoing rows should not expose discard");

  const auto lines = BuildGitSidebarLineSpecs(view_model);
  Expect(lines.size() == 10,
         "line specs should include one header and one row per populated section");
  Expect(lines[0].kind == GitSidebarLineKind::Header,
         "first grouped line should be a section header");
}

void TestGitSidebarActionAvailabilityMessages() {
  GitSidebarEntry untracked_entry{
      .section = GitSidebarEntry::Section::Untracked,
      .status = GitFileStatus::Untracked,
      .supports_stage = true,
      .supports_discard = true,
  };
  const auto availability =
      GitSidebarActionAvailabilityForEntry(untracked_entry, true, true);
  Expect(availability.stage && availability.discard && !availability.unstage,
         "untracked availability should allow stage and discard only");
  Expect(GitSidebarDisabledActionMessage(GitSidebarActionId::Unstage, untracked_entry, true, true) ==
             "Unstage is unavailable for this row",
         "disabled unstage should explain why");

  GitSidebarEntry outgoing_entry{.section = GitSidebarEntry::Section::Outgoing};
  Expect(!GitSidebarActionAvailabilityForEntry(outgoing_entry, true, true).stage,
         "outgoing rows should not be stageable from the sidebar");
}

void TestGitDiscardPreviewSummaryMentionsUntrackedPolicy() {
  GitSidebarEntry entry{
      .section = GitSidebarEntry::Section::Untracked,
      .relative_path = "scratch.txt",
  };
  const std::string summary = BuildGitDiscardPreviewSummary(entry, "fixture");
  Expect(summary.find("untracked") != std::string::npos,
         "discard preview should identify untracked removal");
  Expect(summary.find("trash") != std::string::npos,
         "discard preview should mention file-operation policy");
}

}  // namespace

void RegisterGitSidebarCommandCenterTests(std::vector<TestCase>& tests) {
  AddTest(tests, "GitSidebarCommandCenter/SectionClassification",
          TestGitSidebarSectionClassification);
  AddTest(tests, "GitSidebarCommandCenter/ViewModelGrouping", TestGitSidebarViewModelGrouping);
  AddTest(tests, "GitSidebarCommandCenter/ActionAvailabilityMessages",
          TestGitSidebarActionAvailabilityMessages);
  AddTest(tests, "GitSidebarCommandCenter/DiscardPreviewSummary",
          TestGitDiscardPreviewSummaryMentionsUntrackedPolicy);
}

}  // namespace microide::tests
