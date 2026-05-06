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

  Expect(WorkspaceShellTestAccess::GitSidebarEntries(shell).size() == 1,
         "git sidebar tooltip fixture should expose a single modified entry");

  const auto top_action_rects = WorkspaceShellTestAccess::GitSidebarTopActionRects(shell);
  SendMouseMotion(
      shell, top_action_rects[0].x + top_action_rects[0].w * 0.5f,
      top_action_rects[0].y + top_action_rects[0].h * 0.5f, 0);
  Expect(WorkspaceShellTestAccess::HoveredGitSidebarTooltipLabel(shell).empty(),
         "hovering the full-width stage-all button should not show a tooltip");

  const auto action_rects = WorkspaceShellTestAccess::GitSidebarEntryActionRects(shell, 0);
  SendMouseMotion(shell, action_rects[0].x + action_rects[0].w * 0.5f,
                                              action_rects[0].y + action_rects[0].h * 0.5f, 0);
  Expect(WorkspaceShellTestAccess::HoveredGitSidebarTooltipLabel(shell) == "Stage",
         "hovering the compact stage button should expose the full action name");

  SendMouseMotion(shell, action_rects[0].x - 2.0f,
                                              action_rects[0].y + action_rects[0].h * 0.5f, 0);
  Expect(WorkspaceShellTestAccess::HoveredGitSidebarTooltipLabel(shell) == "Stage",
         "stage button hover should tolerate a small hitbox miss");

  SendMouseMotion(shell, action_rects[1].x + action_rects[1].w * 0.5f,
                                              action_rects[1].y + action_rects[1].h * 0.5f, 0);
  Expect(WorkspaceShellTestAccess::HoveredGitSidebarTooltipLabel(shell) == "Discard",
         "hovering the compact discard button should expose the full action name");

  Expect(WorkspaceShellTestAccess::StageAllGitSidebarEntries(shell),
         "staging the tooltip fixture should succeed");
  WorkspaceShellTestAccess::ConsumeGitSidebarRefresh(shell);
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
  Expect(entries.size() == 2,
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

void TestWorkspaceShellGitOutgoingBaseChoiceRefreshesOutgoingEntries() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path alpha = root / "src" / "alpha.cpp";
  const std::filesystem::path beta = root / "src" / "beta.cpp";
  WriteFile(alpha, "int alpha() {\n  return 1;\n}\n");
  WriteFile(beta, "int beta() {\n  return 2;\n}\n");

  InitializeGitRepo(root);
  CommitAll(root, "base fixture", "base fixture");
  RequireCommandSuccess(
      "git -C '" + EscapedRepoPath(root) + "' checkout -b feature/outgoing-base >/dev/null 2>/dev/null",
      "git checkout feature branch");

  WriteFile(alpha, "int alpha() {\n  return 10;\n}\n");
  CommitAll(root, "feature alpha", "feature alpha");
  WriteFile(beta, "int beta() {\n  return 20;\n}\n");
  CommitAll(root, "feature beta", "feature beta");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::ShowGitSidebar(shell);
  WorkspaceShellTestAccess::ConsumeGitSidebarRefresh(shell);

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
  WorkspaceShellTestAccess::ConsumeGitSidebarRefresh(shell);
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
  WorkspaceShellTestAccess::ConsumeGitSidebarRefresh(shell);
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
  WorkspaceShellTestAccess::ConsumeGitSidebarRefresh(shell);
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

}  // namespace

void RegisterWorkspaceShellSourceControlTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceShell/GitSidebarRefreshPreservesActiveEditorBlameCache",
          TestWorkspaceShellGitSidebarRefreshPreservesActiveEditorBlameCache);
  AddTest(tests, "WorkspaceShell/GitSidebarCompactButtonsExposeHoverTooltips",
          TestWorkspaceShellGitSidebarCompactButtonsExposeHoverTooltips);
  AddTest(tests, "WorkspaceShell/OpeningGitSidebarEntryAlsoInvalidatesSidebarSelection",
          TestWorkspaceShellOpeningGitSidebarEntryAlsoInvalidatesSidebarSelection);
  AddTest(tests, "WorkspaceShell/GitOutgoingBaseChoiceRefreshesOutgoingEntries",
          TestWorkspaceShellGitOutgoingBaseChoiceRefreshesOutgoingEntries);
  AddTest(tests, "WorkspaceShell/GitOutgoingBaseButtonOpensMenuAndPrompt",
          TestWorkspaceShellGitOutgoingBaseButtonOpensMenuAndPrompt);
}

}  // namespace microide::tests
