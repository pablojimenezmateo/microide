#include "TestSupport.h"

#include "editor/TextBuffer.h"
#include "project/GitCompareService.h"
#include "workspace/WorkspaceShellTestAccess.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include "WorkspaceShellEventHelpers.h"

namespace microide::tests {
namespace {

using microide::workspace::TabEntry;
using microide::workspace::WorkspaceShell;
using WorkspaceShellTestAccess = microide::workspace::WorkspaceShell::TestAccess;

std::optional<microide::editor::EditorBlameOverlay> WaitForActiveEditorBlameOverlay(
    WorkspaceShell& shell,
    std::size_t minimum_line_count = 1) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    const auto overlay = WorkspaceShellTestAccess::ActiveEditorBlameOverlay(shell);
    if (overlay.has_value() && overlay->lines.size() >= minimum_line_count) {
      return overlay;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return WorkspaceShellTestAccess::ActiveEditorBlameOverlay(shell);
}

bool WaitForGitSidebarEntryCount(WorkspaceShell& shell, std::size_t expected_count) {
  return WaitUntil(
      [&shell, expected_count]() {
        return WorkspaceShellTestAccess::GitSidebarEntries(shell).size() == expected_count &&
               !WorkspaceShellTestAccess::GitSidebarRefreshing(shell);
      },
      std::chrono::seconds(2), std::chrono::milliseconds(10),
      [&shell]() { WorkspaceShellTestAccess::ConsumeGitSidebarRefresh(shell); });
}

// The outgoing-base ref picker now runs its branch/commit git queries on the
// background executor; drive the mailbox drain until it leaves the loading state.
bool SettleComparePicker(WorkspaceShell& shell) {
  return WaitUntil(
      [&shell]() { return !WorkspaceShellTestAccess::ComparePickerLoading(shell); },
      std::chrono::seconds(2), std::chrono::milliseconds(5),
      [&shell]() { WorkspaceShellTestAccess::ConsumeGitSidebarRefresh(shell); });
}

void TestWorkspaceShellGitSidebarRefreshPreservesActiveEditorBlameCache() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "int alpha() {\n  return 1;\n}\nint beta() {\n  return 2;\n}\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add editor blame fixture", "editor blame fixture");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(2, 0);

  const auto loaded_overlay = WaitForActiveEditorBlameOverlay(shell, 3);
  Expect(loaded_overlay.has_value() && loaded_overlay->lines.size() == 3,
         "refresh-preservation fixture should start with loaded blame lines");

  WorkspaceShellTestAccess::RefreshGitSidebar(shell);

  const auto refreshed_overlay = WorkspaceShellTestAccess::ActiveEditorBlameOverlay(shell);
  Expect(refreshed_overlay.has_value() && refreshed_overlay->lines.size() == 3,
         "refreshing the git sidebar should not flush an unrelated active editor blame cache");
  Expect(refreshed_overlay->lines[1].author == "Microide Tests",
         "refreshing the git sidebar should preserve blame metadata for the active editor");
}

void TestWorkspaceShellGitSidebarEntryRightClickOpensContextMenu() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path alpha = root / "src" / "alpha.cpp";
  const std::filesystem::path beta = root / "src" / "beta.cpp";
  WriteFile(alpha, "int alpha() {\n  return 1;\n}\n");
  WriteFile(beta, "int beta() {\n  return 2;\n}\n");

  InitializeGitRepo(root);
  CommitAll(root, "context menu fixture", "context menu fixture");
  WriteFile(alpha, "int alpha() {\n  return 10;\n}\n");
  WriteFile(beta, "int beta() {\n  return 20;\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::ShowGitSidebar(shell);
  Expect(WaitForGitSidebarEntryCount(shell, 2),
         "context menu fixture should expose two changed entries");

  const SDL_FRect row_rect = WorkspaceShellTestAccess::GitSidebarEntryRowRect(shell, 1);
  Expect(SendMouseDown(shell, row_rect.x + row_rect.w - 12.0f, row_rect.y + row_rect.h * 0.5f,
                       SDL_BUTTON_RIGHT),
         "right-clicking a git sidebar entry should be handled");

  Expect(WorkspaceShellTestAccess::GitEntryContextMenuOpen(shell),
         "right-clicking a git sidebar entry should open the entry context menu");
  Expect(WorkspaceShellTestAccess::GitSidebarSelectedIndex(shell) == 1,
         "right-clicking should select the clicked entry");
  Expect(WorkspaceShellTestAccess::OpenTabs(shell).empty(),
         "right-clicking should not open a compare/editor tab on its own");

  const auto labels = WorkspaceShellTestAccess::GitEntryContextMenuLabels(shell);
  const std::vector<std::string> expected = {"Open Changes", "Stage", "Discard…",
                                             "Copy Relative Path", "Copy Absolute Path"};
  Expect(labels == expected,
         "the git entry context menu should expose the expected action labels");
}

void TestWorkspaceShellGitSidebarLeftClickDoesNotFireActions() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "left_click.cpp";
  WriteFile(source, "int value() {\n  return 1;\n}\n");

  InitializeGitRepo(root);
  CommitAll(root, "left-click fixture", "left-click fixture");
  WriteFile(source, "int value() {\n  return 2;\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::ShowGitSidebar(shell);
  Expect(WaitForGitSidebarEntryCount(shell, 1),
         "left-click fixture should expose one changed entry");

  const SDL_FRect row_rect = WorkspaceShellTestAccess::GitSidebarEntryRowRect(shell, 0);
  // Click on the far right of the row, where the inline Stage/Discard buttons
  // used to live: this must select + open the diff, never stage the entry.
  Expect(SendMouseDown(shell, row_rect.x + row_rect.w - 8.0f, row_rect.y + row_rect.h * 0.5f,
                       SDL_BUTTON_LEFT),
         "left-clicking a git sidebar entry should be handled");

  const auto& entries = WorkspaceShellTestAccess::GitSidebarEntries(shell);
  Expect(entries.size() == 1 &&
             entries[0].section == WorkspaceShell::GitSidebarEntry::Section::Changed &&
             !entries[0].staged,
         "left-clicking a git sidebar entry must not stage it");
  Expect(WorkspaceShellTestAccess::ActiveCompare(shell).path == entries[0].path.lexically_normal(),
         "left-clicking a git sidebar entry should open its comparison");
  Expect(!WorkspaceShellTestAccess::GitEntryContextMenuOpen(shell),
         "left-clicking a git sidebar entry should not open the context menu");
}

