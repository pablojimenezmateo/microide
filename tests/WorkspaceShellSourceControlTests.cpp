#include "TestSupport.h"

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

bool RectsIntersect(const SDL_FRect& lhs, const SDL_FRect& rhs) {
  return lhs.x < rhs.x + rhs.w && lhs.x + lhs.w > rhs.x && lhs.y < rhs.y + rhs.h &&
         lhs.y + lhs.h > rhs.y;
}

bool AnyRectIntersects(const std::vector<SDL_FRect>& rects, const SDL_FRect& target) {
  return std::any_of(rects.begin(), rects.end(),
                     [&](const SDL_FRect& rect) { return RectsIntersect(rect, target); });
}

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
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    WorkspaceShellTestAccess::ConsumeGitSidebarRefresh(shell);
    if (WorkspaceShellTestAccess::GitSidebarEntries(shell).size() == expected_count &&
        !WorkspaceShellTestAccess::GitSidebarRefreshing(shell)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  WorkspaceShellTestAccess::ConsumeGitSidebarRefresh(shell);
  return WorkspaceShellTestAccess::GitSidebarEntries(shell).size() == expected_count;
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

void TestWorkspaceShellGitSidebarCompactButtonsExposeHoverTooltips() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "deep" / "main.cpp";
  WriteFile(source, "int alpha() {\n  return 1;\n}\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add git sidebar tooltip fixture", "git sidebar tooltip fixture");
  WriteFile(source, "int beta() {\n  return 2;\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::ShowGitSidebar(shell);

  Expect(WaitForGitSidebarEntryCount(shell, 1),
         "git sidebar tooltip fixture should expose a single modified entry");
  WorkspaceShellTestAccess::RevealGitSidebarEntry(shell, 0);

  const auto top_action_rects = WorkspaceShellTestAccess::GitSidebarTopActionRects(shell);
  SendMouseMotion(
      shell, top_action_rects[0].x + top_action_rects[0].w * 0.5f,
      top_action_rects[0].y + top_action_rects[0].h * 0.5f, 0);
  Expect(WorkspaceShellTestAccess::HoveredGitSidebarTooltipLabel(shell).empty(),
         "hovering the full-width stage-all button should not show a tooltip");

  const SDL_FRect row_rect = WorkspaceShellTestAccess::GitSidebarEntryRowRect(shell, 0);
  const float stage_button_x = row_rect.x + row_rect.w - 12.0f;
  const float row_center_y = row_rect.y + row_rect.h * 0.5f;
  SendMouseMotion(shell, stage_button_x, row_center_y, 0);
  Expect(WorkspaceShellTestAccess::HoveredGitSidebarTooltipLabel(shell) == "Stage",
         "hovering the compact stage button should expose the full action name");

  SendMouseMotion(shell, stage_button_x - 2.0f, row_center_y, 0);
  Expect(WorkspaceShellTestAccess::HoveredGitSidebarTooltipLabel(shell) == "Stage",
         "stage button hover should tolerate a small hitbox miss");

  const auto action_rects = WorkspaceShellTestAccess::GitSidebarEntryActionRects(shell, 0);
  SendMouseMotion(shell, action_rects[1].x + action_rects[1].w * 0.5f,
                                              action_rects[1].y + action_rects[1].h * 0.5f, 0);
  Expect(WorkspaceShellTestAccess::HoveredGitSidebarTooltipLabel(shell) == "Discard",
         "hovering the compact discard button should expose the full action name");

  Expect(WorkspaceShellTestAccess::StageAllGitSidebarEntries(shell),
         "staging the tooltip fixture should succeed");
  Expect(WaitForGitSidebarEntryCount(shell, 1),
         "git sidebar should re-populate after the stage-all refresh completes");
  const auto staged_action_rects = WorkspaceShellTestAccess::GitSidebarEntryActionRects(shell, 0);
  SendMouseMotion(
      shell, staged_action_rects[0].x + staged_action_rects[0].w * 0.5f,
      staged_action_rects[0].y + staged_action_rects[0].h * 0.5f, 0);
  Expect(WorkspaceShellTestAccess::HoveredGitSidebarTooltipLabel(shell) == "Unstage",
         "hovering the compact unstage button should expose the full action name");
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
  const auto action_rects =
      WorkspaceShellTestAccess::GitSidebarEntryActionRects(shell, untracked_index);
  Expect(action_rects[0].w > 0.0f && action_rects[1].w > 0.0f,
         "untracked rows should still expose stage and discard buttons");

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
  Expect(state.sidebar.git.base_ref == "main",
         "auto outgoing base should resolve the repository base branch");
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
             labels[1] == "Previous commit (HEAD~1)" && labels[2] == "Specific ref...",
         "outgoing base menu should expose the three base-selection choices");

  const auto specific_ref_rect = WorkspaceShellTestAccess::PopupMenuItemRect(
      shell, WorkspaceShell::MenuId::GitOutgoingBase, "Specific ref...");
  Expect(specific_ref_rect.has_value(),
         "outgoing base menu should expose the specific-ref prompt entry");
  Expect(SendMouseDown(shell, specific_ref_rect->x + specific_ref_rect->w * 0.5f,
                       specific_ref_rect->y + specific_ref_rect->h * 0.5f, SDL_BUTTON_LEFT),
         "clicking the specific-ref outgoing base entry should be handled");

  Expect(WorkspaceShellTestAccess::PromptSurfaceVisible(shell),
         "specific-ref outgoing base entry should open the prompt surface");
  Expect(WorkspaceShellTestAccess::PromptSurfaceTitle(shell) == "Outgoing Base Ref",
         "specific-ref outgoing base prompt should use the dedicated title");
  Expect(WorkspaceShellTestAccess::PromptSurfaceInput(shell) == "origin/release/2026-04",
         "specific-ref outgoing base prompt should prefill the saved custom ref");
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

}  // namespace

void RegisterWorkspaceShellSourceControlTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceShell/GitSidebarRefreshPreservesActiveEditorBlameCache",
          TestWorkspaceShellGitSidebarRefreshPreservesActiveEditorBlameCache);
  AddTest(tests, "WorkspaceShell/GitSidebarCompactButtonsExposeHoverTooltips",
          TestWorkspaceShellGitSidebarCompactButtonsExposeHoverTooltips);
  AddTest(tests, "WorkspaceShell/OpeningGitSidebarEntryAlsoInvalidatesSidebarSelection",
          TestWorkspaceShellOpeningGitSidebarEntryAlsoInvalidatesSidebarSelection);
  AddTest(tests, "WorkspaceShell/GitSidebarUntrackedEntryOpensEditor",
          TestWorkspaceShellGitSidebarUntrackedEntryOpensEditor);
  AddTest(tests, "WorkspaceShell/GitOutgoingBaseChoiceRefreshesOutgoingEntries",
          TestWorkspaceShellGitOutgoingBaseChoiceRefreshesOutgoingEntries);
  AddTest(tests, "WorkspaceShell/GitOutgoingBaseButtonOpensMenuAndPrompt",
          TestWorkspaceShellGitOutgoingBaseButtonOpensMenuAndPrompt);
  AddTest(tests, "WorkspaceShell/GitSidebarGroupsWorkflowSections",
          TestWorkspaceShellGitSidebarGroupsWorkflowSections);
  AddTest(tests, "WorkspaceShell/GitSidebarDiscardRequiresConfirmation",
          TestWorkspaceShellGitSidebarDiscardRequiresConfirmation);
  AddTest(tests, "WorkspaceShell/GitSidebarKeyboardStageShortcut",
          TestWorkspaceShellGitSidebarKeyboardStageShortcut);
}

}  // namespace microide::tests