void TestWorkspaceShellGitSidebarContextMenuActionsActOnSelectedEntry() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "menu_action.cpp";
  WriteFile(source, "int value() {\n  return 1;\n}\n");

  InitializeGitRepo(root);
  CommitAll(root, "menu action fixture", "menu action fixture");
  WriteFile(source, "int value() {\n  return 2;\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::ShowGitSidebar(shell);
  Expect(WaitForGitSidebarEntryCount(shell, 1),
         "menu action fixture should expose one changed entry");
  WorkspaceShellTestAccess::RevealGitSidebarEntry(shell, 0);

  // Unstaged entry: the toggle item stages it and reports "Stage".
  Expect(WorkspaceShellTestAccess::GitEntryContextMenuLabels(shell)[1] == "Stage",
         "an unstaged entry's toggle item should read Stage");
  Expect(WorkspaceShellTestAccess::IsActionEnabled(shell, WorkspaceShell::ActionId::GitDiscardEntry),
         "Discard should be enabled for a working-tree entry");
  Expect(WorkspaceShellTestAccess::ExecuteContextMenuAction(
             shell, WorkspaceShell::ActionId::GitStageToggleEntry),
         "the stage toggle context-menu action should be handled");
  Expect(WaitForGitSidebarEntryCount(shell, 1),
         "git sidebar should settle after the stage refresh");
  const auto& staged_entries = WorkspaceShellTestAccess::GitSidebarEntries(shell);
  Expect(std::any_of(staged_entries.begin(), staged_entries.end(),
                     [](const WorkspaceShell::GitSidebarEntry& entry) {
                       return entry.section == WorkspaceShell::GitSidebarEntry::Section::Staged;
                     }),
         "the stage toggle action should move the entry into the staged section");

  // Now staged: the toggle item flips to "Unstage".
  WorkspaceShellTestAccess::RevealGitSidebarEntry(shell, 0);
  Expect(WorkspaceShellTestAccess::GitEntryContextMenuLabels(shell)[1] == "Unstage",
         "a staged entry's toggle item should read Unstage");

  // Discard opens the confirmation prompt rather than mutating immediately.
  Expect(WorkspaceShellTestAccess::ExecuteContextMenuAction(
             shell, WorkspaceShell::ActionId::GitDiscardEntry),
         "the discard context-menu action should be handled");
  Expect(WorkspaceShellTestAccess::PromptSurfaceVisible(shell),
         "the discard context-menu action should open a confirmation prompt");
}

void TestWorkspaceShellOpeningGitSidebarEntryAlsoInvalidatesSidebarSelection() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path alpha = root / "src" / "alpha.cpp";
  const std::filesystem::path beta = root / "src" / "beta.cpp";
  WriteFile(alpha, "int alpha() {\n  return 1;\n}\n");
  WriteFile(beta, "int beta() {\n  return 2;\n}\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add git sidebar selection fixture", "git sidebar selection fixture");
  WriteFile(alpha, "int alpha() {\n  return 10;\n}\n");
  WriteFile(beta, "int beta() {\n  return 20;\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::ShowGitSidebar(shell);

  const auto& entries = WorkspaceShellTestAccess::GitSidebarEntries(shell);
  Expect(WaitForGitSidebarEntryCount(shell, 2),
         "git sidebar selection fixture should expose two modified entries");

  const SDL_FRect row_rect = WorkspaceShellTestAccess::GitSidebarEntryRowRect(shell, 1);
  Expect(row_rect.w > 0.0f && row_rect.h > 0.0f,
         "git sidebar selection fixture should expose a clickable row rect");

  SDL_Event event{};
  event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
  event.button.button = SDL_BUTTON_LEFT;
  event.button.x = row_rect.x + 12.0f;
  event.button.y = row_rect.y + row_rect.h * 0.5f;
  const auto result = shell.HandleEvent(event);
  const auto layout = WorkspaceShellTestAccess::CurrentLayout(shell);

  Expect(result.handled, "clicking a git sidebar entry should be handled");
  Expect(!result.redraw.full && !result.redraw.rects.empty(),
         "opening a git sidebar entry should stay on a partial redraw path");
  Expect(AnyRectIntersects(result.redraw.rects, layout.sidebar),
         "opening a git sidebar entry should also invalidate the sidebar selection state");
  Expect(WorkspaceShellTestAccess::ActiveCompare(shell).path == entries[1].path.lexically_normal(),
         "clicking a git sidebar entry should open the selected comparison target");
}

void TestWorkspaceShellGitSidebarUntrackedEntryOpensEditor() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path untracked_file = root / "new_file.cpp";

  WriteFile(root / "README.md", "fixture\n");
  InitializeGitRepo(root);
  CommitAll(root, "base commit", "git untracked open fixture");
  WriteFile(untracked_file, "int main() { return 0; }\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::ShowGitSidebar(shell);
  Expect(WaitForGitSidebarEntryCount(shell, 1),
         "git sidebar should list the untracked file");

  const auto& entries = WorkspaceShellTestAccess::GitSidebarEntries(shell);
  const auto untracked_index = static_cast<std::size_t>(
      std::distance(entries.begin(),
                    std::find_if(entries.begin(), entries.end(),
                                 [](const WorkspaceShell::GitSidebarEntry& entry) {
                                   return entry.section ==
                                          WorkspaceShell::GitSidebarEntry::Section::Untracked;
                                 })));
  Expect(untracked_index < entries.size(),
         "fixture should expose an untracked git sidebar entry");

  const SDL_FRect row_rect =
      WorkspaceShellTestAccess::GitSidebarEntryRowRect(shell, untracked_index);

  SDL_Event event{};
  event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
  event.button.button = SDL_BUTTON_LEFT;
  event.button.x = row_rect.x + 12.0f;
  event.button.y = row_rect.y + row_rect.h * 0.5f;
  const auto result = shell.HandleEvent(event);

  Expect(result.handled, "clicking an untracked git sidebar entry should be handled");
  const auto& tabs = WorkspaceShellTestAccess::OpenTabs(shell);
  Expect(!tabs.empty(), "clicking an untracked entry should open a tab");
  Expect(tabs.back().path == entries[untracked_index].path.lexically_normal(),
         "clicking an untracked entry should open the file in an editor tab");
  Expect(tabs.back().kind == TabEntry::Kind::Editor,
         "untracked entries should open as editor tabs, not compare tabs");
}

void TestWorkspaceShellGitOutgoingBaseChoiceRefreshesOutgoingEntries() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path alpha = root / "src" / "alpha.cpp";
  const std::filesystem::path beta = root / "src" / "beta.cpp";
  WriteFile(alpha, "int alpha() {\n  return 1;\n}\n");
  WriteFile(beta, "int beta() {\n  return 2;\n}\n");

  InitializeGitRepo(root);
  CommitAll(root, "base fixture", "base fixture");
  RequireGitCommandSuccess(root, {"checkout", "-b", "feature/outgoing-base"},
                           "git checkout feature branch");

  WriteFile(alpha, "int alpha() {\n  return 10;\n}\n");
  CommitAll(root, "feature alpha", "feature alpha");
  WriteFile(beta, "int beta() {\n  return 20;\n}\n");
  CommitAll(root, "feature beta", "feature beta");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::ShowGitSidebar(shell);
  Expect(WaitForGitSidebarEntryCount(shell, 2),
         "git outgoing-base fixture should expose two initial outgoing entries");

  auto count_outgoing = [&]() {
    return static_cast<int>(std::count_if(
        WorkspaceShellTestAccess::GitSidebarEntries(shell).begin(),
        WorkspaceShellTestAccess::GitSidebarEntries(shell).end(),
        [](const WorkspaceShell::GitSidebarEntry& entry) {
          return entry.section == WorkspaceShell::GitSidebarEntry::Section::Outgoing;
        }));
  };

  auto& state = WorkspaceShellTestAccess::CurrentProjectState(shell);
  Expect(state.sidebar.git.base_ref == "refs/heads/main",
         "auto outgoing base should resolve the full base ref (TD-2026-07-17A-025)");
  Expect(count_outgoing() == 2,
         "auto outgoing base should include both feature-branch commits");

  state.sidebar.git.outgoing_base_choice = microide::workspace::OutgoingBaseChoice{
      .kind = microide::workspace::OutgoingBaseChoice::Kind::PreviousCommit,
      .custom_ref = {},
  };
  WorkspaceShellTestAccess::RefreshGitSidebar(shell);
  Expect(WaitForGitSidebarEntryCount(shell, 1),
         "previous-commit outgoing base should settle to one outgoing entry");
  Expect(state.sidebar.git.base_ref == "HEAD~1",
         "previous-commit outgoing base should use HEAD~1");
  Expect(count_outgoing() == 1,
         "previous-commit outgoing base should only include the newest commit delta");
  Expect(std::any_of(WorkspaceShellTestAccess::GitSidebarEntries(shell).begin(),
                     WorkspaceShellTestAccess::GitSidebarEntries(shell).end(),
                     [&](const WorkspaceShell::GitSidebarEntry& entry) {
                       return entry.section == WorkspaceShell::GitSidebarEntry::Section::Outgoing &&
                              entry.relative_path == std::filesystem::path("src/beta.cpp");
                     }),
         "previous-commit outgoing base should preserve the latest outgoing file");

  state.sidebar.git.outgoing_base_choice = microide::workspace::OutgoingBaseChoice{
      .kind = microide::workspace::OutgoingBaseChoice::Kind::SpecificRef,
      .custom_ref = "HEAD~2",
  };
  WorkspaceShellTestAccess::RefreshGitSidebar(shell);
  Expect(WaitForGitSidebarEntryCount(shell, 2),
         "specific-ref outgoing base should settle back to two outgoing entries");
  Expect(state.sidebar.git.base_ref == "HEAD~2",
         "specific-ref outgoing base should preserve the exact ref string");
  Expect(count_outgoing() == 2,
         "specific-ref outgoing base should pass the custom ref through unchanged");
}

void TestWorkspaceShellGitOutgoingBaseButtonOpensMenuAndPrompt() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  WriteFile(root / "README.md", "hello\n");

  InitializeGitRepo(root);
  CommitAll(root, "base fixture", "base fixture");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::ShowGitSidebar(shell);
  Expect(WaitForGitSidebarEntryCount(shell, 0),
         "git outgoing-base menu fixture should settle the initial sidebar refresh");
  WorkspaceShellTestAccess::CurrentProjectState(shell).sidebar.git.outgoing_base_choice =
      microide::workspace::OutgoingBaseChoice{
          .kind = microide::workspace::OutgoingBaseChoice::Kind::SpecificRef,
          .custom_ref = "origin/release/2026-04",
      };

  const auto button_rect = WorkspaceShellTestAccess::GitSidebarOutgoingBaseButtonRect(shell);
  Expect(button_rect.has_value(),
         "git outgoing base fixture should expose the outgoing header button");

  const float click_x = button_rect->x + button_rect->w * 0.5f;
  const float click_y = button_rect->y + button_rect->h * 0.5f;
  Expect(SendMouseDown(shell, click_x, click_y, SDL_BUTTON_LEFT),
         "clicking the outgoing base button should be handled");

  const auto labels =
      WorkspaceShellTestAccess::VisiblePopupMenuLabels(shell, WorkspaceShell::MenuId::GitOutgoingBase);
  Expect(labels.size() == 3 && labels[0] == "Auto (base branch)" &&
             labels[1] == "Previous commit (HEAD~1)" && labels[2] == "Branch or commit\xE2\x80\xA6",
         "outgoing base menu should expose the three base-selection choices");

  const auto pick_ref_rect = WorkspaceShellTestAccess::PopupMenuItemRect(
      shell, WorkspaceShell::MenuId::GitOutgoingBase, "Branch or commit\xE2\x80\xA6");
  Expect(pick_ref_rect.has_value(),
         "outgoing base menu should expose the branch/commit picker entry");
  Expect(SendMouseDown(shell, pick_ref_rect->x + pick_ref_rect->w * 0.5f,
                       pick_ref_rect->y + pick_ref_rect->h * 0.5f, SDL_BUTTON_LEFT),
         "clicking the branch/commit outgoing base entry should be handled");

  Expect(WorkspaceShellTestAccess::OverlayVisible(shell) &&
             WorkspaceShellTestAccess::ActiveOverlayMode(shell) ==
                 WorkspaceShell::OverlayMode::CommitPicker,
         "branch/commit outgoing base entry should open the commit picker overlay");
  const auto& picker =
      WorkspaceShellTestAccess::CurrentProjectState(shell).overlay.workflow.compare_picker;
  Expect(picker.purpose == microide::workspace::ComparePickerPurpose::OutgoingBaseRef,
         "outgoing base picker should run in the outgoing-base purpose");
  // The branch/commit list is populated asynchronously; wait for the query to land.
  Expect(SettleComparePicker(shell), "the async outgoing-base query should settle");
  Expect(!picker.items.empty(),
         "outgoing base picker should list at least the current branch / recent commit");
}

void TestWorkspaceShellGitSidebarKeepsOutgoingRowsWhenFilesAlsoHaveWorkingTreeChanges() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path alpha = root / "src" / "alpha.cpp";
  const std::filesystem::path beta = root / "src" / "beta.cpp";
  WriteFile(alpha, "int alpha() {\n  return 1;\n}\n");
  WriteFile(beta, "int beta() {\n  return 2;\n}\n");

  InitializeGitRepo(root);
  CommitAll(root, "base fixture", "base fixture");
  RequireGitCommandSuccess(root, {"checkout", "-b", "feature/outgoing-visible"},
                           "git checkout feature branch");

  WriteFile(alpha, "int alpha() {\n  return 10;\n}\n");
  CommitAll(root, "feature alpha", "feature alpha");
  WriteFile(beta, "int beta() {\n  return 20;\n}\n");
  CommitAll(root, "feature beta", "feature beta");
  WriteFile(alpha, "int alpha() {\n  return 11;\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::ShowGitSidebar(shell);
  Expect(WaitForGitSidebarEntryCount(shell, 3),
         "outgoing-visible fixture should expose the changed file plus both outgoing rows");

  auto count_outgoing = [&]() {
    return static_cast<int>(std::count_if(
        WorkspaceShellTestAccess::GitSidebarEntries(shell).begin(),
        WorkspaceShellTestAccess::GitSidebarEntries(shell).end(),
        [](const WorkspaceShell::GitSidebarEntry& entry) {
          return entry.section == WorkspaceShell::GitSidebarEntry::Section::Outgoing;
        }));
  };
  auto count_changed = [&]() {
    return static_cast<int>(std::count_if(
        WorkspaceShellTestAccess::GitSidebarEntries(shell).begin(),
        WorkspaceShellTestAccess::GitSidebarEntries(shell).end(),
        [](const WorkspaceShell::GitSidebarEntry& entry) {
          return entry.section == WorkspaceShell::GitSidebarEntry::Section::Changed;
        }));
  };

  Expect(count_changed() >= 1,
         "fixture should still show working-tree changed entries");
  Expect(count_outgoing() == 2,
         "outgoing rows should remain visible even when one path also has working-tree changes");

  WorkspaceShellTestAccess::RequestAutomaticGitSidebarRefresh(shell);
  Expect(WaitForGitSidebarEntryCount(shell, 3),
         "automatic refresh should preserve outgoing rows while source control is active");
  Expect(count_outgoing() == 2,
         "automatic refresh should keep the outgoing section populated for active source control");
}

void TestWorkspaceShellGitSidebarGroupsWorkflowSections() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path changed = root / "changed.cpp";
  const std::filesystem::path staged = root / "staged.cpp";
  const std::filesystem::path untracked = root / "untracked.cpp";
  const std::filesystem::path renamed_from = root / "old_name.cpp";
  WriteFile(changed, "int changed() { return 1; }\n");
  WriteFile(staged, "int staged() { return 1; }\n");
  WriteFile(renamed_from, "int renamed() { return 1; }\n");

  InitializeGitRepo(root);
  CommitAll(root, "grouping fixture", "grouping fixture");
  WriteFile(changed, "int changed() { return 2; }\n");
  WriteFile(staged, "int staged() { return 2; }\n");
  RequireGitCommandSuccess(root, {"add", "staged.cpp"}, "stage fixture file");
  RequireGitCommandSuccess(root, {"mv", "old_name.cpp", "renamed.cpp"}, "rename fixture file");
  WriteFile(untracked, "hello\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::ShowGitSidebar(shell);
  Expect(WaitForGitSidebarEntryCount(shell, 4),
         "grouping fixture should expose changed, staged, rename, and untracked rows");

  const auto& entries = WorkspaceShellTestAccess::GitSidebarEntries(shell);
  auto has_section = [&](WorkspaceShell::GitSidebarEntry::Section section) {
    return std::any_of(entries.begin(), entries.end(),
                       [&](const WorkspaceShell::GitSidebarEntry& entry) {
                         return entry.section == section;
                       });
  };
  Expect(std::any_of(entries.begin(), entries.end(),
                     [](const WorkspaceShell::GitSidebarEntry& entry) {
                       return entry.relative_path == std::filesystem::path("changed.cpp") &&
                              entry.section == WorkspaceShell::GitSidebarEntry::Section::Changed;
                     }),
         "grouping fixture should classify modified unstaged files under Changed");
  Expect(has_section(WorkspaceShell::GitSidebarEntry::Section::Staged),
         "grouping fixture should include a staged section row");
  Expect(has_section(WorkspaceShell::GitSidebarEntry::Section::Untracked),
         "grouping fixture should include an untracked section row");
  Expect(std::any_of(entries.begin(), entries.end(),
                     [](const WorkspaceShell::GitSidebarEntry& entry) {
                       return entry.relative_path == std::filesystem::path("renamed.cpp");
                     }),
         "rename fixture should surface the destination path in the sidebar");
}

void TestWorkspaceShellGitSidebarDiscardRequiresConfirmation() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "discard.cpp";
  WriteFile(source, "int value() { return 1; }\n");

  InitializeGitRepo(root);
  CommitAll(root, "discard fixture", "discard fixture");
  WriteFile(source, "int value() { return 2; }\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::ShowGitSidebar(shell);
  Expect(WaitForGitSidebarEntryCount(shell, 1),
         "discard fixture should expose one changed row");

  Expect(SendKeyDown(shell, SDLK_X, SDL_KMOD_NONE),
         "discard shortcut should be handled by the git sidebar");
  Expect(WorkspaceShellTestAccess::PromptSurfaceVisible(shell),
         "discard shortcut should open a confirmation prompt");
  Expect(ReadFile(source).find("return 2") != std::string::npos,
         "discard shortcut alone should not revert the working tree");

  WorkspaceShellTestAccess::ConfirmPromptSurface(shell);
  Expect(WaitForGitSidebarEntryCount(shell, 0),
         "confirmed discard should clear the changed row after refresh");
  Expect(ReadFile(source).find("return 1") != std::string::npos,
         "confirmed discard should restore the committed file contents");
}

// Regression (data loss): discarding an UNTRACKED file from the git sidebar must honor
// the confirm prompt's promise ("Existing file-operation policy applies (trash when
// configured)") and route through the same trash the file tree's Delete uses — NOT
// `git clean -fd`, which permanently destroys a file the user created. The trashed file
// must be recoverable from the freedesktop trash under XDG_DATA_HOME.
void TestWorkspaceShellGitSidebarDiscardUntrackedFileTrashesNotDeletes() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path trash_home = temp_dir.path() / "xdg-data-home";
  ScopedEnvVar scoped_xdg_data_home("XDG_DATA_HOME", trash_home.string());

  const std::filesystem::path tracked = root / "tracked.cpp";
  WriteFile(tracked, "int main() { return 0; }\n");
  InitializeGitRepo(root);
  CommitAll(root, "base", "base");
  // A brand-new untracked file with content that must survive as recoverable.
  const std::filesystem::path untracked = root / "notes.txt";
  WriteFile(untracked, "important untracked notes\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::ShowGitSidebar(shell);
  Expect(WaitForGitSidebarEntryCount(shell, 1),
         "the untracked file should surface as one git sidebar row");
  const auto& entries = WorkspaceShellTestAccess::GitSidebarEntries(shell);
  Expect(!entries.empty() &&
             entries[0].section == WorkspaceShell::GitSidebarEntry::Section::Untracked,
         "the row should be classified as untracked");

  WorkspaceShellTestAccess::RevealGitSidebarEntry(shell, 0);
  Expect(SendKeyDown(shell, SDLK_X, SDL_KMOD_NONE),
         "discard shortcut should be handled by the git sidebar");
  Expect(WorkspaceShellTestAccess::PromptSurfaceVisible(shell),
         "discarding an untracked file should open a confirmation prompt");
  WorkspaceShellTestAccess::ConfirmPromptSurface(shell);

  Expect(WaitForGitSidebarEntryCount(shell, 0),
         "confirmed discard should remove the untracked row after refresh");
  Expect(!std::filesystem::exists(untracked),
         "the untracked file should be gone from the worktree after discard");

  // The correctness property: it was TRASHED (recoverable), not `git clean`-deleted.
  const std::filesystem::path trash_files = trash_home / "Trash" / "files";
  bool trashed_content_found = false;
  std::error_code ec;
  if (std::filesystem::is_directory(trash_files, ec)) {
    for (const auto& entry : std::filesystem::directory_iterator(trash_files, ec)) {
      if (entry.is_regular_file(ec) &&
          ReadFile(entry.path()).find("important untracked notes") != std::string::npos) {
        trashed_content_found = true;
        break;
      }
    }
  }
  Expect(trashed_content_found,
         "the untracked file's content must be recoverable from trash, proving discard trashed "
         "it rather than permanently deleting it with git clean");
}

void TestWorkspaceShellGitSidebarKeyboardStageShortcut() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "stage.cpp";
  WriteFile(source, "int stage() { return 1; }\n");

  InitializeGitRepo(root);
  CommitAll(root, "stage shortcut fixture", "stage shortcut fixture");
  WriteFile(source, "int stage() { return 2; }\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::ShowGitSidebar(shell);
  Expect(WaitForGitSidebarEntryCount(shell, 1),
         "stage shortcut fixture should expose one changed row");

  const auto& entries = WorkspaceShellTestAccess::GitSidebarEntries(shell);
  std::size_t changed_index = 0;
  for (std::size_t i = 0; i < entries.size(); ++i) {
    if (entries[i].section == WorkspaceShell::GitSidebarEntry::Section::Changed) {
      changed_index = i;
      break;
    }
  }
  WorkspaceShellTestAccess::RevealGitSidebarEntry(shell, changed_index);

  Expect(SendKeyDown(shell, SDLK_S, SDL_KMOD_NONE),
         "stage shortcut should be handled by the git sidebar");
  Expect(WaitForGitSidebarEntryCount(shell, 1),
         "stage shortcut fixture should settle after refresh");
  Expect(std::any_of(WorkspaceShellTestAccess::GitSidebarEntries(shell).begin(),
                     WorkspaceShellTestAccess::GitSidebarEntries(shell).end(),
                     [](const WorkspaceShell::GitSidebarEntry& entry) {
                       return entry.section == WorkspaceShell::GitSidebarEntry::Section::Staged;
                     }),
         "stage shortcut should move the row into the staged section");
}

void TestWorkspaceShellGitStageFailureSurfacesFeedback() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path changed = root / "changed.cpp";
  WriteFile(changed, "int changed() { return 1; }\n");

  InitializeGitRepo(root);
  CommitAll(root, "stage failure fixture", "stage failure fixture");
  WriteFile(changed, "int changed() { return 2; }\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::ShowGitSidebar(shell);
  Expect(WaitForGitSidebarEntryCount(shell, 1),
         "stage-failure fixture should expose the single changed row");

  // Remove the repository so the staging git command fails deterministically
  // while the sidebar still believes the entry is stageable (the silent-failure
  // scenario this guards).
  std::filesystem::remove_all(root / ".git");

  Expect(!WorkspaceShellTestAccess::StageGitSidebarEntry(shell, 0),
         "staging must report failure when the underlying git command cannot run");
  const std::string feedback = WorkspaceShellTestAccess::CommandFeedbackText(shell);
  Expect(feedback.find("Failed to stage") != std::string::npos,
         "a failed stage must surface feedback to the user instead of failing silently");
  Expect(feedback.find("changed.cpp") != std::string::npos,
         "stage-failure feedback should name the affected file");
}

void TestWorkspaceShellCommitWorkflowFieldsAreKeyboardEditable() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "main.cpp";
  WriteFile(source, "int main() { return 0; }\n");
  InitializeGitRepo(root);
  CommitAll(root, "seed commit", "seed body");
  WriteFile(source, "int main() { return 1; }\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::ShowGitSidebar(shell);
  Expect(WaitForGitSidebarEntryCount(shell, 1),
         "fixture should expose a single modified entry");

  Expect(WorkspaceShellTestAccess::OpenCommitWorkflow(shell), "commit workflow should open");
  WorkspaceShellTestAccess::RenderFrame(shell);
  Expect(WorkspaceShellTestAccess::CurrentTextInputSurface(shell) ==
             microide::workspace::TextInputSurface::CommitSubject,
         "the commit subject should be the active text-input surface");

  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "Fix the bug"),
         "typing should be accepted by the commit subject");
  Expect(WorkspaceShellTestAccess::CommitSubjectText(shell) == "Fix the bug",
         "typed text should populate the commit subject");
  WorkspaceShellTestAccess::RenderFrame(shell);

  // While a commit field owns the keyboard, git-action keys are gated: 's' (Stage) and the
  // Down navigation key must be ignored by the git sidebar (returning unhandled) instead of
  // staging the file or moving the git selection.
  Expect(!WorkspaceShellTestAccess::HandleKeyDown(shell, SDLK_S, SDL_KMOD_NONE),
         "'s' must not trigger Stage while the commit subject is focused");
  Expect(!WorkspaceShellTestAccess::HandleKeyDown(shell, SDLK_DOWN, SDL_KMOD_NONE),
         "Down must not move the git selection while the commit subject is focused");

  // Tab moves focus to the body; typing then lands in the body.
  Expect(WorkspaceShellTestAccess::HandleKeyDown(shell, SDLK_TAB, SDL_KMOD_NONE),
         "Tab should be consumed by the commit panel");
  Expect(WorkspaceShellTestAccess::CommitWorkflow(shell).focus_field ==
             microide::workspace::CommitWorkflowFocusField::Body,
         "Tab should move focus to the commit body");
  Expect(WorkspaceShellTestAccess::CurrentTextInputSurface(shell) ==
             microide::workspace::TextInputSurface::CommitBody,
         "the commit body should be the active text-input surface after Tab");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "More detail"),
         "typing should be accepted by the commit body");
  Expect(WorkspaceShellTestAccess::CommitBodyText(shell).find("More detail") != std::string::npos,
         "typed text should populate the commit body");

  // Exercise the multi-line body render path: add many lines (far more than the body's
  // visible rows, so most are scrolled off), select across them, and render to confirm the
  // panel paints without faulting. A large body also guards TD-2026-07-17-083: the commit
  // body must paint via per-visible-row LineView, never a whole-buffer TextBuffer::Snapshot().
  for (int i = 0; i < 40; ++i) {
    Expect(WorkspaceShellTestAccess::HandleKeyDown(shell, SDLK_RETURN, SDL_KMOD_NONE),
           "Enter should insert a newline in the commit body");
    Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "more body text"),
           "typing should be accepted by the commit body");
  }
  WorkspaceShellTestAccess::HandleKeyDown(shell, SDLK_HOME, SDL_KMOD_CTRL);
  WorkspaceShellTestAccess::HandleKeyDown(shell, SDLK_END, SDL_KMOD_CTRL | SDL_KMOD_SHIFT);
  // No editor/merge tab is open, so the commit body is the only TextViewport in the frame;
  // a whole-buffer snapshot here could only come from the commit-body paint path.
  microide::editor::TextBuffer::reset_snapshot_build_count();
  WorkspaceShellTestAccess::RenderFrame(shell);
  Expect(microide::editor::TextBuffer::snapshot_build_count() == 0,
         "commit-body render must not materialize a whole-buffer snapshot");

  // Shift+Tab returns to the subject.
  Expect(WorkspaceShellTestAccess::HandleKeyDown(shell, SDLK_TAB, SDL_KMOD_SHIFT),
         "Shift+Tab should be consumed by the commit panel");
  Expect(WorkspaceShellTestAccess::CommitWorkflow(shell).focus_field ==
             microide::workspace::CommitWorkflowFocusField::Subject,
         "Shift+Tab should move focus back to the commit subject");
}

// Git write operations run on the project background executor and publish through a
// main-thread mailbox, so a test must drain that mailbox rather than sleep.
bool SettleGitOperation(WorkspaceShell& shell) {
  return WaitUntil([&shell]() { return !WorkspaceShellTestAccess::GitOperationBusy(shell); },
                   std::chrono::seconds(10), std::chrono::milliseconds(5),
                   [&shell]() { WorkspaceShellTestAccess::DrainGitOperationCompletions(shell); });
}

std::string CurrentGitBranch(const std::filesystem::path& root) {
  for (const auto& branch : microide::project::CollectGitBranches(root)) {
    if (branch.is_head) {
      return branch.label;
    }
  }
  return {};
}

// End-to-end through the action layer: `git-create-branch` and `git-switch-branch`
// must actually move HEAD, and must do so off the shell thread.
void TestWorkspaceShellGitBranchActionsSwitchHead() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  WriteFile(root / "a.txt", "one\n");
  InitializeGitRepo(root);
  CommitAll(root, "initial", "git action fixture");
  const std::string base_branch = CurrentGitBranch(root);
  Expect(!base_branch.empty(), "the fixture repo should start on a branch");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::ShowGitSidebar(shell);
  Expect(WaitForGitSidebarEntryCount(shell, 0), "a clean fixture repo should list no entries");

  Expect(WorkspaceShellTestAccess::ExecuteAction(shell, WorkspaceShell::ActionId::GitCreateBranch,
                                                 {"topic"}),
         "git-create-branch should be accepted");
  Expect(SettleGitOperation(shell), "the create-branch operation should complete");
  Expect(CurrentGitBranch(root) == "topic", "git-create-branch should check out the new branch");

  Expect(WorkspaceShellTestAccess::ExecuteAction(shell, WorkspaceShell::ActionId::GitSwitchBranch,
                                                 {base_branch}),
         "git-switch-branch should be accepted");
  Expect(SettleGitOperation(shell), "the switch operation should complete");
  Expect(CurrentGitBranch(root) == base_branch, "git-switch-branch should move HEAD back");
}

// A missing required argument must be rejected before anything is dispatched, and a
// git failure must surface as a notification rather than being swallowed.
void TestWorkspaceShellGitOperationFailuresAreReported() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  WriteFile(root / "a.txt", "one\n");
  InitializeGitRepo(root);
  CommitAll(root, "initial", "git failure fixture");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::ShowGitSidebar(shell);
  Expect(WaitForGitSidebarEntryCount(shell, 0), "the fixture repo should be clean");

  WorkspaceShellTestAccess::ExecuteAction(shell, WorkspaceShell::ActionId::GitSwitchBranch, {});
  Expect(!WorkspaceShellTestAccess::GitOperationBusy(shell),
         "a missing branch argument must be rejected without dispatching git");

  Expect(WorkspaceShellTestAccess::ExecuteAction(shell, WorkspaceShell::ActionId::GitSwitchBranch,
                                                 {"no-such-branch"}),
         "a switch to a missing branch should still dispatch");
  Expect(SettleGitOperation(shell), "the failing switch should complete");
  const auto& notifications = WorkspaceShellTestAccess::ActiveNotifications(shell);
  Expect(!notifications.empty(), "a failed git operation should post a notification");
  Expect(notifications.back().tone != microide::workspace::NotificationService::Tone::Info,
         "a failure notification should not be posted as plain info");
}

// Stash push/pop through the action layer must move the working tree, and the shell
// must re-read the file it rewrote underneath.
void TestWorkspaceShellGitStashActionsRoundTrip() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path file = root / "a.txt";
  WriteFile(file, "one\n");
  InitializeGitRepo(root);
  CommitAll(root, "initial", "stash action fixture");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::ShowGitSidebar(shell);
  Expect(WaitForGitSidebarEntryCount(shell, 0), "the fixture repo should start clean");

  WriteFile(file, "stashed edit\n");
  Expect(WorkspaceShellTestAccess::ExecuteAction(shell, WorkspaceShell::ActionId::GitStash, {}),
         "git-stash should be accepted");
  Expect(SettleGitOperation(shell), "the stash operation should complete");
  Expect(ReadFile(file) == "one\n", "stashing should restore the committed content on disk");

  Expect(WorkspaceShellTestAccess::ExecuteAction(shell, WorkspaceShell::ActionId::GitStashPop, {}),
         "git-stash-pop should be accepted");
  Expect(SettleGitOperation(shell), "the pop operation should complete");
  Expect(ReadFile(file) == "stashed edit\n", "popping should restore the stashed edit");
}

// The branch picker is how a branch switch is actually reached: `git-switch-branch`
// with no argument opens it, it lists every branch except the one HEAD is on, and
// activating a row checks that branch out.
void TestWorkspaceShellGitBranchPickerSwitchesBranch() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  WriteFile(root / "a.txt", "one\n");
  InitializeGitRepo(root);
  CommitAll(root, "initial", "branch picker fixture");
  const std::string base_branch = CurrentGitBranch(root);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::ShowGitSidebar(shell);
  Expect(WaitForGitSidebarEntryCount(shell, 0), "the fixture repo should start clean");

  Expect(WorkspaceShellTestAccess::ExecuteAction(shell, WorkspaceShell::ActionId::GitCreateBranch,
                                                 {"topic"}),
         "creating the second branch should be accepted");
  Expect(SettleGitOperation(shell), "the create-branch operation should complete");
  Expect(CurrentGitBranch(root) == "topic", "the fixture should now be on topic");

  // No argument means "let me choose".
  Expect(WorkspaceShellTestAccess::ExecuteAction(shell, WorkspaceShell::ActionId::GitSwitchBranch,
                                                 {}),
         "argument-less git-switch-branch should be accepted");
  Expect(SettleComparePicker(shell), "the branch picker should finish loading");

  const auto refs = WorkspaceShellTestAccess::ComparePickerMatchRefs(shell);
  Expect(std::find(refs.begin(), refs.end(), base_branch) != refs.end(),
         "the picker should offer the other local branch");
  Expect(std::find(refs.begin(), refs.end(), "topic") == refs.end(),
         "the picker must not offer the branch HEAD is already on");

  const auto selected = std::find(refs.begin(), refs.end(), base_branch);
  WorkspaceShellTestAccess::SetComparePickerSelection(
      shell, static_cast<std::size_t>(selected - refs.begin()));
  WorkspaceShellTestAccess::ActivateComparePickerSelection(shell);
  Expect(SettleGitOperation(shell), "activating the picker row should run the switch");
  Expect(CurrentGitBranch(root) == base_branch,
         "activating a branch row should check that branch out");
}

// The branch row is the mouse-reachable surface for branch work. Clicking the
// branch button opens the switch picker; the rows must not overlap the working-tree
// action row above them or the clicks would land on the wrong control.
void TestWorkspaceShellGitSidebarBranchRowIsClickable() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  WriteFile(root / "a.txt", "one\n");
  InitializeGitRepo(root);
  CommitAll(root, "initial", "branch row fixture");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::ShowGitSidebar(shell);
  Expect(WaitForGitSidebarEntryCount(shell, 0), "the fixture repo should start clean");

  const auto top_row = WorkspaceShellTestAccess::GitSidebarTopActionRects(shell);
  const SDL_FRect branch_rect = WorkspaceShellTestAccess::GitSidebarBranchButtonRect(shell);
  const SDL_FRect sync_rect = WorkspaceShellTestAccess::GitSidebarSyncButtonRect(shell);
  Expect(branch_rect.w > 0.0f && sync_rect.w > 0.0f, "both branch-row buttons should be laid out");
  Expect(branch_rect.y > top_row[0].y + top_row[0].h - 1.0f,
         "the branch row must sit below the working-tree action row, not overlap it");
  Expect(!RectsIntersect(branch_rect, sync_rect),
         "the branch and sync buttons must not overlap each other");
  Expect(sync_rect.x > branch_rect.x, "sync should sit to the right of the branch button");

  Expect(SendMouseDown(shell, branch_rect.x + branch_rect.w * 0.5f,
                       branch_rect.y + branch_rect.h * 0.5f, SDL_BUTTON_LEFT),
         "clicking the branch button should be handled");
  Expect(SettleComparePicker(shell), "the branch picker should open and finish loading");
  Expect(WorkspaceShellTestAccess::OverlayVisible(shell),
         "clicking the branch button should open the picker overlay");
}

}  // namespace

void RegisterWorkspaceShellSourceControlTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceShell/GitBranchActionsSwitchHead",
          TestWorkspaceShellGitBranchActionsSwitchHead);
  AddTest(tests, "WorkspaceShell/GitOperationFailuresAreReported",
          TestWorkspaceShellGitOperationFailuresAreReported);
  AddTest(tests, "WorkspaceShell/GitStashActionsRoundTrip",
          TestWorkspaceShellGitStashActionsRoundTrip);
  AddTest(tests, "WorkspaceShell/GitBranchPickerSwitchesBranch",
          TestWorkspaceShellGitBranchPickerSwitchesBranch);
  AddTest(tests, "WorkspaceShell/GitSidebarBranchRowIsClickable",
          TestWorkspaceShellGitSidebarBranchRowIsClickable);
  AddTest(tests, "WorkspaceShell/GitSidebarRefreshPreservesActiveEditorBlameCache",
          TestWorkspaceShellGitSidebarRefreshPreservesActiveEditorBlameCache);
  AddTest(tests, "WorkspaceShell/GitSidebarEntryRightClickOpensContextMenu",
          TestWorkspaceShellGitSidebarEntryRightClickOpensContextMenu);
  AddTest(tests, "WorkspaceShell/GitSidebarLeftClickDoesNotFireActions",
          TestWorkspaceShellGitSidebarLeftClickDoesNotFireActions);
  AddTest(tests, "WorkspaceShell/GitSidebarContextMenuActionsActOnSelectedEntry",
          TestWorkspaceShellGitSidebarContextMenuActionsActOnSelectedEntry);
  AddTest(tests, "WorkspaceShell/OpeningGitSidebarEntryAlsoInvalidatesSidebarSelection",
          TestWorkspaceShellOpeningGitSidebarEntryAlsoInvalidatesSidebarSelection);
  AddTest(tests, "WorkspaceShell/GitSidebarUntrackedEntryOpensEditor",
          TestWorkspaceShellGitSidebarUntrackedEntryOpensEditor);
  AddTest(tests, "WorkspaceShell/GitOutgoingBaseChoiceRefreshesOutgoingEntries",
          TestWorkspaceShellGitOutgoingBaseChoiceRefreshesOutgoingEntries);
  AddTest(tests, "WorkspaceShell/GitOutgoingBaseButtonOpensMenuAndPrompt",
          TestWorkspaceShellGitOutgoingBaseButtonOpensMenuAndPrompt);
  AddTest(tests, "WorkspaceShell/GitSidebarKeepsOutgoingRowsWhenFilesAlsoHaveWorkingTreeChanges",
          TestWorkspaceShellGitSidebarKeepsOutgoingRowsWhenFilesAlsoHaveWorkingTreeChanges);
  AddTest(tests, "WorkspaceShell/GitSidebarGroupsWorkflowSections",
          TestWorkspaceShellGitSidebarGroupsWorkflowSections);
  AddTest(tests, "WorkspaceShell/GitSidebarDiscardRequiresConfirmation",
          TestWorkspaceShellGitSidebarDiscardRequiresConfirmation);
  AddTest(tests, "WorkspaceShell/GitSidebarDiscardUntrackedFileTrashesNotDeletes",
          TestWorkspaceShellGitSidebarDiscardUntrackedFileTrashesNotDeletes);
  AddTest(tests, "WorkspaceShell/GitSidebarKeyboardStageShortcut",
          TestWorkspaceShellGitSidebarKeyboardStageShortcut);
  AddTest(tests, "WorkspaceShell/GitStageFailureSurfacesFeedback",
          TestWorkspaceShellGitStageFailureSurfacesFeedback);
  AddTest(tests, "WorkspaceShell/CommitWorkflowFieldsAreKeyboardEditable",
          TestWorkspaceShellCommitWorkflowFieldsAreKeyboardEditable);
}

}  // namespace microide::tests
