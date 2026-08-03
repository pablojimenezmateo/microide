#include "TestSupport.h"

#include "platform/AppDirectories.h"
#include "persistence/PersistedRecordWriter.h"
#include "workspace/render/DiffDividerGeometry.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"
#include "workspace/persistence/WorkspacePersistenceCoordinator.h"
#include "workspace/persistence/WorkspacePersistenceFormat.h"
#include "workspace/WorkspaceProjectPresentation.h"
#include "project/GitCompareService.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "WorkspaceShellEventHelpers.h"

namespace microide::tests {
namespace {


using microide::workspace::PersistenceCoordinator;
using microide::workspace::WorkspaceShell;
using WorkspaceShellTestAccess = microide::workspace::WorkspaceShell::TestAccess;

// Divider grab rects for the active merge tab. DiffDividerGeometry.h takes
// WorkspaceShell by name, so it cannot be reached from inside the TestAccess class
// body; this lives here instead and reads the same geometry production does.
std::array<SDL_FRect, 2> MergeDividerRectsOf(microide::workspace::WorkspaceShell& shell) {
  const auto layout = WorkspaceShellTestAccess::CurrentLayout(shell);
  return microide::workspace::MergeDividerHitRects(
      layout.editor_surface, WorkspaceShellTestAccess::ActiveMergeSurfaceLayout(shell));
}
using microide::compare::MergeChoice;

class ScopedSessionAppHomes {
 public:
  ScopedSessionAppHomes(const std::filesystem::path& state_home,
                        const std::filesystem::path& config_home)
      : xdg_state_home_("XDG_STATE_HOME", state_home.string()),
        xdg_config_home_("XDG_CONFIG_HOME", config_home.string()),
        localappdata_("LOCALAPPDATA", state_home.string()),
        appdata_("APPDATA", config_home.string()) {}

 private:
  ScopedEnvVar xdg_state_home_;
  ScopedEnvVar xdg_config_home_;
  ScopedEnvVar localappdata_;
  ScopedEnvVar appdata_;
};

void TestWorkspaceShellRestoreSessionPreservesBranchCompareState() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "history.txt";
  WriteFile(source, "base line\n");

  const std::filesystem::path home = temp_dir.path() / "home";
  const std::filesystem::path xdg_state_home = temp_dir.path() / "xdg-state-home";
  const std::filesystem::path xdg_config_home = temp_dir.path() / "xdg-config-home";
  std::filesystem::create_directories(home);
  std::filesystem::create_directories(xdg_state_home);
  std::filesystem::create_directories(xdg_config_home);
  ScopedEnvVar scoped_home("HOME", home.string());
  ScopedSessionAppHomes scoped_app_homes(xdg_state_home, xdg_config_home);

  InitializeGitRepo(root);
  CommitAll(root, "base fixture", "base fixture");
  WriteFile(source, "head line\n");
  CommitAll(root, "head fixture", "head fixture");

  const auto history = microide::project::CollectGitFileHistory(root, source).commits;
  Expect(history.size() == 2, "session restore fixture should have two commits");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenBranchHeadComparison(shell, source, history[1].hash, "base",
                                                            history[0].hash, "head"),
         "branch comparison should open");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  const std::size_t expected_row =
      compare.model.rows.empty() ? 0 : std::min<std::size_t>(1, compare.model.rows.size() - 1);
  compare.selected_row = expected_row;
  compare.scroll_row = 2;
  compare.horizontal_scroll = 4;
  compare.divider_fraction = 0.68f;
  WorkspaceShellTestAccess::SaveSessionState(shell);

  WorkspaceShell restored;
  WorkspaceShellTestAccess::SetProjectRoot(restored, root);
  Expect(WorkspaceShellTestAccess::RestoreSessionState(restored),
         "project-local session restore should succeed");
  Expect(WorkspaceShellTestAccess::OpenTabs(restored).size() == 1,
         "restored branch comparison should reopen as a single tab");

  const auto& rebuilt = WorkspaceShellTestAccess::ActiveCompare(restored);
  Expect(rebuilt.path == source.lexically_normal(),
         "restored branch comparison should keep the original path");
  Expect(rebuilt.commit_hash == history[1].hash,
         "restored branch comparison should preserve the left-side ref");
  Expect(rebuilt.right_ref == history[0].hash,
         "restored branch comparison should preserve the right-side ref");
  Expect(rebuilt.left_label == "base",
         "restored branch comparison should preserve the left label");
  Expect(rebuilt.right_label == "head",
         "restored branch comparison should preserve the right label");
  Expect(rebuilt.selected_row == expected_row,
         "restored branch comparison should preserve the selected row");
  Expect(rebuilt.scroll_row == 2,
         "restored branch comparison should preserve vertical scroll");
  Expect(rebuilt.horizontal_scroll == 4,
         "restored branch comparison should preserve horizontal scroll");
  Expect(std::fabs(rebuilt.divider_fraction - 0.68f) < 0.0001f,
         "restored branch comparison should preserve divider fraction");
  Expect(rebuilt.persistable,
         "restored branch comparison should preserve its session-persistable flag");
}


void TestWorkspaceShellRestoreWorkspaceSessionAcrossProjects() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path repo_root = temp_dir.path() / "repo";
  const std::filesystem::path repo_file = repo_root / "src" / "compare.txt";
  WriteFile(repo_file, "base line\n");

  const std::filesystem::path second_root = temp_dir.path() / "notes-project";
  const std::filesystem::path second_file = second_root / "notes.txt";
  WriteFile(second_file, "notes\n");

  const std::filesystem::path home = temp_dir.path() / "home";
  const std::filesystem::path xdg_state_home = temp_dir.path() / "xdg-state-home";
  const std::filesystem::path xdg_config_home = temp_dir.path() / "xdg-config-home";
  std::filesystem::create_directories(home);
  std::filesystem::create_directories(xdg_state_home);
  std::filesystem::create_directories(xdg_config_home);
  ScopedEnvVar scoped_home("HOME", home.string());
  ScopedSessionAppHomes scoped_app_homes(xdg_state_home, xdg_config_home);

  InitializeGitRepo(repo_root);
  CommitAll(repo_root, "base fixture", "base fixture");
  WriteFile(repo_file, "head line\n");
  CommitAll(repo_root, "head fixture", "head fixture");

  const auto history = microide::project::CollectGitFileHistory(repo_root, repo_file).commits;
  Expect(history.size() == 2, "workspace restore fixture should have two commits");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, repo_root, false, false),
         "first project should open");
  Expect(WorkspaceShellTestAccess::OpenBranchHeadComparison(shell, repo_file, history[1].hash, "base",
                                                            history[0].hash, "head"),
         "branch comparison should open in the first project");
  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  const std::size_t expected_row =
      compare.model.rows.empty() ? 0 : std::min<std::size_t>(1, compare.model.rows.size() - 1);
  compare.selected_row = expected_row;
  compare.scroll_row = 3;
  compare.horizontal_scroll = 6;

  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, second_root, false, false),
         "second project should open");
  WorkspaceShellTestAccess::OpenFile(shell, second_file);
  Expect(WorkspaceShellTestAccess::OpenTabs(shell).size() == 1,
         "second project should persist one editor tab");
  WorkspaceShellTestAccess::SaveSessionState(shell);
  WorkspaceShellTestAccess::SaveWorkspaceSession(shell);

  WorkspaceShell restored;
  Expect(WorkspaceShellTestAccess::RestoreWorkspaceSession(restored),
         "workspace session restore should succeed");
  Expect(WorkspaceShellTestAccess::ProjectCount(restored) == 2,
         "workspace restore should reopen both projects");
  Expect(WorkspaceShellTestAccess::ActiveProjectIndex(restored) == 1,
         "workspace restore should preserve the active project index");
  Expect(WorkspaceShellTestAccess::ProjectRoot(restored) == second_root.lexically_normal(),
         "workspace restore should reactivate the second project");
  Expect(WorkspaceShellTestAccess::ActiveEditor(restored).path() == second_file.lexically_normal(),
         "workspace restore should reopen the active project's editor tab");

  Expect(WorkspaceShellTestAccess::SwitchProject(restored, 0, false),
         "switching to the restored first project should succeed");
  Expect(WorkspaceShellTestAccess::ProjectRoot(restored) == repo_root.lexically_normal(),
         "switch should activate the first restored project");
  Expect(WorkspaceShellTestAccess::OpenTabs(restored).size() == 1,
         "first restored project should reopen the compare tab");

  const auto& rebuilt = WorkspaceShellTestAccess::ActiveCompare(restored);
  Expect(rebuilt.path == repo_file.lexically_normal(),
         "restored first project should keep the compare path");
  Expect(rebuilt.commit_hash == history[1].hash,
         "restored first project should preserve the compare left ref");
  Expect(rebuilt.right_ref == history[0].hash,
         "restored first project should preserve the compare right ref");
  Expect(rebuilt.left_label == "base",
         "restored first project should preserve the compare left label");
  Expect(rebuilt.right_label == "head",
         "restored first project should preserve the compare right label");
  Expect(rebuilt.selected_row == expected_row,
         "restored first project should preserve the compare selected row");
  Expect(rebuilt.scroll_row == 3,
         "restored first project should preserve compare vertical scroll");
  Expect(rebuilt.horizontal_scroll == 6,
         "restored first project should preserve compare horizontal scroll");
}

// Regression: `active_project_index` indexes the ORIGINAL saved project list. If a
// project positioned BEFORE the active one is missing on restore, it is culled and
// every later project shifts down — so the active index must be remapped, not just
// clamped, or the wrong project activates on startup.
void TestWorkspaceShellRestoreRemapsActiveIndexAfterMissingProjectCulled() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path first_root = temp_dir.path() / "alpha";
  const std::filesystem::path second_root = temp_dir.path() / "beta";
  const std::filesystem::path third_root = temp_dir.path() / "gamma";
  WriteFile(first_root / "a.txt", "a\n");
  WriteFile(second_root / "b.txt", "b\n");
  WriteFile(third_root / "c.txt", "c\n");

  const std::filesystem::path home = temp_dir.path() / "home";
  const std::filesystem::path xdg_state_home = temp_dir.path() / "xdg-state-home";
  const std::filesystem::path xdg_config_home = temp_dir.path() / "xdg-config-home";
  std::filesystem::create_directories(home);
  std::filesystem::create_directories(xdg_state_home);
  std::filesystem::create_directories(xdg_config_home);
  ScopedEnvVar scoped_home("HOME", home.string());
  ScopedSessionAppHomes scoped_app_homes(xdg_state_home, xdg_config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, first_root, false, false), "open alpha");
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, second_root, false, false), "open beta");
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, third_root, false, false), "open gamma");
  // Activate the MIDDLE project (beta, original index 1).
  Expect(WorkspaceShellTestAccess::SwitchProject(shell, 1, false), "activate beta");
  Expect(WorkspaceShellTestAccess::ActiveProjectIndex(shell) == 1, "beta is active pre-save");
  WorkspaceShellTestAccess::SaveSessionState(shell);
  WorkspaceShellTestAccess::SaveWorkspaceSession(shell);

  // The first project vanishes between sessions.
  std::filesystem::remove_all(first_root);

  WorkspaceShell restored;
  Expect(WorkspaceShellTestAccess::RestoreWorkspaceSession(restored), "restore should succeed");
  Expect(WorkspaceShellTestAccess::ProjectCount(restored) == 2,
         "the two surviving projects are restored");
  // beta must still be the active project (now at index 0), not gamma.
  Expect(WorkspaceShellTestAccess::ProjectRoot(restored) == second_root.lexically_normal(),
         "the originally-active project (beta) must stay active after culling alpha");
  Expect(WorkspaceShellTestAccess::ActiveProjectIndex(restored) == 0,
         "beta's remapped active index is 0 after the missing predecessor is culled");
}

void TestWorkspaceShellRestoredProjectTabBadgeColorsHydrateOnStartup() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path first_root = temp_dir.path() / "alpha-project";
  const std::filesystem::path second_root = temp_dir.path() / "beta-project";
  WriteFile(first_root / "README.md", "alpha\n");
  WriteFile(second_root / "README.md", "beta\n");

  const std::filesystem::path home = temp_dir.path() / "home";
  const std::filesystem::path xdg_state_home = temp_dir.path() / "xdg-state-home";
  const std::filesystem::path xdg_config_home = temp_dir.path() / "xdg-config-home";
  std::filesystem::create_directories(home);
  std::filesystem::create_directories(xdg_state_home);
  std::filesystem::create_directories(xdg_config_home);
  ScopedEnvVar scoped_home("HOME", home.string());
  ScopedSessionAppHomes scoped_app_homes(xdg_state_home, xdg_config_home);

  const SDL_Color color_a{0x12, 0x34, 0x56, 0xff};
  const SDL_Color color_b{0xab, 0xcd, 0xef, 0xff};
  const auto colors_match = [](SDL_Color lhs, SDL_Color rhs) {
    return lhs.r == rhs.r && lhs.g == rhs.g && lhs.b == rhs.b && lhs.a == rhs.a;
  };

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, first_root, false, false),
         "project badge restore fixture should open the first project");
  WorkspaceShellTestAccess::SetProjectBaseColor(shell, color_a);
  WorkspaceShellTestAccess::SaveConfigState(shell);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, second_root, false, false),
         "project badge restore fixture should open the second project");
  WorkspaceShellTestAccess::SetProjectBaseColor(shell, color_b);
  WorkspaceShellTestAccess::SaveConfigState(shell);
  WorkspaceShellTestAccess::SaveWorkspaceSession(shell);

  WorkspaceShell restored;
  Expect(WorkspaceShellTestAccess::RestoreWorkspaceSession(restored),
         "workspace session restore should succeed for badge color hydration");
  WorkspaceShellTestAccess::SetWindowSize(restored, 1280, 720);
  Expect(WorkspaceShellTestAccess::ProjectCount(restored) == 2,
         "restored workspace should reopen both projects");
  Expect(WorkspaceShellTestAccess::ActiveProjectIndex(restored) == 1,
         "restored workspace should preserve the active project index");
  Expect(colors_match(WorkspaceShellTestAccess::ProjectTabBadgeColor(restored, 1), color_b),
         "restored active project tab badge should use its persisted color");
  Expect(colors_match(WorkspaceShellTestAccess::ProjectTabBadgeColor(restored, 0), color_a),
         "restored inactive project tab badge should hydrate its persisted color on startup");
}

void TestWorkspaceShellShutdownPreservesDistinctWorkspaceProjectRoots() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path first_root = temp_dir.path() / "project-a";
  const std::filesystem::path first_file = first_root / "a.txt";
  WriteFile(first_file, "alpha\n");

  const std::filesystem::path second_root = temp_dir.path() / "project-b";
  const std::filesystem::path second_file = second_root / "b.txt";
  WriteFile(second_file, "beta\n");

  const std::filesystem::path home = temp_dir.path() / "home";
  const std::filesystem::path xdg_state_home = temp_dir.path() / "xdg-state-home";
  const std::filesystem::path xdg_config_home = temp_dir.path() / "xdg-config-home";
  std::filesystem::create_directories(home);
  std::filesystem::create_directories(xdg_state_home);
  std::filesystem::create_directories(xdg_config_home);
  ScopedEnvVar scoped_home("HOME", home.string());
  ScopedSessionAppHomes scoped_app_homes(xdg_state_home, xdg_config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, first_root, false, false),
         "first workspace project should open");
  WorkspaceShellTestAccess::OpenFile(shell, first_file);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, second_root, false, false),
         "second workspace project should open");
  WorkspaceShellTestAccess::OpenFile(shell, second_file);
  Expect(WorkspaceShellTestAccess::SwitchProject(shell, 0, false),
         "fixture should make the first project active before shutdown");

  shell.Shutdown();

  WorkspaceShell restored;
  Expect(WorkspaceShellTestAccess::RestoreWorkspaceSession(restored),
         "workspace session restore after shutdown should succeed");
  Expect(WorkspaceShellTestAccess::ProjectCount(restored) == 2,
         "workspace restore should keep both project entries");
  Expect(WorkspaceShellTestAccess::ProjectRoot(restored) == first_root.lexically_normal(),
         "workspace restore should reactivate the first project");

  const auto restored_roots = WorkspaceShellTestAccess::ProjectRoots(restored);
  Expect(restored_roots.size() == 2,
         "restored workspace should expose two distinct project roots");
  Expect(restored_roots[0] == first_root.lexically_normal(),
         "restored workspace should preserve the first saved project root");
  Expect(restored_roots[1] == second_root.lexically_normal(),
         "restored workspace should preserve the second saved project root");
}

void TestWorkspaceShellMergeHorizontalNavigationInvalidatesResultPane() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path base = root / "base.txt";
  const std::filesystem::path incoming = root / "incoming.txt";
  const std::filesystem::path current = root / "current.txt";
  const std::filesystem::path output = root / "output.txt";
  WriteFile(base, "line 1\nline 2\nline 3\n");
  WriteFile(incoming, "line 1\nincoming 2\nline 3\n");
  WriteFile(current, "line 1\ncurrent 2\nline 3\n");
  WriteFile(output, "<<<<<<< ours\ncurrent 2\n=======\nincoming 2\n>>>>>>> theirs\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, current, output),
         "merge invalidation fixture should open");
  (void)shell.ConsumePendingRenderInvalidation();

  const auto surface = WorkspaceShellTestAccess::ActiveMergeSurfaceLayout(shell);
  const SDL_FRect result_rect = WorkspaceShellTestAccess::ActiveMergeResultRect(shell);
  const auto layout = WorkspaceShellTestAccess::CurrentLayout(shell);
  const SDL_FRect left_rect =
      microide::workspace::MakeRect(surface.left_x, layout.editor_surface.y,
                                    surface.gutter_width + surface.left_width,
                                    layout.editor_surface.h);

  SDL_Event event{};
  event.type = SDL_EVENT_KEY_DOWN;
  event.key.key = SDLK_RIGHT;
  const auto result = shell.HandleEvent(event);
  const auto redraw_rect = result.redraw.SingleRectIfOnlyOne();

  Expect(result.handled, "merge horizontal navigation should be handled");
  Expect(!result.redraw.full && redraw_rect.has_value(),
         "merge horizontal navigation should stay on a partial redraw path");
  Expect(RectsIntersect(*redraw_rect, result_rect),
         "merge horizontal navigation should repaint the result pane");
  Expect(!RectsIntersect(*redraw_rect, left_rect),
         "merge horizontal navigation should avoid repainting the incoming pane");
}

void TestWorkspaceShellMoveMergeSelectionInvalidatesConflictBand() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path base = root / "base.txt";
  const std::filesystem::path incoming = root / "incoming.txt";
  const std::filesystem::path current = root / "current.txt";
  const std::filesystem::path output = root / "output.txt";
  WriteFile(base, "line 1\nline 2\nline 3\nline 4\nline 5\n");
  WriteFile(incoming, "line 1\nincoming 2\nline 3\nincoming 4\nline 5\n");
  WriteFile(current, "line 1\ncurrent 2\nline 3\ncurrent 4\nline 5\n");
  WriteFile(output,
            "<<<<<<< ours\ncurrent 2\n=======\nincoming 2\n>>>>>>> theirs\n"
            "line 3\n"
            "<<<<<<< ours\ncurrent 4\n=======\nincoming 4\n>>>>>>> theirs\n"
            "line 5\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, current, output),
         "merge conflict invalidation fixture should open");
  (void)shell.ConsumePendingRenderInvalidation();

  const auto previous_conflict_rect = WorkspaceShellTestAccess::ActiveMergeConflictRect(shell, 0);
  WorkspaceShellTestAccess::MoveMergeSelection(shell, 1);
  const auto redraw = shell.ConsumePendingRenderInvalidation();
  const auto next_conflict_rect = WorkspaceShellTestAccess::ActiveMergeConflictRect(shell, 1);
  const auto layout = WorkspaceShellTestAccess::CurrentLayout(shell);

  Expect(!redraw.full && !redraw.rects.empty(),
         "merge conflict navigation should stay on a partial redraw path");
  Expect(previous_conflict_rect.has_value() && AnyRectCovers(redraw.rects, *previous_conflict_rect),
         "merge conflict navigation should repaint the previously selected conflict");
  Expect(next_conflict_rect.has_value() && AnyRectCovers(redraw.rects, *next_conflict_rect),
         "merge conflict navigation should repaint the newly selected conflict");
  Expect(MaxRectHeight(redraw.rects) < layout.editor_surface.h,
         "merge conflict navigation should redraw less than the full merge surface height");
}

void TestWorkspaceShellRestoreSessionPreservesRenamedWorkingTreeCompareState() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "compare.txt";
  WriteFile(source, "zero\none\ntwo\nthree\n");

  const std::filesystem::path home = temp_dir.path() / "home";
  const std::filesystem::path xdg_state_home = temp_dir.path() / "xdg-state-home";
  const std::filesystem::path xdg_config_home = temp_dir.path() / "xdg-config-home";
  std::filesystem::create_directories(home);
  std::filesystem::create_directories(xdg_state_home);
  std::filesystem::create_directories(xdg_config_home);
  ScopedEnvVar scoped_home("HOME", home.string());
  ScopedSessionAppHomes scoped_app_homes(xdg_state_home, xdg_config_home);

  InitializeGitRepo(root);
  CommitAll(root, "base fixture", "base fixture");
  WriteFile(source, "zero\none changed\ntwo changed\nthree changed\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "working-tree comparison should open before rename");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  const std::size_t expected_row =
      compare.model.rows.empty() ? 0 : std::min<std::size_t>(2, compare.model.rows.size() - 1);
  compare.selected_row = expected_row;
  compare.scroll_row = 2;
  compare.horizontal_scroll = 5;

  WorkspaceShellTestAccess::PrepareRenamePrompt(shell, root / "src", "renamed-src");
  WorkspaceShellTestAccess::ConfirmPromptSurface(shell);

  const std::filesystem::path renamed = root / "renamed-src" / "compare.txt";
  Expect(std::filesystem::is_regular_file(renamed),
         "renamed working-tree compare fixture should create the destination file");
  WorkspaceShellTestAccess::SaveSessionState(shell);

  WorkspaceShell restored;
  WorkspaceShellTestAccess::SetProjectRoot(restored, root);
  Expect(WorkspaceShellTestAccess::RestoreSessionState(restored),
         "session restore should rebuild the renamed working-tree compare");
  Expect(WorkspaceShellTestAccess::OpenTabs(restored).size() == 1,
         "renamed working-tree compare should restore as a single tab");

  const auto& rebuilt = WorkspaceShellTestAccess::ActiveCompare(restored);
  Expect(rebuilt.path == renamed.lexically_normal(),
         "restored compare should preserve the renamed live path");
  Expect(rebuilt.left_path == source.lexically_normal(),
         "restored compare should preserve the original commit-side path");
  Expect(rebuilt.right_path == renamed.lexically_normal(),
         "restored compare should preserve the renamed working-tree path");
  Expect(rebuilt.commit_hash == "HEAD",
         "restored compare should preserve the left-side ref");
  Expect(rebuilt.right_ref == "WORKTREE",
         "restored compare should preserve the working-tree right-side ref");
  Expect(rebuilt.selected_row == expected_row,
         "restored compare should preserve the selected row");
  Expect(rebuilt.scroll_row == 2,
         "restored compare should preserve vertical scroll");
  Expect(rebuilt.horizontal_scroll == 5,
         "restored compare should preserve horizontal scroll");
  const bool kept_compare_content = std::any_of(
      rebuilt.model.rows.begin(), rebuilt.model.rows.end(), [](const auto& row) {
        return row.left_text == "one" && row.right_text == "one changed";
      });
  Expect(kept_compare_content,
         "restored compare should preserve the pre-rename commit-vs-working-tree content");
}

// J15 regression: a stale/corrupt persisted `selected_launch_config_index` that
// points past the rebuilt launch-config list must be clamped/reset on restore.
// An out-of-range selection makes StartDebuggingWithDefaultConfig ignore every
// launch config and fall back to the first registered adapter with empty args.
void TestWorkspaceShellRestoreClampsOutOfRangeSelectedLaunchConfig() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.py";
  WriteFile(source, "print('hi')\n");

  const std::filesystem::path home = temp_dir.path() / "home";
  const std::filesystem::path xdg_state_home = temp_dir.path() / "xdg-state-home";
  const std::filesystem::path xdg_config_home = temp_dir.path() / "xdg-config-home";
  std::filesystem::create_directories(home);
  std::filesystem::create_directories(xdg_state_home);
  std::filesystem::create_directories(xdg_config_home);
  ScopedEnvVar scoped_home("HOME", home.string());
  ScopedSessionAppHomes scoped_app_homes(xdg_state_home, xdg_config_home);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  // Persist the session so a later restore has a project-session file to load
  // (RestoreDebugState runs as part of RestoreSessionState).
  WorkspaceShellTestAccess::SaveSessionState(shell);

  // Write a stale debug-state file directly: exactly one launch config, but a
  // persisted selected index of 2 (out of range). A plain copy-through of the
  // index would leave the restored state pointing past the end of the list.
  microide::workspace::PersistedDebugState persisted;
  persisted.launch_configs.push_back(microide::workspace::PersistedLaunchConfig{
      .name = "Debug main",
      .type = "debugpy",
      .request = "launch",
      .arguments_json = {},
  });
  persisted.selected_launch_config_index = 2;
  std::vector<std::byte> body;
  Expect(microide::workspace::EncodeDebugStateRecord(persisted, &body),
         "stale debug state should encode");
  const std::filesystem::path debug_path =
      microide::workspace::ProjectStateDirectory(root) / "debug";
  Expect(microide::persistence::PersistedRecordWriter::WriteFile(debug_path, body, 5u),
         "stale debug state file should write to the project state directory");

  WorkspaceShell restored;
  WorkspaceShellTestAccess::SetProjectRoot(restored, root);
  Expect(WorkspaceShellTestAccess::RestoreSessionState(restored),
         "session restore should succeed with a stale debug file present");
  WorkspaceShellTestAccess::OpenLaunchConfigPicker(restored);
  Expect(WorkspaceShellTestAccess::LaunchConfigPickerMatchLabels(restored).size() == 1,
         "the single persisted launch config should restore");
  Expect(WorkspaceShellTestAccess::SelectedLaunchConfigIndex(restored) == 0,
         "an out-of-range persisted selected launch index should clamp to 0 on restore");
}

void TestWorkspaceShellCompareSyntaxTokensAreDeferredUntilRender() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "compare.txt";
  WriteFile(source, "alpha\nbeta\ngamma\n");

  InitializeGitRepo(root);
  CommitAll(root, "base fixture", "base fixture");
  WriteFile(source, "alpha changed\nbeta changed\ngamma changed\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "working-tree comparison should open for deferred syntax-token fixture");

  const auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  Expect(compare.left_tokens_by_row.size() == compare.model.rows.size(),
         "deferred compare syntax should size left token cache to compare rows");
  Expect(compare.right_tokens_by_row.size() == compare.model.rows.size(),
         "deferred compare syntax should size right token cache to compare rows");
  Expect(compare.syntax_rows_tokenized == 0,
         "deferred compare syntax should avoid eager tokenization during tab open");
}

void TestWorkspaceShellReopenFileReloadsCleanEditorTab() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "notes.txt";
  WriteFile(source, "alpha long editor line\nbeta long editor line\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);

  auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  editor.SetViewportSize(1, 8);
  editor.MoveCursorTo(1, 2);
  editor.SetScrollLine(1);
  editor.SetHorizontalScroll(1);

  WriteFile(source, "alpha refreshed editor line\nbeta refreshed editor line\n");
  WorkspaceShellTestAccess::OpenFile(shell, source);

  const auto& reopened = WorkspaceShellTestAccess::ActiveEditor(shell);
  Expect(WorkspaceShellTestAccess::OpenTabs(shell).size() == 1,
         "reopening a clean editor should reuse the existing tab");
  Expect(reopened.lines()[0] == "alpha refreshed editor line",
         "reopening a clean editor should reload the latest disk content");
  Expect(reopened.lines()[1] == "beta refreshed editor line",
         "reopening a clean editor should refresh every line from disk");
  Expect(reopened.cursor_line() == 1 && reopened.cursor_column() == 2,
         "reopening a clean editor should preserve cursor state");
  Expect(!reopened.dirty(),
         "reopening a clean editor should keep the tab clean");
}

void TestWorkspaceShellRefreshReloadsCleanOpenEditorBuffers() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "notes.txt";
  WriteFile(source, "alpha\nbeta\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);

  auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  editor.MoveCursorTo(1, 1);
  editor.SetScrollLine(1);

  WriteFile(source, "alpha refreshed\nbeta refreshed\n");
  Expect(WorkspaceShellTestAccess::ExecuteTreeRefresh(shell),
         "tree refresh should execute");

  const auto& refreshed = WorkspaceShellTestAccess::ActiveEditor(shell);
  Expect(refreshed.lines()[0] == "alpha refreshed",
         "tree refresh should reload clean open editor buffers from disk");
  Expect(refreshed.lines()[1] == "beta refreshed",
         "tree refresh should refresh every line in the clean open buffer");
  Expect(refreshed.cursor_line() == 1 && refreshed.cursor_column() == 1,
         "tree refresh should preserve cursor state while reloading the buffer");
}

void TestWorkspaceShellReopenClearsFoldCollapseState() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "notes.cpp";
  WriteFile(source, "void f() {\n  body();\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);

  auto* initial_model = WorkspaceShellTestAccess::EnsureActiveFoldingModelFresh(shell);
  Expect(initial_model != nullptr, "folding model should be available for editor tabs");
  Expect(!initial_model->ranges().empty(), "folding model should compute a fold range");
  Expect(initial_model->Collapse(0), "test fixture should collapse the top-level fold");
  Expect(initial_model->IsCollapsedAtOpener(0),
         "collapsed state should be present before reopen");

  WriteFile(source, "void f() {\n  refreshed();\n}\n");
  WorkspaceShellTestAccess::OpenFile(shell, source);

  auto* reopened_model = WorkspaceShellTestAccess::EnsureActiveFoldingModelFresh(shell);
  Expect(reopened_model != nullptr, "reopened editor should rebuild the folding model");
  Expect(!reopened_model->ranges().empty(),
         "reopened editor should still expose fold ranges");
  Expect(!reopened_model->IsCollapsedAtOpener(0),
         "reopening a clean editor should clear prior collapsed fold state");
}

void TestWorkspaceShellRefreshClearsFoldCollapseState() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "notes.cpp";
  WriteFile(source, "void f() {\n  body();\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);

  auto* initial_model = WorkspaceShellTestAccess::EnsureActiveFoldingModelFresh(shell);
  Expect(initial_model != nullptr, "folding model should be available for refresh fixture");
  Expect(initial_model->Collapse(0), "test fixture should collapse the top-level fold");
  Expect(initial_model->IsCollapsedAtOpener(0),
         "collapsed state should be present before refresh");

  WriteFile(source, "void f() {\n  refreshed();\n}\n");
  Expect(WorkspaceShellTestAccess::ExecuteTreeRefresh(shell),
         "tree refresh should execute for folding refresh fixture");

  auto* refreshed_model = WorkspaceShellTestAccess::EnsureActiveFoldingModelFresh(shell);
  Expect(refreshed_model != nullptr, "refreshed editor should rebuild the folding model");
  Expect(!refreshed_model->ranges().empty(),
         "refreshed editor should still expose fold ranges");
  Expect(!refreshed_model->IsCollapsedAtOpener(0),
         "tree refresh should clear prior collapsed fold state");
}

void TestWorkspaceShellEditorEditInvalidatesFoldingFingerprint() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "notes.cpp";
  WriteFile(source, "void f() {\n  body();\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);

  auto* model = WorkspaceShellTestAccess::EnsureActiveFoldingModelFresh(shell);
  Expect(model != nullptr, "folding model should be available for edit fixture");
  const auto fingerprint_before = model->fingerprint();
  Expect(model->IsFresh(fingerprint_before),
         "fresh folding model should report fresh before edits");

  auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  editor.MoveCursorTo(1, 2);
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "extra_"),
         "editor should accept text input for folding invalidation fixture");

  Expect(!model->IsFresh(fingerprint_before),
         "editing the viewport should invalidate the prior folding fingerprint");

  auto* refreshed_model = WorkspaceShellTestAccess::EnsureActiveFoldingModelFresh(shell);
  Expect(refreshed_model != nullptr, "folding model should refresh after edit invalidation");
  Expect(refreshed_model->fingerprint().layout_revision != fingerprint_before.layout_revision,
         "post-edit folding fingerprint should pick up the new layout revision");
}

void TestWorkspaceShellBufferSearchRevealsCollapsedMatchAndKeepsItOnClose() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "notes.cpp";
  WriteFile(source, "void f() {\n  target();\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);

  auto* model = WorkspaceShellTestAccess::EnsureActiveFoldingModelFresh(shell);
  Expect(model != nullptr, "buffer-search fold fixture should build a folding model");
  Expect(model->Collapse(0), "buffer-search fold fixture should collapse the top-level fold");
  Expect(model->IsCollapsedAtOpener(0),
         "buffer-search fold fixture should start with the top-level fold collapsed");

  Expect(SendKeyDown(shell, SDLK_F, SDL_KMOD_CTRL),
         "Ctrl+F should open the buffer-search overlay");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "target"),
         "buffer-search fold fixture should accept a query");

  Expect(!model->IsCollapsedAtOpener(0),
         "buffer-search should auto-expand a collapsed fold that hides the selected match");
  const auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  Expect(editor.cursor_line() == 1 && editor.cursor_column() == 2,
         "buffer-search should move the caret onto the revealed match");

  Expect(SendKeyDown(shell, SDLK_ESCAPE, SDL_KMOD_NONE),
         "Escape should close the non-modal find widget");
  Expect(!WorkspaceShellTestAccess::OverlayVisible(shell),
         "Escape should hide the find widget");
  // The non-modal widget already moved the caret onto the match, so closing it
  // keeps the fold expanded — re-collapsing would hide where the caret landed.
  Expect(!model->IsCollapsedAtOpener(0),
         "closing the find widget keeps the revealed fold expanded under the caret");
}

void TestWorkspaceShellBufferSearchKeepsRevealAfterClose() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "notes.cpp";
  WriteFile(source, "void f() {\n  target();\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);

  auto* model = WorkspaceShellTestAccess::EnsureActiveFoldingModelFresh(shell);
  Expect(model != nullptr, "buffer-search activate fixture should build a folding model");
  Expect(model->Collapse(0), "buffer-search activate fixture should collapse the top-level fold");

  Expect(SendKeyDown(shell, SDLK_F, SDL_KMOD_CTRL),
         "Ctrl+F should open the buffer-search overlay for activate fixture");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "target"),
         "buffer-search activate fixture should accept a query");
  Expect(!model->IsCollapsedAtOpener(0),
         "buffer-search activate fixture should auto-expand the collapsed match");

  // Enter now cycles to the next match and keeps the non-modal find widget open
  // (VSCode-style) instead of jumping + dismissing.
  Expect(SendKeyDown(shell, SDLK_RETURN, SDL_KMOD_NONE),
         "Enter should advance the buffer-search match");
  Expect(WorkspaceShellTestAccess::OverlayVisible(shell),
         "Enter must keep the non-modal find widget open");
  Expect(!model->IsCollapsedAtOpener(0),
         "cycling matches should keep the revealed fold expanded");

  // Esc closes the widget and returns focus to the editor; the fold that the
  // search revealed stays expanded.
  Expect(SendKeyDown(shell, SDLK_ESCAPE, SDL_KMOD_NONE),
         "Escape should close the non-modal find widget");
  Expect(!WorkspaceShellTestAccess::OverlayVisible(shell),
         "closing the find widget should hide the overlay");
  Expect(!model->IsCollapsedAtOpener(0),
         "closing the widget should keep the fold the search revealed expanded");
}

void TestWorkspaceShellEnsureActiveFoldingModelFreshBinding() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path plain = root / "plain.txt";
  const std::filesystem::path folded = root / "folded.cpp";
  WriteFile(plain, "hello\n");
  WriteFile(folded, "void f() {\n  body();\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, plain);

  auto& plain_editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  WorkspaceShellTestAccess::EnsureActiveFoldingModelFresh(shell);
  Expect(plain_editor.folding_revision() == 0,
         "plain buffer without fold ranges should detach viewport folding model");

  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, folded),
         "folded editor tab should open");
  auto* folded_model = WorkspaceShellTestAccess::EnsureActiveFoldingModelFresh(shell);
  Expect(folded_model != nullptr, "folded tab should expose a folding model");
  Expect(!folded_model->ranges().empty(), "folded tab should compute fold ranges");
  auto& folded_editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  Expect(folded_editor.folding_revision() != 0,
         "non-empty folding model should bind to the active viewport");

  Expect(folded_model->Collapse(0), "folded tab should accept collapse");
  WorkspaceShellTestAccess::EnsureActiveFoldingModelFresh(shell);
  const int collapsed_visual_rows = folded_editor.VisualRowCount();
  Expect(collapsed_visual_rows < static_cast<int>(folded_editor.line_count()),
         "collapsed fold should reduce visible rows after refresh");

  folded_model->ExpandAll();
  WorkspaceShellTestAccess::EnsureActiveFoldingModelFresh(shell);
  Expect(!folded_model->IsCollapsedAtOpener(0),
         "expand-all should clear collapsed fold state");
  Expect(folded_editor.VisualRowCount() == static_cast<int>(folded_editor.line_count()),
         "expanded folds should restore full visible rows after refresh");
}

void TestWorkspaceShellFoldingSurvivesTabVectorReallocationAndLayout() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "fold_target.cpp";
  WriteFile(source, "void f() {\n  hidden();\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  auto* model = WorkspaceShellTestAccess::EnsureActiveFoldingModelFresh(shell);
  Expect(model != nullptr, "folding integration fixture should build a folding model");
  const void* model_ptr_before = model;
  Expect(model->Collapse(0), "integration fixture should collapse the top-level fold");
  WorkspaceShellTestAccess::EnsureActiveFoldingModelFresh(shell);
  auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  const int collapsed_visual_rows = editor.VisualRowCount();
  Expect(collapsed_visual_rows < static_cast<int>(editor.line_count()),
         "collapsed fold should hide interior rows before opening more tabs");

  for (int i = 0; i < 24; ++i) {
    const std::filesystem::path extra =
        root / ("extra_" + std::to_string(i) + ".txt");
    WriteFile(extra, "line\n");
    Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, extra),
           "extra tabs should open to force open_tabs reallocation");
  }

  WorkspaceShellTestAccess::ActivateTab(shell, 0);
  const auto& tabs = WorkspaceShellTestAccess::OpenTabs(shell);
  Expect(tabs.front().editor_state.has_value(),
         "first editor tab should still exist after opening many tabs");
  Expect(tabs.front().editor_state->folding_model.get() == model_ptr_before,
         "active tab folding model heap address must survive open_tabs reallocation");

  auto* model_after = WorkspaceShellTestAccess::EnsureActiveFoldingModelFresh(shell);
  Expect(model_after == model_ptr_before,
         "EnsureActiveFoldingModelFresh must return the stable folding model pointer");
  Expect(model_after->IsLineHidden(1),
         "collapsed fold visibility must remain valid after tab churn");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).VisualRowCount() == collapsed_visual_rows,
         "layout after tab churn must still honor collapsed fold visibility");
}

void TestWorkspaceShellFoldAllUnfoldAllCtrlKChord() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "notes.cpp";
  WriteFile(source, "void f() {\n  body();\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);

  auto* model = WorkspaceShellTestAccess::EnsureActiveFoldingModelFresh(shell);
  Expect(model != nullptr, "fold-all chord fixture should build a folding model");
  Expect(!model->ranges().empty(),
         "fold-all chord fixture should expose at least one fold range");
  Expect(!model->IsCollapsedAtOpener(0),
         "fold-all chord fixture should start with folds expanded");

  Expect(SendKeyDown(shell, SDLK_K, SDL_KMOD_CTRL),
         "Ctrl+K should arm the editor fold chord sequence");
  Expect(SendKeyDown(shell, SDLK_0, SDL_KMOD_CTRL),
         "Ctrl+K Ctrl+0 should collapse all folds");
  Expect(model->IsCollapsedAtOpener(0),
         "Ctrl+K Ctrl+0 should collapse the opener fold");

  Expect(SendKeyDown(shell, SDLK_K, SDL_KMOD_CTRL),
         "Ctrl+K should arm the editor fold chord sequence");
  Expect(SendKeyDown(shell, SDLK_0, SDL_KMOD_CTRL),
         "Ctrl+K Ctrl+0 should collapse all folds");
  Expect(model->IsCollapsedAtOpener(0),
         "Ctrl+K Ctrl+0 should collapse the opener fold");

  Expect(SendKeyDown(shell, SDLK_K, SDL_KMOD_CTRL),
         "Ctrl+K should arm unfold-all");
  Expect(SendKeyDown(shell, SDLK_J, SDL_KMOD_CTRL),
         "Ctrl+K Ctrl+J should expand folds");
  Expect(!model->IsCollapsedAtOpener(0),
         "Ctrl+K Ctrl+J should expand collapsed folds");

  Expect(SendKeyDown(shell, SDLK_K, SDL_KMOD_CTRL),
         "Ctrl+K should arm keypad fold-all");
  Expect(SendKeyDown(shell, SDLK_KP_0, SDL_KMOD_CTRL),
         "Ctrl+K Ctrl+keypad 0 should collapse all folds");
  Expect(model->IsCollapsedAtOpener(0),
         "Ctrl+keypad 0 should complete the fold-all chord");
}

// Data-integrity (B1/C10): the crash-safety session flush must stage unsaved buffer
// content to the durable store WITHOUT a clean shutdown, so a fresh shell can restore it.
// This is the net for a crash / kill -9 between event-driven session saves.
void TestWorkspaceShellCrashSafetyFlushPersistsUnsavedContent() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "notes.txt";
  WriteFile(source, "alpha\nbeta\n");

  const std::filesystem::path home = temp_dir.path() / "home";
  const std::filesystem::path xdg_state_home = temp_dir.path() / "xdg-state-home";
  const std::filesystem::path xdg_config_home = temp_dir.path() / "xdg-config-home";
  std::filesystem::create_directories(home);
  std::filesystem::create_directories(xdg_state_home);
  std::filesystem::create_directories(xdg_config_home);
  ScopedEnvVar scoped_home("HOME", home.string());
  ScopedSessionAppHomes scoped_app_homes(xdg_state_home, xdg_config_home);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  editor.MoveCursorTo(1, 0);
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "unsaved "),
         "crash-safety flush fixture should accept text input");
  Expect(editor.dirty(), "the buffer should be dirty before the flush");

  // Fire the exact routine the debounced timer runs — NO clean shutdown.
  WorkspaceShellTestAccess::FlushSessionStateForCrashSafety(shell);

  // A fresh shell (as if relaunched after a crash) must recover the unsaved content.
  WorkspaceShell restored;
  WorkspaceShellTestAccess::SetProjectRoot(restored, root);
  Expect(WorkspaceShellTestAccess::RestoreSessionState(restored),
         "restore after a crash-safety flush should succeed");
  WorkspaceShellTestAccess::ActivateTab(restored, 0);
  const auto& reopened = WorkspaceShellTestAccess::ActiveEditor(restored);
  Expect(reopened.lines()[1] == "unsaved beta",
         "the crash-safety flush must persist unsaved buffer content for restore");
  Expect(reopened.dirty(), "restored crash-safety content must stay dirty (still unsaved to disk)");
}

// B1: the crash-safety flush debounce arms once a real edit lands on a dirty buffer.
void TestWorkspaceShellCrashSafetyFlushTimerArmsOnEdit() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "notes.txt";
  WriteFile(source, "alpha\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);

  // First arm call only baselines the active viewport's content revision.
  WorkspaceShellTestAccess::ArmSessionFlushTimer(shell);
  Expect(!WorkspaceShellTestAccess::SessionFlushArmed(shell),
         "the flush timer should not arm before any edit");

  // A real edit bumps the content revision; the next arm detects it and arms.
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("x");
  WorkspaceShellTestAccess::ArmSessionFlushTimer(shell);
  Expect(WorkspaceShellTestAccess::SessionFlushArmed(shell),
         "an edit on a dirty buffer should arm the crash-safety flush debounce");
}

void TestWorkspaceShellRestoreSessionPreservesDirtyEditorBufferContent() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "notes.txt";
  WriteFile(source, "alpha\nbeta\n");

  const std::filesystem::path home = temp_dir.path() / "home";
  const std::filesystem::path xdg_state_home = temp_dir.path() / "xdg-state-home";
  const std::filesystem::path xdg_config_home = temp_dir.path() / "xdg-config-home";
  std::filesystem::create_directories(home);
  std::filesystem::create_directories(xdg_state_home);
  std::filesystem::create_directories(xdg_config_home);
  ScopedEnvVar scoped_home("HOME", home.string());
  ScopedSessionAppHomes scoped_app_homes(xdg_state_home, xdg_config_home);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  editor.MoveCursorTo(1, 0);
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "dirty "),
         "dirty editor-session fixture should accept text input");
  editor.SetScrollLine(1);
  WorkspaceShellTestAccess::SaveSessionState(shell);

  WriteFile(source, "alpha disk replacement\nbeta disk replacement\n");

  WorkspaceShell restored;
  WorkspaceShellTestAccess::SetProjectRoot(restored, root);
  Expect(WorkspaceShellTestAccess::RestoreSessionState(restored),
         "dirty editor-session restore should succeed");
  WorkspaceShellTestAccess::ActivateTab(restored, 0);

  const auto& reopened = WorkspaceShellTestAccess::ActiveEditor(restored);
  Expect(reopened.lines()[1] == "dirty beta",
         "dirty editor-session restore should prefer the unsaved in-memory buffer over disk changes");
  Expect(reopened.dirty(),
         "dirty editor-session restore should preserve the dirty state");
  Expect(reopened.cursor_line() == 1 && reopened.cursor_column() == 6,
         "dirty editor-session restore should preserve the caret location");
  Expect(reopened.scroll_line() == 1,
         "dirty editor-session restore should preserve the scroll position");
}

// Session save must enforce the reader's dirty-buffer budget BEFORE snapshotting:
// an over-budget dirty editor tab persists as a path-only reference (no whole-buffer
// snapshot materialized/written) and restores by reopening from disk.
// TD-2026-07-17A-083.
void TestWorkspaceShellSessionSaveOmitsOverBudgetDirtySnapshot() {
  // Tiny budget so a small dirty edit is "over budget"; restored at scope exit.
  struct BudgetGuard {
    BudgetGuard() { PersistenceCoordinator::SetMaxDirtySnapshotBytesForTesting(8); }
    ~BudgetGuard() { PersistenceCoordinator::SetMaxDirtySnapshotBytesForTesting(0); }
  } budget_guard;

  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "notes.txt";
  WriteFile(source, "alpha\nbeta\n");

  const std::filesystem::path home = temp_dir.path() / "home";
  const std::filesystem::path xdg_state_home = temp_dir.path() / "xdg-state-home";
  const std::filesystem::path xdg_config_home = temp_dir.path() / "xdg-config-home";
  std::filesystem::create_directories(home);
  std::filesystem::create_directories(xdg_state_home);
  std::filesystem::create_directories(xdg_config_home);
  ScopedEnvVar scoped_home("HOME", home.string());
  ScopedSessionAppHomes scoped_app_homes(xdg_state_home, xdg_config_home);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  editor.MoveCursorTo(1, 0);
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "unsaved bigger than budget "),
         "fixture should accept text input (making the buffer dirty and over budget)");
  Expect(editor.dirty(), "the buffer is dirty before save");
  WorkspaceShellTestAccess::SaveSessionState(shell);

  // Change the file on disk so we can tell a path-only restore (disk content) from
  // a snapshot restore (the unsaved in-memory content).
  WriteFile(source, "DISK-ALPHA\nDISK-BETA\n");

  WorkspaceShell restored;
  WorkspaceShellTestAccess::SetProjectRoot(restored, root);
  Expect(WorkspaceShellTestAccess::RestoreSessionState(restored),
         "session restore should succeed");
  // Mirror the real startup path: this hydrates the active tab's deferred viewport.
  WorkspaceShellTestAccess::ActivateCurrentTabAfterStateLoad(restored);
  const auto& reopened = WorkspaceShellTestAccess::ActiveEditor(restored);
  Expect(reopened.lines()[0] == "DISK-ALPHA",
         "an over-budget dirty tab restores from disk (the snapshot was omitted)");
  Expect(!reopened.dirty(),
         "an over-budget dirty tab restores clean (path-only, no persisted snapshot)");
}

// Session restore caps the number of tabs it rebuilds per group at a product limit
// (far below the 4096 decode ceiling), so a huge saved session can't make startup
// path-probe / model-build thousands of tabs before the first frame. The active tab
// is always restored. TD-2026-07-17A-082.
void TestWorkspaceShellRestoreSessionCapsTabCount() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::size_t tab_count = 205;  // > the 200 per-group restore cap
  std::vector<std::filesystem::path> files;
  for (std::size_t i = 0; i < tab_count; ++i) {
    const std::filesystem::path path = root / ("f" + std::to_string(i) + ".txt");
    WriteFile(path, "x\n");
    files.push_back(path);
  }

  const std::filesystem::path home = temp_dir.path() / "home";
  const std::filesystem::path xdg_state_home = temp_dir.path() / "xdg-state-home";
  const std::filesystem::path xdg_config_home = temp_dir.path() / "xdg-config-home";
  std::filesystem::create_directories(home);
  std::filesystem::create_directories(xdg_state_home);
  std::filesystem::create_directories(xdg_config_home);
  ScopedEnvVar scoped_home("HOME", home.string());
  ScopedSessionAppHomes scoped_app_homes(xdg_state_home, xdg_config_home);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, files.front());
  for (std::size_t i = 1; i < files.size(); ++i) {
    Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, files[i]),
           "tab-cap fixture should open each tab");
  }
  WorkspaceShellTestAccess::SaveSessionState(shell);

  WorkspaceShell restored;
  WorkspaceShellTestAccess::SetProjectRoot(restored, root);
  Expect(WorkspaceShellTestAccess::RestoreSessionState(restored),
         "restore should succeed for an oversized session");
  const std::size_t restored_tabs = WorkspaceShellTestAccess::OpenTabs(restored).size();
  Expect(restored_tabs <= 201,
         "restore rebuilds at most the per-group cap (+ the active tab)");
  Expect(restored_tabs < tab_count,
         "an oversized session restores fewer tabs than were saved (cap engaged)");
}

// A file scrolled (without moving the caret) must reopen at the SAME scroll line.
// The 2-space-indented body makes indent-detection change the tab size on open,
// which runs EnsureCursorVisible -- the operation that used to snap the restored
// scroll back onto the line-0 caret.
void TestWorkspaceShellRestoreSessionPreservesIndependentScrollPosition() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "deep.py";
  std::string content;
  for (int i = 0; i < 200; ++i) {
    content += "  line\n";  // consistent 2-space indentation
  }
  WriteFile(source, content);

  const std::filesystem::path home = temp_dir.path() / "home";
  const std::filesystem::path xdg_state_home = temp_dir.path() / "xdg-state-home";
  const std::filesystem::path xdg_config_home = temp_dir.path() / "xdg-config-home";
  std::filesystem::create_directories(home);
  std::filesystem::create_directories(xdg_state_home);
  std::filesystem::create_directories(xdg_config_home);
  ScopedEnvVar scoped_home("HOME", home.string());
  ScopedSessionAppHomes scoped_app_homes(xdg_state_home, xdg_config_home);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  // Caret stays at line 0; only the viewport scrolls (scroll-wheel behaviour).
  editor.SetScrollLine(40);
  Expect(editor.cursor_line() == 0,
         "scroll-independence fixture should leave the caret at line 0");
  WorkspaceShellTestAccess::SaveSessionState(shell);

  WorkspaceShell restored;
  WorkspaceShellTestAccess::SetProjectRoot(restored, root);
  Expect(WorkspaceShellTestAccess::RestoreSessionState(restored),
         "scroll-independence restore should succeed");
  // Mirror the real startup path: this hydrates the active tab's deferred viewport.
  WorkspaceShellTestAccess::ActivateCurrentTabAfterStateLoad(restored);

  const auto& reopened = WorkspaceShellTestAccess::ActiveEditor(restored);
  Expect(reopened.cursor_line() == 0,
         "restore should preserve the line-0 caret");
  Expect(reopened.scroll_line() == 40,
         "restore should preserve the scroll position independent of the caret");
}

// Regression: activating a deferred (non-active, never-hydrated) editor tab whose
// file was deleted out from under the IDE must NOT wipe the tab's path/title by
// falling through to the empty group welcome surface. Losing the path stranded
// the tab as "Welcome" in the strip and broke the open-file dedup (a re-open then
// spawned a duplicate tab).
void TestWorkspaceShellActivatingDeletedDeferredTabPreservesIdentity() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file_a = root / "alpha.txt";
  const std::filesystem::path file_b = root / "beta.txt";
  WriteFile(file_a, "alpha\n");
  WriteFile(file_b, "beta\n");

  const std::filesystem::path home = temp_dir.path() / "home";
  const std::filesystem::path xdg_state_home = temp_dir.path() / "xdg-state-home";
  const std::filesystem::path xdg_config_home = temp_dir.path() / "xdg-config-home";
  std::filesystem::create_directories(home);
  std::filesystem::create_directories(xdg_state_home);
  std::filesystem::create_directories(xdg_config_home);
  ScopedEnvVar scoped_home("HOME", home.string());
  ScopedSessionAppHomes scoped_app_homes(xdg_state_home, xdg_config_home);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, file_a);
  WorkspaceShellTestAccess::OpenFileInNewTab(shell, file_b);
  // Make alpha the active tab so beta restores as a deferred, never-hydrated tab
  // (only the active tab is eager-hydrated on restore). Resolve alpha's index so
  // the test is robust to any leading welcome tab.
  {
    const auto& tabs = WorkspaceShellTestAccess::OpenTabs(shell);
    std::size_t alpha_index = tabs.size();
    for (std::size_t i = 0; i < tabs.size(); ++i) {
      if (tabs[i].path == file_a.lexically_normal()) alpha_index = i;
    }
    Expect(alpha_index < tabs.size(), "alpha tab should exist before save");
    WorkspaceShellTestAccess::ActivateTab(shell, alpha_index);
  }
  WorkspaceShellTestAccess::SaveSessionState(shell);

  WorkspaceShell restored;
  WorkspaceShellTestAccess::SetProjectRoot(restored, root);
  Expect(WorkspaceShellTestAccess::RestoreSessionState(restored),
         "two-tab session restore should succeed");
  WorkspaceShellTestAccess::ActivateCurrentTabAfterStateLoad(restored);

  std::size_t beta_index = 0;
  {
    const auto& tabs = WorkspaceShellTestAccess::OpenTabs(restored);
    beta_index = tabs.size();
    for (std::size_t i = 0; i < tabs.size(); ++i) {
      if (tabs[i].path == file_b.lexically_normal()) beta_index = i;
    }
    Expect(beta_index < tabs.size(), "beta tab should be restored (deferred)");
  }

  // Delete beta.txt before it is ever activated, then activate its tab.
  std::filesystem::remove(file_b);
  WorkspaceShellTestAccess::ActivateTab(restored, beta_index);

  const auto& tabs = WorkspaceShellTestAccess::OpenTabs(restored);
  Expect(beta_index < tabs.size() && tabs[beta_index].path == file_b.lexically_normal(),
         "a deleted deferred tab must keep its real path, not the empty welcome path");
  Expect(tabs[beta_index].title == "beta.txt",
         "a deleted deferred tab must keep its filename title, not 'Welcome'");
}

// Expanded folders + the selected node survive a session round-trip.
void TestWorkspaceShellRestoreSessionPreservesTreeExpansion() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path leaf = root / "dir_a" / "sub" / "leaf.txt";
  WriteFile(root / "README.md", "root\n");
  WriteFile(leaf, "leaf\n");
  WriteFile(root / "dir_b" / "other.txt", "other\n");

  const std::filesystem::path home = temp_dir.path() / "home";
  const std::filesystem::path xdg_state_home = temp_dir.path() / "xdg-state-home";
  const std::filesystem::path xdg_config_home = temp_dir.path() / "xdg-config-home";
  std::filesystem::create_directories(home);
  std::filesystem::create_directories(xdg_state_home);
  std::filesystem::create_directories(xdg_config_home);
  ScopedEnvVar scoped_home("HOME", home.string());
  ScopedSessionAppHomes scoped_app_homes(xdg_state_home, xdg_config_home);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  // Selecting the leaf expands every ancestor directory (dir_a, dir_a/sub).
  Expect(WorkspaceShellTestAccess::SelectTreePath(shell, leaf),
         "tree fixture should be able to select the nested leaf");
  WorkspaceShellTestAccess::SaveSessionState(shell);

  WorkspaceShell restored;
  WorkspaceShellTestAccess::SetProjectRoot(restored, root);
  Expect(WorkspaceShellTestAccess::RestoreSessionState(restored),
         "tree-expansion restore should succeed");

  const auto& entries = WorkspaceShellTestAccess::TreeEntries(restored);
  const auto expanded_dir = [&](const std::filesystem::path& path) {
    const auto normalized = path.lexically_normal();
    for (const auto& entry : entries) {
      if (entry.path.lexically_normal() == normalized) {
        return entry.is_directory && entry.expanded;
      }
    }
    return false;
  };
  const auto leaf_visible = [&]() {
    const auto normalized = leaf.lexically_normal();
    for (const auto& entry : entries) {
      if (entry.path.lexically_normal() == normalized) {
        return true;
      }
    }
    return false;
  };
  Expect(expanded_dir(root / "dir_a"),
         "restore should re-expand dir_a");
  Expect(expanded_dir(root / "dir_a" / "sub"),
         "restore should re-expand dir_a/sub");
  Expect(leaf_visible(),
         "restore should reveal the nested leaf under the restored expansion");
  Expect(WorkspaceShellTestAccess::SelectedTreePath(restored).lexically_normal() ==
             leaf.lexically_normal(),
         "restore should re-select the saved tree node");
}

void TestWorkspaceShellRestoreSessionPreservesDirtyUntitledBufferContent() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "README.md", "root\n");

  const std::filesystem::path home = temp_dir.path() / "home";
  const std::filesystem::path xdg_state_home = temp_dir.path() / "xdg-state-home";
  const std::filesystem::path xdg_config_home = temp_dir.path() / "xdg-config-home";
  std::filesystem::create_directories(home);
  std::filesystem::create_directories(xdg_state_home);
  std::filesystem::create_directories(xdg_config_home);
  ScopedEnvVar scoped_home("HOME", home.string());
  ScopedSessionAppHomes scoped_app_homes(xdg_state_home, xdg_config_home);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "tab"),
         "untitled session fixture should open a new untitled tab");
  auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "scratch buffer"),
         "untitled session fixture should accept text input");
  editor.SetScrollLine(0);
  WorkspaceShellTestAccess::SaveSessionState(shell);

  WorkspaceShell restored;
  WorkspaceShellTestAccess::SetProjectRoot(restored, root);
  Expect(WorkspaceShellTestAccess::RestoreSessionState(restored),
         "untitled session restore should succeed");
  WorkspaceShellTestAccess::ActivateTab(restored, 0);

  const auto& reopened = WorkspaceShellTestAccess::ActiveEditor(restored);
  Expect(reopened.path().empty(),
         "untitled session restore should preserve the missing file path");
  Expect(reopened.lines().size() == 1 && reopened.lines()[0] == "scratch buffer",
         "untitled session restore should reopen the unsaved untitled buffer contents");
  Expect(reopened.dirty(),
         "untitled session restore should preserve the dirty state");
}

void TestWorkspaceShellQuitShutdownPersistsDirtyEditorBuffers() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "notes.txt";
  WriteFile(source, "alpha\nbeta\n");

  const std::filesystem::path home = temp_dir.path() / "home";
  const std::filesystem::path xdg_state_home = temp_dir.path() / "xdg-state-home";
  const std::filesystem::path xdg_config_home = temp_dir.path() / "xdg-config-home";
  std::filesystem::create_directories(home);
  std::filesystem::create_directories(xdg_state_home);
  std::filesystem::create_directories(xdg_config_home);
  ScopedEnvVar scoped_home("HOME", home.string());
  ScopedSessionAppHomes scoped_app_homes(xdg_state_home, xdg_config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "quit session fixture should open the project");
  WorkspaceShellTestAccess::OpenFile(shell, source);
  auto& file_editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  file_editor.MoveCursorTo(1, 0);
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "dirty "),
         "quit session fixture should dirty the file-backed editor");
  file_editor.SetScrollLine(1);

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "tab"),
         "quit session fixture should open an untitled tab");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "scratch buffer"),
         "quit session fixture should dirty the untitled editor");

  shell.RequestQuit();
  Expect(!WorkspaceShellTestAccess::DirtyPromptVisible(shell),
         "quit should not show the dirty prompt for dirty editors");
  Expect(shell.ConsumeQuitRequested(),
         "quit should signal shutdown immediately");

  shell.Shutdown();

  Expect(ReadFile(source) == "alpha\nbeta\n",
         "quit session persistence should not write dirty file-backed buffers to disk");

  WorkspaceShell restored;
  Expect(WorkspaceShellTestAccess::RestoreWorkspaceSession(restored),
         "workspace session restore after quit should succeed");
  Expect(WorkspaceShellTestAccess::ProjectCount(restored) == 1,
         "workspace session restore after quit should reopen the saved project");
  Expect(WorkspaceShellTestAccess::ProjectRoot(restored) == root.lexically_normal(),
         "workspace session restore after quit should reactivate the original project");

  const auto& tabs = WorkspaceShellTestAccess::OpenTabs(restored);
  Expect(tabs.size() == 2,
         "workspace session restore after quit should reopen both dirty editor tabs");

  std::size_t file_tab_index = tabs.size();
  std::size_t untitled_tab_index = tabs.size();
  for (std::size_t i = 0; i < tabs.size(); ++i) {
    if (tabs[i].path == source.lexically_normal()) {
      file_tab_index = i;
    }
    if (tabs[i].path.empty()) {
      untitled_tab_index = i;
    }
  }

  Expect(file_tab_index < tabs.size(),
         "workspace session restore after quit should reopen the dirty file-backed editor");
  Expect(untitled_tab_index < tabs.size(),
         "workspace session restore after quit should reopen the dirty untitled editor");

  WorkspaceShellTestAccess::ActivateTab(restored, file_tab_index);
  const auto& reopened_file = WorkspaceShellTestAccess::ActiveEditor(restored);
  Expect(reopened_file.lines()[1] == "dirty beta",
         "workspace session restore after quit should preserve the unsaved file-backed buffer");
  Expect(reopened_file.dirty(),
         "workspace session restore after quit should keep the file-backed editor dirty");
  Expect(reopened_file.cursor_line() == 1 && reopened_file.cursor_column() == 6,
         "workspace session restore after quit should preserve the file-backed caret position");
  Expect(reopened_file.scroll_line() == 1,
         "workspace session restore after quit should preserve the file-backed scroll position");

  WorkspaceShellTestAccess::ActivateTab(restored, untitled_tab_index);
  const auto& reopened_untitled = WorkspaceShellTestAccess::ActiveEditor(restored);
  Expect(reopened_untitled.path().empty(),
         "workspace session restore after quit should preserve the untitled editor path");
  Expect(reopened_untitled.lines().size() == 1 &&
             reopened_untitled.lines()[0] == "scratch buffer",
         "workspace session restore after quit should preserve the untitled buffer contents");
  Expect(reopened_untitled.dirty(),
         "workspace session restore after quit should keep the untitled editor dirty");
}

void TestWorkspaceShellReopenWorkingTreeComparisonRefreshesExistingTab() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "compare.txt";
  WriteFile(source, "alpha\nbeta\n");

  InitializeGitRepo(root);
  CommitAll(root, "base fixture", "base fixture");
  WriteFile(source, "alpha working\nbeta\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "working-tree comparison should open");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  compare.selected_row = compare.model.rows.empty()
                             ? 0
                             : std::min<std::size_t>(1, compare.model.rows.size() - 1);
  compare.scroll_row = 1;
  compare.horizontal_scroll = 3;
  const std::size_t expected_selected_row = compare.selected_row;

  WriteFile(source, "alpha refreshed\nbeta\n");
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "reopening the same working-tree comparison should succeed");

  const auto& reopened = WorkspaceShellTestAccess::ActiveCompare(shell);
  const bool saw_refreshed_right_side =
      std::any_of(reopened.model.rows.begin(), reopened.model.rows.end(), [](const auto& row) {
        return row.right_text == "alpha refreshed";
      });
  Expect(WorkspaceShellTestAccess::OpenTabs(shell).size() == 1,
         "reopening a comparison should refresh the existing tab in place");
  Expect(saw_refreshed_right_side,
         "reopening a comparison should rebuild the working-tree side from disk");
  Expect(reopened.selected_row == expected_selected_row,
         "reopening a comparison should preserve the selected row");
  Expect(reopened.scroll_row == 1 && reopened.horizontal_scroll == 3,
         "reopening a comparison should preserve scroll state");
}

void TestWorkspaceShellMergeSyntaxTokensAreDeferredUntilRender() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path base = root / "base.cpp";
  const std::filesystem::path incoming = root / "incoming.cpp";
  const std::filesystem::path current = root / "current.cpp";
  const std::filesystem::path output = root / "result.cpp";
  WriteFile(base, "int main() {\n  return 0;\n}\n");
  WriteFile(incoming, "int main() {\n  return 1;\n}\n");
  WriteFile(current, "int main() {\n  return 2;\n}\n");
  WriteFile(output, "int main() {\n  return 0;\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, current, output),
         "merge editor should open for deferred syntax-token fixture");

  const auto& merge = WorkspaceShellTestAccess::ActiveMerge(shell);
  Expect(merge.incoming_tokens.size() == merge.model.incoming_lines.size(),
         "deferred merge syntax should size incoming token cache to incoming lines");
  Expect(merge.current_tokens.size() == merge.model.current_lines.size(),
         "deferred merge syntax should size current token cache to current lines");
  Expect(merge.incoming_syntax_rows_tokenized == 0,
         "deferred merge syntax should avoid eager incoming tokenization during tab open");
  Expect(merge.current_syntax_rows_tokenized == 0,
         "deferred merge syntax should avoid eager current tokenization during tab open");
}

void TestWorkspaceShellReopenMergeEditorRefreshesCleanTabFromOutput() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path base = root / "base.txt";
  const std::filesystem::path incoming = root / "incoming.txt";
  const std::filesystem::path current = root / "current.txt";
  WriteFile(base, "header\nshared\nfooter\n");
  WriteFile(incoming, "header\nincoming line 1\nincoming line 2\nfooter\n");
  WriteFile(current, "header\ncurrent line 1\ncurrent line 2\nfooter\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, current, current),
         "merge editor should open when the output path is the current file");

  auto& merge = WorkspaceShellTestAccess::ActiveMerge(shell);
  Expect(!merge.result_viewport.dirty(),
         "merge output seeded from disk should start clean");
  Expect(merge.result_viewport.lines()[1] == "current line 1",
         "merge result should load the existing output buffer from disk");
  Expect(!merge.conflicts.empty() && merge.conflicts.front().last_choice == MergeChoice::Current,
         "merge result loaded from disk should infer the matching conflict choice");
  merge.result_viewport.SetViewportSize(1, 8);
  merge.selected_hunk = 0;
  merge.scroll_row = 1;
  merge.horizontal_scroll = 2;
  merge.left_divider_fraction = 0.29f;
  merge.right_divider_fraction = 0.73f;
  merge.result_viewport.SetScrollLine(1);
  merge.result_viewport.SetHorizontalScroll(2);

  WriteFile(current, "header\ncurrent line 1 updated\ncurrent line 2 updated\ncurrent line 3 updated\nfooter\n");
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, current, current),
         "reopening the same merge editor should succeed");

  const auto& reopened = WorkspaceShellTestAccess::ActiveMerge(shell);
  Expect(WorkspaceShellTestAccess::OpenTabs(shell).size() == 1,
         "reopening a clean merge tab should refresh the existing tab in place");
  Expect(!reopened.result_viewport.dirty(),
         "reopening a clean merge tab should keep the result clean");
  Expect(reopened.result_viewport.lines()[1] == "current line 1 updated",
         "reopening a clean merge tab should reload the latest output content");
  Expect(reopened.result_viewport.lines()[3] == "current line 3 updated",
         "reopening a clean merge tab should handle multiline current-side output");
  Expect(!reopened.conflicts.empty() && reopened.conflicts.front().last_choice == MergeChoice::Current,
         "reopened merge output should continue to infer the current-side conflict choice");
  Expect(reopened.selected_hunk == 0,
         "reopening a clean merge tab should preserve the selected conflict");
  Expect(reopened.scroll_row == 1 && reopened.horizontal_scroll == 2,
         "reopening a clean merge tab should preserve scroll state");
  Expect(std::fabs(reopened.left_divider_fraction - 0.29f) < 0.0001f &&
             std::fabs(reopened.right_divider_fraction - 0.73f) < 0.0001f,
         "reopening a clean merge tab should preserve divider positions");
}

void TestWorkspaceShellMergeEditorUsesWorkingTreeConflictMarkers() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path base = root / "base.txt";
  const std::filesystem::path incoming = root / "incoming.txt";
  const std::filesystem::path current = root / "current.txt";
  const std::filesystem::path output = root / "result.txt";
  WriteFile(base, "section 1\nvalue: base 1\ncontext: unchanged1\n");
  WriteFile(incoming, "section 1\nvalue: feature 1\ncontext: unchanged1\n");
  WriteFile(current, "section 1\nvalue: main 1\ncontext: unchanged1\n");
  WriteFile(output,
            "section 1\n"
            "<<<<<<< HEAD\n"
            "value: main 1\n"
            "let's see what happens with many conflicts\n"
            "this is multiline by design\n"
            "=======\n"
            "value: feature 1\n"
            ">>>>>>> feature-branch\n"
            "context: unchanged1\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, current, output),
         "merge editor should open for conflict-marker fixture");

  const auto& merge = WorkspaceShellTestAccess::ActiveMerge(shell);
  Expect(!merge.result_viewport.dirty(),
         "opening from working-tree conflict markers should not dirty the merge tab");
  Expect(merge.result_viewport.lines().size() == 5,
         "working-tree conflict markers should collapse into a markerless result buffer");
  Expect(merge.result_viewport.lines()[1] == "value: main 1",
         "working-tree conflict markers should preserve the current block");
  Expect(merge.result_viewport.lines()[2] == "let's see what happens with many conflicts",
         "working-tree conflict markers should preserve multiline edits in the current block");
  Expect(merge.result_viewport.lines()[3] == "this is multiline by design",
         "working-tree conflict markers should preserve every extra current-side line");
  Expect(!merge.conflicts.empty() && merge.conflicts.front().valid,
         "working-tree conflict markers should keep the conflict span actionable");
  Expect(merge.conflicts.front().last_choice == MergeChoice::Current,
         "working-tree conflict markers with current-side edits should infer the current choice");
  Expect(merge.conflicts.front().start_line == 1 && merge.conflicts.front().end_line == 4,
         "working-tree conflict markers should size the tracked conflict span to the edited block");
}

void TestWorkspaceShellMergeEditorParsesLargeWorkingTreeConflictBlock() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path base = root / "base.txt";
  const std::filesystem::path incoming = root / "incoming.txt";
  const std::filesystem::path current = root / "current.txt";
  const std::filesystem::path output = root / "result.txt";
  WriteFile(base,
            "section 1\nvalue: base 1\ncontext: unchanged1\n\n"
            "section 2\nvalue: base 2\ncontext: unchanged2\n");
  WriteFile(incoming,
            "section 1\nvalue: feature 1\ncontext: unchanged1\n\n"
            "section 2\nvalue: feature 2\ncontext: unchanged2\n");
  WriteFile(current,
            "section 1\nvalue: main 1\ncontext: unchanged1\n\n"
            "section 2\nvalue: main 2\ncontext: unchanged2\n");
  WriteFile(output,
            "section 1\n"
            "<<<<<<< HEAD\n"
            "value: main 1\n"
            "this is multiline by design\n"
            "context: unchanged1\n"
            "\n"
            "section 2\n"
            "value: main 2\n"
            "context: unchanged2\n"
            "=======\n"
            "value: feature 1\n"
            "context: unchanged1\n"
            "\n"
            "section 2\n"
            "value: feature 2\n"
            "context: unchanged2\n"
            ">>>>>>> feature-branch\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, current, output),
         "merge editor should open for large conflict-block fixture");

  const auto& merge = WorkspaceShellTestAccess::ActiveMerge(shell);
  Expect(!merge.result_viewport.dirty(),
         "large working-tree conflict blocks should open clean");
  Expect(merge.result_viewport.lines()[1] == "value: main 1",
         "large working-tree conflict blocks should preserve current-side content");
  Expect(merge.result_viewport.lines()[2] == "this is multiline by design",
         "large working-tree conflict blocks should preserve extra current-side lines");
  Expect(merge.result_viewport.lines()[6] == "value: main 2",
         "large working-tree conflict blocks should continue with later current-side conflicts");
  Expect(merge.result_viewport.lines()[7] == "context: unchanged2",
         "large working-tree conflict blocks should keep later context lines in place");
  Expect(merge.conflicts.size() == 2,
         "large working-tree conflict blocks should still map back to individual merge conflicts");
  Expect(merge.conflicts[0].valid,
         "large working-tree conflict blocks should keep the first conflict actionable");
  Expect(merge.conflicts[0].start_line == 1 && merge.conflicts[0].end_line == 3,
         "large working-tree conflict blocks should expand the first conflict span around inserted lines");
  Expect(merge.conflicts[1].valid,
         "large working-tree conflict blocks should keep later conflicts actionable");
  Expect(merge.conflicts[1].last_choice == MergeChoice::Current,
         "later conflicts in a large block should infer the current-side choice");
}

void TestWorkspaceShellMergeBothOrdersAndBaseToggle() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path base = root / "base.txt";
  const std::filesystem::path incoming = root / "incoming.txt";
  const std::filesystem::path current = root / "current.txt";
  const std::filesystem::path output = root / "result.txt";
  WriteFile(base, "zero\none\ntwo\n");
  WriteFile(incoming, "zero\none incoming\ntwo\n");
  WriteFile(current, "zero\none current\ntwo\n");
  WriteFile(output, "zero\none\ntwo\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, current, output),
         "merge editor should open for both-order fixture");

  auto& merge = WorkspaceShellTestAccess::ActiveMerge(shell);
  merge.selected_hunk = 0;
  WorkspaceShellTestAccess::ApplyMergeChoice(shell, MergeChoice::BothCurrentFirst);
  Expect(merge.result_viewport.lines()[1] == "one current" &&
             merge.result_viewport.lines()[2] == "one incoming",
         "both-current-first should place current text before incoming text");
  WorkspaceShellTestAccess::ResetMergeHunk(shell);
  Expect(!merge.conflicts.empty() && !merge.conflicts.front().resolved,
         "reset hunk should return the conflict to unresolved");
  Expect(!merge.base_pane_visible, "base pane should start hidden");
  WorkspaceShellTestAccess::ToggleMergeBasePane(shell);
  Expect(merge.base_pane_visible, "base pane toggle should expose the ancestor pane");
  WorkspaceShellTestAccess::ToggleMergeBasePane(shell);
  Expect(!merge.base_pane_visible, "base pane toggle should hide the ancestor pane again");
}

void TestWorkspaceShellMergeChoicePreservesManualEditsAroundConflicts() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path base = root / "base.txt";
  const std::filesystem::path incoming = root / "incoming.txt";
  const std::filesystem::path current = root / "current.txt";
  const std::filesystem::path output = root / "result.txt";
  WriteFile(base, "zero\none\ntwo\nthree\nfour\n");
  WriteFile(incoming, "zero\none incoming\ntwo\nthree incoming\nfour\n");
  WriteFile(current, "zero\none current\ntwo\nthree current\nfour\n");
  WriteFile(output, "zero\none\ntwo\nthree\nfour\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, current, output),
         "merge editor should open for manual-edit preservation fixture");

  auto& merge = WorkspaceShellTestAccess::ActiveMerge(shell);
  merge.result_viewport.MoveCursorTo(2, 0);
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "edited "),
         "merge result should accept direct text input");

  merge.selected_hunk = 1;
  WorkspaceShellTestAccess::ApplyMergeChoice(shell, MergeChoice::Incoming);

  const auto& lines = merge.result_viewport.lines().Snapshot();
  Expect(lines[1] == "one",
         "accepting a later conflict should not rebuild earlier untouched conflicts");
  Expect(lines[2] == "edited two",
         "accepting a later conflict should preserve surrounding manual result edits");
  Expect(lines[3] == "three incoming",
         "accepting a conflict should patch only the selected conflict span");
}

void TestWorkspaceShellMergeConflictTrackingShiftsAfterInsertion() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path base = root / "base.txt";
  const std::filesystem::path incoming = root / "incoming.txt";
  const std::filesystem::path current = root / "current.txt";
  const std::filesystem::path output = root / "result.txt";
  WriteFile(base, "zero\none\ntwo\nthree\nfour\n");
  WriteFile(incoming, "zero\none incoming\ntwo\nthree incoming\nfour\n");
  WriteFile(current, "zero\none current\ntwo\nthree current\nfour\n");
  WriteFile(output, "zero\none\ntwo\nthree\nfour\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, current, output),
         "merge editor should open for conflict-tracking fixture");

  auto& merge = WorkspaceShellTestAccess::ActiveMerge(shell);
  merge.result_viewport.MoveCursorTo(2, 0);
  Expect(SendKeyDown(shell, SDLK_RETURN, SDL_KMOD_NONE),
         "merge result should support inserting lines");

  merge.selected_hunk = 1;
  WorkspaceShellTestAccess::ApplyMergeChoice(shell, MergeChoice::Current);

  const auto& lines = merge.result_viewport.lines().Snapshot();
  Expect(lines[2].empty(),
         "manual result insertions should remain in place after tracked-span updates");
  Expect(lines[4] == "three current",
         "later conflict accepts should follow the shifted tracked span");
}

// Opening a file whose conflict markers are still UNTOUCHED (each side equals the
// model) must NOT silently collapse the conflict to base and mark it resolved. The
// conflict must remain valid and unresolved so the user still has to pick a side.
void TestWorkspaceShellRawConflictMarkersOpenUnresolved() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path base = root / "base.txt";
  const std::filesystem::path incoming = root / "incoming.txt";
  const std::filesystem::path current = root / "current.txt";
  const std::filesystem::path output = root / "result.txt";
  WriteFile(base, "zero\none\ntwo\n");
  WriteFile(incoming, "zero\none incoming\ntwo\n");
  WriteFile(current, "zero\none current\ntwo\n");
  // Raw, unedited conflict markers whose sides exactly equal the model.
  WriteFile(output,
            "zero\n"
            "<<<<<<< HEAD\n"
            "one current\n"
            "=======\n"
            "one incoming\n"
            ">>>>>>> feature\n"
            "two\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, current, output),
         "merge editor should open for raw-marker fixture");

  const auto& merge = WorkspaceShellTestAccess::ActiveMerge(shell);
  Expect(merge.conflicts.size() == 1, "the untouched marker block maps to one conflict");
  Expect(merge.conflicts.front().valid,
         "an untouched raw conflict must remain actionable");
  Expect(!merge.conflicts.front().resolved,
         "an untouched raw conflict must NOT be auto-marked resolved (data-loss regression)");
  Expect(!merge.marked_resolved, "the file must not be auto-marked resolved on open");
  const bool has_remaining = merge.conflicts.front().valid && !merge.conflicts.front().resolved;
  Expect(has_remaining,
         "the untouched conflict must count as remaining, not silently resolved to base");
}

// Inserting at column 0 of a conflict's first result line inserts BEFORE the
// conflict, so its tracking must shift down, not be invalidated (the pure-insertion
// branch must use >= like the general edit branch).
void TestWorkspaceShellInsertionAtConflictStartKeepsTracking() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path base = root / "base.txt";
  const std::filesystem::path incoming = root / "incoming.txt";
  const std::filesystem::path current = root / "current.txt";
  const std::filesystem::path output = root / "result.txt";
  WriteFile(base, "zero\none\ntwo\n");
  WriteFile(incoming, "zero\none incoming\ntwo\n");
  WriteFile(current, "zero\none current\ntwo\n");
  WriteFile(output, "zero\none\ntwo\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, current, output),
         "merge editor should open for insertion-at-start fixture");

  auto& merge = WorkspaceShellTestAccess::ActiveMerge(shell);
  Expect(merge.conflicts.size() == 1 && merge.conflicts.front().start_line == 1,
         "the conflict starts on the second result line");

  // Caret at column 0 of the conflict's first line, then insert a new line before it.
  merge.result_viewport.MoveCursorTo(1, 0);
  Expect(SendKeyDown(shell, SDLK_RETURN, SDL_KMOD_NONE), "merge result should accept a line insert");

  Expect(merge.conflicts.size() == 1 && merge.conflicts.front().valid,
         "inserting before the conflict must not invalidate its tracking");
  Expect(merge.conflicts.front().start_line == 2,
         "the conflict span should shift down by the inserted line");
}

void TestWorkspaceShellMergeHoverPreviewDoesNotCommitState() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path base = root / "base.txt";
  const std::filesystem::path incoming = root / "incoming.txt";
  const std::filesystem::path current = root / "current.txt";
  const std::filesystem::path output = root / "result.txt";
  WriteFile(base,
            "alpha long content 0123456789 abcdefghijklmnopqrstuvwxyz alpha long content "
            "0123456789 abcdefghijklmnopqrstuvwxyz\none\ntwo\nthree\nfour\nfive\nsix\n");
  WriteFile(incoming,
            "incoming long content 0123456789 abcdefghijklmnopqrstuvwxyz incoming long content "
            "0123456789 abcdefghijklmnopqrstuvwxyz\none\ntwo\nthree\nfour\nfive\nsix\n");
  WriteFile(current,
            "current long content 0123456789 abcdefghijklmnopqrstuvwxyz current long content "
            "0123456789 abcdefghijklmnopqrstuvwxyz\none\ntwo\nthree\nfour\nfive\nsix\n");
  WriteFile(output,
            "alpha long content 0123456789 abcdefghijklmnopqrstuvwxyz alpha long content "
            "0123456789 abcdefghijklmnopqrstuvwxyz\none\ntwo\nthree\nfour\nfive\nsix\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1200, 900);
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, current, output),
         "merge editor should open for hover-preview fixture");

  auto& merge = WorkspaceShellTestAccess::ActiveMerge(shell);
  const auto before_lines = merge.result_viewport.lines().Snapshot();
  const bool before_dirty = merge.result_viewport.dirty();
  const auto surface = WorkspaceShellTestAccess::ActiveMergeSurfaceLayout(shell);
  const auto& conflict = merge.conflicts.front();
  const float x = surface.left_x + surface.gutter_width + 12.0f;
  const float y =
      surface.rows_y +
      (static_cast<float>(conflict.incoming_start_line) - static_cast<float>(merge.scroll_row) + 0.5f) *
          surface.line_height;

  Expect(SendMouseMotion(shell, x, y, 0),
         "merge hover fixture should register pointer motion");

  const auto& hover = WorkspaceShellTestAccess::ActiveMergeHoverState(shell);
  Expect(hover.has_value(), "merge hover fixture should produce a hover state");
  Expect(WorkspaceShellTestAccess::ActiveMergeHoverIsIncomingConflict(shell),
         "hovering incoming content should preview the incoming choice");
  Expect(WorkspaceShellTestAccess::ActiveMergeHoverPreviewChoice(shell) == MergeChoice::Incoming,
         "incoming hover preview should advertise the incoming merge choice");
  Expect(merge.result_viewport.lines().Snapshot() == before_lines,
         "hover preview should not mutate the committed result buffer");
  Expect(merge.result_viewport.dirty() == before_dirty,
         "hover preview should not dirty the merge result");
}

void OpenMergeHoverFixture(WorkspaceShell& shell,
                           TemporaryDirectory& temp_dir,
                           std::filesystem::path* base_path,
                           std::filesystem::path* incoming_path,
                           std::filesystem::path* current_path,
                           std::filesystem::path* output_path) {
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path base = root / "base.txt";
  const std::filesystem::path incoming = root / "incoming.txt";
  const std::filesystem::path current = root / "current.txt";
  const std::filesystem::path output = root / "result.txt";
  WriteFile(base, "top\nbase change\nbottom\n");
  WriteFile(incoming, "top\nincoming change\nbottom\n");
  WriteFile(current, "top\ncurrent change\nbottom\n");
  WriteFile(output, "top\nbase change\nbottom\n");

  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 900);
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, current, output),
         "merge hover fixture should open a merge editor");

  if (base_path != nullptr) {
    *base_path = base;
  }
  if (incoming_path != nullptr) {
    *incoming_path = incoming;
  }
  if (current_path != nullptr) {
    *current_path = current;
  }
  if (output_path != nullptr) {
    *output_path = output;
  }
}

void OpenTallMergeMouseFixture(WorkspaceShell& shell, TemporaryDirectory& temp_dir) {
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path base = root / "base.txt";
  const std::filesystem::path incoming = root / "incoming.txt";
  const std::filesystem::path current = root / "current.txt";
  const std::filesystem::path output = root / "result.txt";

  std::string base_text;
  std::string incoming_text;
  std::string current_text;
  for (int i = 0; i < 120; ++i) {
    const std::string line = "line " + std::to_string(i) + "\n";
    base_text += line;
    incoming_text += i == 60 ? "incoming change\n" : line;
    current_text += i == 60 ? "current change\n" : line;
  }

  WriteFile(base, base_text);
  WriteFile(incoming, incoming_text);
  WriteFile(current, current_text);
  WriteFile(output, base_text);

  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 360);
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, current, output),
         "tall merge mouse fixture should open a merge editor");
}

void TestWorkspaceShellMergeHoverPrefersIncomingAcceptButton() {
  TemporaryDirectory temp_dir;
  WorkspaceShell shell;
  OpenMergeHoverFixture(shell, temp_dir, nullptr, nullptr, nullptr, nullptr);

  auto& merge = WorkspaceShellTestAccess::ActiveMerge(shell);
  Expect(!merge.conflicts.empty(), "incoming-accept hover fixture should expose a merge conflict");

  const SDL_FRect accept_rect = WorkspaceShellTestAccess::MergeSourceAcceptRect(shell, 0, true);
  const float hover_x = accept_rect.x + accept_rect.w * 0.5f;
  const float hover_y = accept_rect.y + accept_rect.h * 0.5f;

  Expect(SendMouseMotion(shell, hover_x, hover_y, 0),
         "hovering the incoming accept button should request a redraw");

  const auto& hover = WorkspaceShellTestAccess::ActiveMergeHoverState(shell);
  Expect(hover.has_value(), "incoming accept button hover should produce a hover state");
  Expect(hover->kind == microide::workspace::MergeHoverState::Kind::IncomingAccept,
         "incoming accept button hover should take precedence over the source conflict hover");
  Expect(hover->preview_choice == MergeChoice::Incoming,
         "incoming accept button hover should advertise the incoming merge choice");
}

void TestWorkspaceShellMergeHoverPrefersResultActionButton() {
  TemporaryDirectory temp_dir;
  WorkspaceShell shell;
  OpenMergeHoverFixture(shell, temp_dir, nullptr, nullptr, nullptr, nullptr);

  auto& merge = WorkspaceShellTestAccess::ActiveMerge(shell);
  Expect(!merge.conflicts.empty(), "result-action hover fixture should expose a merge conflict");

  const std::array<SDL_FRect, 4> action_rects =
      WorkspaceShellTestAccess::MergeResultActionRects(shell, 0);
  const float hover_x = action_rects[3].x + action_rects[3].w * 0.5f;
  const float hover_y = action_rects[3].y + action_rects[3].h * 0.5f;

  Expect(SendMouseMotion(shell, hover_x, hover_y, 0),
         "hovering a merge result action button should request a redraw");

  const auto& hover = WorkspaceShellTestAccess::ActiveMergeHoverState(shell);
  Expect(hover.has_value(), "result action button hover should produce a hover state");
  Expect(hover->kind == microide::workspace::MergeHoverState::Kind::ResultAction,
         "result action button hover should take precedence over generic result conflict hover");
  Expect(hover->preview_choice == MergeChoice::Both,
         "result action button hover should advertise the hovered merge choice");
}

void TestWorkspaceShellMergeResultDragSelectionTracksPointer() {
  TemporaryDirectory temp_dir;
  WorkspaceShell shell;
  OpenMergeHoverFixture(shell, temp_dir, nullptr, nullptr, nullptr, nullptr);

  const auto interaction = WorkspaceShellTestAccess::ActiveMergeInteractionLayout(shell);
  const float y = interaction.result.text.first_line_y +
                  interaction.result.text.line_height * 0.5f;
  const float start_x = interaction.result.text.text_x +
                        interaction.result.text.char_width * 0.1f;
  const float end_x = interaction.result.text.text_x +
                      interaction.result.text.char_width * 3.1f;

  Expect(SendMouseDown(shell, start_x, y, SDL_BUTTON_LEFT),
         "pressing inside the merge result pane should start selection");
  Expect(SendMouseMotion(shell, end_x, y, SDL_BUTTON_LMASK),
         "dragging inside the merge result pane should update selection");
  Expect(SendMouseUp(shell, end_x, y, SDL_BUTTON_LEFT),
         "releasing after a merge result drag should be handled");
  Expect(WorkspaceShellTestAccess::ActiveMergeHasSelection(shell),
         "dragging across merge result text should create a selection");
  Expect(WorkspaceShellTestAccess::ActiveMergeSelectedText(shell) == "top",
         "merge result drag selection should capture the dragged text");
}

void TestWorkspaceShellMergeDividerDragUpdatesPaneFractions() {
  TemporaryDirectory temp_dir;
  WorkspaceShell shell;
  OpenMergeHoverFixture(shell, temp_dir, nullptr, nullptr, nullptr, nullptr);

  auto& merge = WorkspaceShellTestAccess::ActiveMerge(shell);
  const float before_fraction = merge.left_divider_fraction;
  const auto divider_rects = MergeDividerRectsOf(shell);
  const float start_x = divider_rects[0].x + divider_rects[0].w * 0.5f;
  const float y = divider_rects[0].y + divider_rects[0].h * 0.5f;

  Expect(SendMouseDown(shell, start_x, y, SDL_BUTTON_LEFT),
         "pressing the merge divider should be handled");
  Expect(SendMouseMotion(shell, start_x + 80.0f, y,
                                                     SDL_BUTTON_LMASK),
         "dragging the merge divider should be handled");
  Expect(SendMouseUp(shell, start_x + 80.0f, y,
                                                       SDL_BUTTON_LEFT),
         "releasing the merge divider drag should be handled");
  Expect(merge.left_divider_fraction > before_fraction,
         "dragging the merge divider should update the left pane fraction");
}

void TestWorkspaceShellMergeWheelScrollsRows() {
  TemporaryDirectory temp_dir;
  WorkspaceShell shell;
  OpenTallMergeMouseFixture(shell, temp_dir);

  auto& merge = WorkspaceShellTestAccess::ActiveMerge(shell);
  const auto surface = WorkspaceShellTestAccess::ActiveMergeSurfaceLayout(shell);
  Expect(merge.result_viewport.lines().size() > static_cast<std::size_t>(surface.visible_rows),
         "merge wheel fixture should overflow the viewport");

  const int before_scroll = merge.scroll_row;
  const SDL_FRect result_rect = WorkspaceShellTestAccess::ActiveMergeResultRect(shell);
  const float wheel_x = result_rect.x + 24.0f;
  const float wheel_y = result_rect.y + 24.0f;
  // Same contract as the compare surface and the other five scrollable surfaces:
  // the wheel scrolls, it does not focus. See CompareWheelScrollsRows.
  WorkspaceShellTestAccess::SetFocusPanel(shell);
  Expect(SendMouseWheel(shell, wheel_x, wheel_y, -1),
         "scrolling the merge result pane should be handled");
  Expect(merge.scroll_row > before_scroll,
         "scrolling the merge result pane should advance the visible row");
  Expect(WorkspaceShellTestAccess::FocusIsPanel(shell),
         "scrolling the merge result pane should leave keyboard focus where it was");
}

void TestWorkspaceShellMergeToolbarButtonsNavigateConflicts() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path base = root / "base.txt";
  const std::filesystem::path incoming = root / "incoming.txt";
  const std::filesystem::path current = root / "current.txt";
  WriteFile(base, "top\nbase a\nmid\nbase b\nbottom\n");
  WriteFile(incoming, "top\nincoming a\nmid\nincoming b\nbottom\n");
  WriteFile(current, "top\ncurrent a\nmid\ncurrent b\nbottom\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, current, current),
         "merge editor should open for toolbar navigation fixture");

  auto& merge = WorkspaceShellTestAccess::ActiveMerge(shell);
  Expect(merge.conflicts.size() == 2,
         "toolbar navigation fixture should expose two merge conflicts");
  Expect(merge.selected_hunk == 0,
         "toolbar navigation fixture should start on the first conflict");

  const auto nav_rects = WorkspaceShellTestAccess::MergeToolbarNavigationRects(shell);
  const float next_x = nav_rects[1].x + nav_rects[1].w * 0.5f;
  const float next_y = nav_rects[1].y + nav_rects[1].h * 0.5f;
  Expect(SendMouseDown(shell, next_x, next_y, SDL_BUTTON_LEFT),
         "clicking the merge next button should be handled");
  Expect(merge.selected_hunk == 1,
         "clicking the merge next button should select the next conflict");

  const float prev_x = nav_rects[0].x + nav_rects[0].w * 0.5f;
  const float prev_y = nav_rects[0].y + nav_rects[0].h * 0.5f;
  Expect(SendMouseDown(shell, prev_x, prev_y, SDL_BUTTON_LEFT),
         "clicking the merge prev button should be handled");
  Expect(merge.selected_hunk == 0,
         "clicking the merge prev button should select the previous conflict");
}

void TestWorkspaceShellRestoreSessionPreservesMergeNavigationState() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path base = root / "base.txt";
  const std::filesystem::path incoming = root / "incoming.txt";
  const std::filesystem::path current = root / "current.txt";
  const std::filesystem::path output = root / "result.txt";
  WriteFile(base, "alpha\nshared\n");
  WriteFile(incoming, "incoming\nshared\n");
  WriteFile(current, "current\nshared\n");
  WriteFile(output, "alpha\nshared\n");

  const std::filesystem::path home = temp_dir.path() / "home";
  const std::filesystem::path xdg_state_home = temp_dir.path() / "xdg-state-home";
  const std::filesystem::path xdg_config_home = temp_dir.path() / "xdg-config-home";
  std::filesystem::create_directories(home);
  std::filesystem::create_directories(xdg_state_home);
  std::filesystem::create_directories(xdg_config_home);
  ScopedEnvVar scoped_home("HOME", home.string());
  ScopedSessionAppHomes scoped_app_homes(xdg_state_home, xdg_config_home);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, current, output),
         "merge editor should open for session-restore fixture");

  auto& merge = WorkspaceShellTestAccess::ActiveMerge(shell);
  Expect(!merge.model.hunks.empty() && merge.model.hunks.front().conflict,
         "merge session fixture should start with a conflict hunk");
  merge.model.hunks.front().choice = MergeChoice::Current;
  WorkspaceShellTestAccess::RefreshMergeTabDerivedState(shell);
  merge.selected_hunk = 0;
  merge.scroll_row = 2;
  merge.horizontal_scroll = 5;
  merge.left_divider_fraction = 0.24f;
  merge.right_divider_fraction = 0.74f;
  merge.result_viewport.SetScrollLine(2);
  WorkspaceShellTestAccess::SaveSessionState(shell);

  WorkspaceShell restored;
  WorkspaceShellTestAccess::SetProjectRoot(restored, root);
  Expect(WorkspaceShellTestAccess::RestoreSessionState(restored),
         "merge session restore should succeed");
  Expect(WorkspaceShellTestAccess::OpenTabs(restored).size() == 1,
         "merge session restore should reopen the merge tab");

  const auto& rebuilt = WorkspaceShellTestAccess::ActiveMerge(restored);
  Expect(rebuilt.base_path == base.lexically_normal(),
         "merge session restore should preserve the base path");
  Expect(rebuilt.incoming_path == incoming.lexically_normal(),
         "merge session restore should preserve the incoming path");
  Expect(rebuilt.current_path == current.lexically_normal(),
         "merge session restore should preserve the current path");
  Expect(rebuilt.output_path == output.lexically_normal(),
         "merge session restore should preserve the output path");
  Expect(rebuilt.selected_hunk == 0,
         "merge session restore should preserve the selected conflict");
  Expect(rebuilt.scroll_row == 2,
         "merge session restore should preserve vertical scroll");
  Expect(rebuilt.horizontal_scroll == 5,
         "merge session restore should preserve horizontal scroll");
  Expect(std::fabs(rebuilt.left_divider_fraction - 0.24f) < 0.0001f,
         "merge session restore should preserve the left divider fraction");
  Expect(std::fabs(rebuilt.right_divider_fraction - 0.74f) < 0.0001f,
         "merge session restore should preserve the right divider fraction");
  Expect(!rebuilt.model.hunks.empty() && rebuilt.model.hunks.front().choice == MergeChoice::Current,
         "merge session restore should preserve the committed conflict choice metadata");
}

// Merge session save keeps hunk choices positional and capped at the reader's
// budget; a multi-hunk tab round-trips every hunk's choice exactly.
// TD-2026-07-17A-097.
void TestWorkspaceShellRestoreSessionRoundTripsMultiHunkMergeChoices() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path base = root / "base.txt";
  const std::filesystem::path incoming = root / "incoming.txt";
  const std::filesystem::path current = root / "current.txt";
  const std::filesystem::path output = root / "result.txt";
  // Two independent conflict hunks (section 1 and section 2).
  WriteFile(base,
            "section 1\nvalue: base 1\ncontext: unchanged1\n\n"
            "section 2\nvalue: base 2\ncontext: unchanged2\n");
  WriteFile(incoming,
            "section 1\nvalue: feature 1\ncontext: unchanged1\n\n"
            "section 2\nvalue: feature 2\ncontext: unchanged2\n");
  WriteFile(current,
            "section 1\nvalue: main 1\ncontext: unchanged1\n\n"
            "section 2\nvalue: main 2\ncontext: unchanged2\n");
  WriteFile(output,
            "section 1\nvalue: main 1\ncontext: unchanged1\n\n"
            "section 2\nvalue: main 2\ncontext: unchanged2\n");

  const std::filesystem::path home = temp_dir.path() / "home";
  const std::filesystem::path xdg_state_home = temp_dir.path() / "xdg-state-home";
  const std::filesystem::path xdg_config_home = temp_dir.path() / "xdg-config-home";
  std::filesystem::create_directories(home);
  std::filesystem::create_directories(xdg_state_home);
  std::filesystem::create_directories(xdg_config_home);
  ScopedEnvVar scoped_home("HOME", home.string());
  ScopedSessionAppHomes scoped_app_homes(xdg_state_home, xdg_config_home);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, current, output),
         "merge editor should open for the two-hunk fixture");

  auto& merge = WorkspaceShellTestAccess::ActiveMerge(shell);
  Expect(merge.model.hunks.size() >= 2, "fixture should produce at least two hunks");
  // Distinct explicit choices per hunk exercise the positional persist/restore.
  merge.model.hunks[0].choice = MergeChoice::Both;
  merge.model.hunks[1].choice = MergeChoice::Incoming;
  WorkspaceShellTestAccess::RefreshMergeTabDerivedState(shell);
  WorkspaceShellTestAccess::SaveSessionState(shell);

  WorkspaceShell restored;
  WorkspaceShellTestAccess::SetProjectRoot(restored, root);
  Expect(WorkspaceShellTestAccess::RestoreSessionState(restored),
         "merge session restore should succeed");
  const auto& rebuilt = WorkspaceShellTestAccess::ActiveMerge(restored);
  Expect(rebuilt.model.hunks.size() >= 2, "restore should rebuild both hunks");
  Expect(rebuilt.model.hunks[0].choice == MergeChoice::Both,
         "the first hunk restores to its persisted choice (positional)");
  Expect(rebuilt.model.hunks[1].choice == MergeChoice::Incoming,
         "the second hunk restores to its persisted choice (positional)");
}

void TestWorkspaceShellRestoreSessionDefersInactiveCleanEditorTabs() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  std::vector<std::filesystem::path> files;
  for (int i = 0; i < 20; ++i) {
    const std::filesystem::path path = root / ("file_" + std::to_string(i) + ".txt");
    WriteFile(path, "line 1\nline 2\nline 3\n");
    files.push_back(path);
  }

  const std::filesystem::path home = temp_dir.path() / "home";
  const std::filesystem::path xdg_state_home = temp_dir.path() / "xdg-state-home";
  const std::filesystem::path xdg_config_home = temp_dir.path() / "xdg-config-home";
  std::filesystem::create_directories(home);
  std::filesystem::create_directories(xdg_state_home);
  std::filesystem::create_directories(xdg_config_home);
  ScopedEnvVar scoped_home("HOME", home.string());
  ScopedSessionAppHomes scoped_app_homes(xdg_state_home, xdg_config_home);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, files.front());
  for (std::size_t i = 1; i < files.size(); ++i) {
    Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, files[i]),
           "session defer fixture should open each tab");
  }
  WorkspaceShellTestAccess::ActivateTab(shell, 0);
  WorkspaceShellTestAccess::SaveSessionState(shell);

  WorkspaceShell restored;
  WorkspaceShellTestAccess::SetProjectRoot(restored, root);
  Expect(WorkspaceShellTestAccess::RestoreSessionState(restored),
         "session restore should succeed for deferred-tab fixture");

  const auto& tabs = WorkspaceShellTestAccess::OpenTabs(restored);
  Expect(tabs.size() == files.size(),
         "session restore should preserve the total tab count");
  std::size_t deferred_count = 0;
  for (std::size_t i = 0; i < tabs.size(); ++i) {
    if (tabs[i].deferred_handle.has_value()) {
      ++deferred_count;
      if (i != 0) {
        Expect(!tabs[i].path.empty(),
               "deferred tab entries should preserve displayable paths in tab-strip state");
      }
    }
  }
  Expect(deferred_count >= 1,
         "inactive clean editor tabs should restore as deferred handles");
}

void TestWorkspaceShellDeferredTabHydrationPreservesCursorAndScroll() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path alpha = root / "alpha.txt";
  const std::filesystem::path beta = root / "beta.txt";
  WriteFile(alpha, "a1\na2\na3\na4\na5\na6\na7\na8\na9\na10\n");
  WriteFile(beta, "b1\nb2\nb3\nb4\nb5\nb6\nb7\nb8\nb9\nb10\n");

  const std::filesystem::path home = temp_dir.path() / "home";
  const std::filesystem::path xdg_state_home = temp_dir.path() / "xdg-state-home";
  const std::filesystem::path xdg_config_home = temp_dir.path() / "xdg-config-home";
  std::filesystem::create_directories(home);
  std::filesystem::create_directories(xdg_state_home);
  std::filesystem::create_directories(xdg_config_home);
  ScopedEnvVar scoped_home("HOME", home.string());
  ScopedSessionAppHomes scoped_app_homes(xdg_state_home, xdg_config_home);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, alpha);
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, beta),
         "deferred hydration fixture should open the secondary tab");
  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(6, 1);
  WorkspaceShellTestAccess::ActiveEditor(shell).SetScrollLine(4);
  WorkspaceShellTestAccess::ActiveEditor(shell).SetHorizontalScroll(0);
  WorkspaceShellTestAccess::ActivateTab(shell, 0);
  WorkspaceShellTestAccess::SaveSessionState(shell);

  WorkspaceShell restored;
  WorkspaceShellTestAccess::SetProjectRoot(restored, root);
  Expect(WorkspaceShellTestAccess::RestoreSessionState(restored),
         "deferred hydration restore should succeed");
  const auto& tabs_before = WorkspaceShellTestAccess::OpenTabs(restored);
  Expect(tabs_before.size() == 2, "deferred hydration restore should reopen both tabs");
  Expect(tabs_before[1].deferred_handle.has_value(),
         "inactive secondary tab should restore as deferred");
  Expect(tabs_before[1].deferred_handle->cursor_line == 6 &&
             tabs_before[1].deferred_handle->cursor_column == 1 &&
             tabs_before[1].deferred_handle->scroll_line == 4,
         "deferred handle should persist cursor/scroll metadata");

  WorkspaceShellTestAccess::ActivateTab(restored, 1);
  const auto& tabs_after = WorkspaceShellTestAccess::OpenTabs(restored);
  Expect(!tabs_after[1].deferred_handle.has_value() && tabs_after[1].editor_state.has_value(),
         "activating a deferred tab should hydrate and clear the deferred handle");
  const auto& hydrated = WorkspaceShellTestAccess::ActiveEditor(restored);
  Expect(hydrated.path() == beta.lexically_normal(),
         "hydrated deferred tab should open the same path");
  Expect(hydrated.cursor_line() == 6 && hydrated.cursor_column() == 1 &&
             hydrated.scroll_line() == 4,
         "hydrated deferred tab should restore cursor and scroll metadata");
}

// A folder rename must retarget the paths of deferred (session-restored,
// never-activated) editor tabs too, not only hydrated ones. Otherwise the
// deferred tab strands on the old path: activating it later fails to open, and
// the next session-save persists a path that no longer exists, dropping the tab.
void TestWorkspaceShellRenameRetargetsDeferredEditorTab() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path alpha = root / "src" / "alpha.txt";
  const std::filesystem::path beta = root / "src" / "beta.txt";
  WriteFile(alpha, "a1\na2\na3\n");
  WriteFile(beta, "b1\nb2\nb3\n");

  const std::filesystem::path home = temp_dir.path() / "home";
  const std::filesystem::path xdg_state_home = temp_dir.path() / "xdg-state-home";
  const std::filesystem::path xdg_config_home = temp_dir.path() / "xdg-config-home";
  std::filesystem::create_directories(home);
  std::filesystem::create_directories(xdg_state_home);
  std::filesystem::create_directories(xdg_config_home);
  ScopedEnvVar scoped_home("HOME", home.string());
  ScopedSessionAppHomes scoped_app_homes(xdg_state_home, xdg_config_home);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, alpha);
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, beta),
         "rename-deferred fixture should open the secondary tab");
  WorkspaceShellTestAccess::ActivateTab(shell, 0);  // beta (index 1) becomes inactive => deferred
  WorkspaceShellTestAccess::SaveSessionState(shell);

  WorkspaceShell restored;
  WorkspaceShellTestAccess::SetProjectRoot(restored, root);
  Expect(WorkspaceShellTestAccess::RestoreSessionState(restored),
         "rename-deferred restore should succeed");
  const auto& tabs = WorkspaceShellTestAccess::OpenTabs(restored);
  Expect(tabs.size() == 2, "rename-deferred restore should reopen both tabs");
  Expect(tabs[1].deferred_handle.has_value(),
         "inactive secondary tab should restore as deferred before rename");

  // Rename the enclosing folder while the beta tab is still deferred.
  WorkspaceShellTestAccess::PrepareRenamePrompt(restored, root / "src", "renamed-src");
  WorkspaceShellTestAccess::ConfirmPromptSurface(restored);

  const std::filesystem::path renamed_beta = root / "renamed-src" / "beta.txt";
  Expect(std::filesystem::is_regular_file(renamed_beta),
         "folder rename should move beta.txt to the new directory");

  const auto& tabs_after = WorkspaceShellTestAccess::OpenTabs(restored);
  Expect(tabs_after[1].deferred_handle.has_value() &&
             tabs_after[1].deferred_handle->path == renamed_beta.lexically_normal(),
         "deferred tab's deferred_handle path must be retargeted to the renamed folder");
  Expect(tabs_after[1].path == renamed_beta.lexically_normal(),
         "deferred tab's tab-strip path must be retargeted too");

  // Activating the retargeted deferred tab must hydrate from the new path.
  WorkspaceShellTestAccess::ActivateTab(restored, 1);
  const auto& hydrated = WorkspaceShellTestAccess::ActiveEditor(restored);
  Expect(hydrated.path() == renamed_beta.lexically_normal(),
         "activating the retargeted deferred tab must open the renamed path");
}

void TestWorkspaceShellRestoreSessionTabSwitchSelectsTreePath() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path alpha = root / "src" / "alpha.txt";
  const std::filesystem::path beta = root / "docs" / "beta.txt";
  WriteFile(alpha, "alpha\n");
  WriteFile(beta, "beta\n");

  const std::filesystem::path home = temp_dir.path() / "home";
  const std::filesystem::path xdg_state_home = temp_dir.path() / "xdg-state-home";
  const std::filesystem::path xdg_config_home = temp_dir.path() / "xdg-config-home";
  std::filesystem::create_directories(home);
  std::filesystem::create_directories(xdg_state_home);
  std::filesystem::create_directories(xdg_config_home);
  ScopedEnvVar scoped_home("HOME", home.string());
  ScopedEnvVar scoped_xdg_state_home("XDG_STATE_HOME", xdg_state_home.string());
  ScopedEnvVar scoped_xdg_config_home("XDG_CONFIG_HOME", xdg_config_home.string());

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "tree selection restore fixture should open the project");
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, alpha),
         "tree selection restore fixture should open the primary tab");
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, beta),
         "tree selection restore fixture should open the secondary tab");
  WorkspaceShellTestAccess::ActivateTab(shell, 0);
  WorkspaceShellTestAccess::SaveSessionState(shell);
  WorkspaceShellTestAccess::SaveWorkspaceSession(shell);

  WorkspaceShell restored;
  Expect(WorkspaceShellTestAccess::RestoreWorkspaceSession(restored),
         "tree selection restore fixture should restore the workspace session");
  Expect(WorkspaceShellTestAccess::SelectedTreePath(restored) == alpha.lexically_normal(),
         "restored active tab should select its file path in the project tree");

  WorkspaceShellTestAccess::ActivateTab(restored, 1);
  Expect(WorkspaceShellTestAccess::SelectedTreePath(restored) == beta.lexically_normal(),
         "switching restored tabs should select the active file path in the project tree");
}

void TestWorkspaceShellRestoreSessionPreservesOutgoingBaseChoice() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "README.md", "hello\n");

  const std::filesystem::path home = temp_dir.path() / "home";
  const std::filesystem::path xdg_state_home = temp_dir.path() / "xdg-state-home";
  const std::filesystem::path xdg_config_home = temp_dir.path() / "xdg-config-home";
  std::filesystem::create_directories(home);
  std::filesystem::create_directories(xdg_state_home);
  std::filesystem::create_directories(xdg_config_home);
  ScopedEnvVar scoped_home("HOME", home.string());
  ScopedSessionAppHomes scoped_app_homes(xdg_state_home, xdg_config_home);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  auto& state = WorkspaceShellTestAccess::CurrentProjectState(shell);
  state.sidebar.git.outgoing_base_choice = microide::workspace::OutgoingBaseChoice{
      .kind = microide::workspace::OutgoingBaseChoice::Kind::SpecificRef,
      .custom_ref = "origin/release/2026-04",
  };
  WorkspaceShellTestAccess::SaveSessionState(shell);

  WorkspaceShell restored;
  WorkspaceShellTestAccess::SetProjectRoot(restored, root);
  Expect(WorkspaceShellTestAccess::RestoreSessionState(restored),
         "outgoing base choice fixture should restore project session state");
  const auto& restored_state = WorkspaceShellTestAccess::CurrentProjectState(restored);
  Expect(restored_state.sidebar.git.outgoing_base_choice.kind ==
             microide::workspace::OutgoingBaseChoice::Kind::SpecificRef &&
             restored_state.sidebar.git.outgoing_base_choice.custom_ref ==
                 "origin/release/2026-04",
         "project session restore should preserve the outgoing base choice");
}

void TestWorkspaceShellRestoreSessionPreservesEditorGroupSplit() {
  using microide::workspace::EditorSplitOrientation;
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file_a = root / "a.txt";
  const std::filesystem::path file_b = root / "b.txt";
  std::string many_lines;
  for (int i = 0; i < 200; ++i) {
    many_lines += "line " + std::to_string(i) + "\n";
  }
  WriteFile(file_a, many_lines);
  WriteFile(file_b, "only b\n");

  const std::filesystem::path home = temp_dir.path() / "home";
  const std::filesystem::path xdg_state_home = temp_dir.path() / "xdg-state-home";
  const std::filesystem::path xdg_config_home = temp_dir.path() / "xdg-config-home";
  std::filesystem::create_directories(home);
  std::filesystem::create_directories(xdg_state_home);
  std::filesystem::create_directories(xdg_config_home);
  ScopedEnvVar scoped_home("HOME", home.string());
  ScopedSessionAppHomes scoped_app_homes(xdg_state_home, xdg_config_home);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1000, 700);
  WorkspaceShellTestAccess::OpenFile(shell, file_a);
  WorkspaceShellTestAccess::SetGroupScrollLine(shell, 0, 40);
  // Stack a second group (split-down) and give it a distinct document; focus
  // stays on the new group.
  Expect(WorkspaceShellTestAccess::SplitEditorGroup(shell, EditorSplitOrientation::Horizontal),
         "split-down should create the second group");
  WorkspaceShellTestAccess::OpenFile(shell, file_b);
  WorkspaceShellTestAccess::SaveSessionState(shell);

  WorkspaceShell restored;
  WorkspaceShellTestAccess::SetProjectRoot(restored, root);
  WorkspaceShellTestAccess::SetWindowSize(restored, 1000, 700);
  Expect(WorkspaceShellTestAccess::RestoreSessionState(restored),
         "restoring a two-group session should succeed");
  // Mirror the production startup flow, which hydrates every group's active tab.
  WorkspaceShellTestAccess::ActivateCurrentTabAfterStateLoad(restored);
  Expect(WorkspaceShellTestAccess::EditorGroupCount(restored) == 2,
         "restore should rebuild both editor groups");
  Expect(WorkspaceShellTestAccess::GroupSplitOrientation(restored) ==
             EditorSplitOrientation::Horizontal,
         "restore should preserve the stacked split orientation");
  Expect(WorkspaceShellTestAccess::FocusedGroupIndex(restored) == 1,
         "restore should preserve the focused group index");
  Expect(WorkspaceShellTestAccess::GroupActiveViewport(restored, 0).path() ==
             file_a.lexically_normal(),
         "group 0 should restore its document");
  Expect(WorkspaceShellTestAccess::GroupActiveViewport(restored, 1).path() ==
             file_b.lexically_normal(),
         "group 1 should restore its distinct document");
  Expect(WorkspaceShellTestAccess::GroupActiveViewport(restored, 0).scroll_line() == 40,
         "group 0 should restore its independent scroll position");
}

void TestWorkspaceShellAfterDelayAutosaveSurvivesTabSwitch() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file_a = root / "a.txt";
  const std::filesystem::path file_b = root / "b.txt";
  WriteFile(file_a, "alpha\n");
  WriteFile(file_b, "beta\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, file_a);  // tab 0
  WorkspaceShellTestAccess::OpenFile(shell, file_b);  // tab 1 (active)
  Expect(WorkspaceShellTestAccess::OpenTabs(shell).size() == 2, "both files should open as tabs");

  Expect(WorkspaceShellTestAccess::SetSettingValue(shell, "editor.autosave", "after_delay"),
         "after_delay autosave should be settable");

  // Focus tab A and let the autosave sampler baseline it (mirrors the per-input-batch
  // MaybeArmAutosaveTimer call the event loop makes before the first edit).
  WorkspaceShellTestAccess::ActivateTab(shell, 0);
  WorkspaceShellTestAccess::MaybeArmAutosaveTimer(shell);
  Expect(!WorkspaceShellTestAccess::AutosaveArmed(shell),
         "no edit yet -> the after_delay autosave timer should be idle");

  // Edit A so it becomes dirty, then re-sample to arm the debounce.
  auto& editor_a = WorkspaceShellTestAccess::ActiveEditor(shell);
  editor_a.MoveCursorTo(0, 0);
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "x"), "editing tab A should be accepted");
  WorkspaceShellTestAccess::MaybeArmAutosaveTimer(shell);
  Expect(WorkspaceShellTestAccess::AutosaveArmed(shell),
         "editing a dirty path-backed buffer should arm the after_delay autosave");

  // Switch to the clean tab B before the debounce elapses and re-sample. Tab A's
  // pending autosave must survive the switch: content_revision() is per-viewport, so
  // sampling B used to look like an edit and disarm A's flush, silently dropping its
  // unsaved edits until an unrelated edit re-armed.
  WorkspaceShellTestAccess::ActivateTab(shell, 1);
  WorkspaceShellTestAccess::MaybeArmAutosaveTimer(shell);
  Expect(WorkspaceShellTestAccess::AutosaveArmed(shell),
         "switching to a clean tab must not disarm tab A's pending after_delay autosave");
}

// Regression: closing the active tab promotes a neighbor. If that neighbor is a
// session-restored, still-deferred editor tab, the promote path must honor its
// restored cursor/scroll (from deferred_handle) rather than opening fresh at
// (0,0). Previously the bespoke promote branch ignored deferred_handle, silently
// losing the persisted view state (and leaving a stale handle behind).
void TestWorkspaceShellClosePromotesDeferredTabWithRestoredViewState() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file_a = root / "a.txt";
  const std::filesystem::path file_b = root / "b.txt";
  WriteFile(file_a, "alpha\n");
  std::string b_content;
  for (int i = 0; i < 200; ++i) {
    b_content += "abcdef\n";
  }
  WriteFile(file_b, b_content);

  const std::filesystem::path home = temp_dir.path() / "home";
  const std::filesystem::path xdg_state_home = temp_dir.path() / "xdg-state-home";
  const std::filesystem::path xdg_config_home = temp_dir.path() / "xdg-config-home";
  std::filesystem::create_directories(home);
  std::filesystem::create_directories(xdg_state_home);
  std::filesystem::create_directories(xdg_config_home);
  ScopedEnvVar scoped_home("HOME", home.string());
  ScopedSessionAppHomes scoped_app_homes(xdg_state_home, xdg_config_home);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, file_a);  // tab 0
  WorkspaceShellTestAccess::OpenFile(shell, file_b);  // tab 1 (active)
  auto& editor_b = WorkspaceShellTestAccess::ActiveEditor(shell);
  editor_b.MoveCursorTo(2, 3);
  editor_b.SetScrollLine(10);
  // Make tab A active so that on restore B is the DEFERRED neighbor.
  WorkspaceShellTestAccess::ActivateTab(shell, 0);
  WorkspaceShellTestAccess::SaveSessionState(shell);

  WorkspaceShell restored;
  WorkspaceShellTestAccess::SetProjectRoot(restored, root);
  Expect(WorkspaceShellTestAccess::RestoreSessionState(restored),
         "session restore should succeed");
  // Hydrate only the active tab (A); tab B stays deferred, as on real startup.
  WorkspaceShellTestAccess::ActivateCurrentTabAfterStateLoad(restored);

  // Close the active tab WITHOUT first activating B; B is promoted while deferred.
  WorkspaceShellTestAccess::RequestCloseTab(restored, 0);

  const auto& promoted = WorkspaceShellTestAccess::ActiveEditor(restored);
  Expect(promoted.cursor_line() == 2 && promoted.cursor_column() == 3,
         "promoting a deferred tab on close must restore its persisted caret");
  Expect(promoted.scroll_line() == 10,
         "promoting a deferred tab on close must restore its persisted scroll position");
}

}  // namespace

void RegisterWorkspaceShellSessionTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceShell/ClosePromotesDeferredTabWithRestoredViewState",
          TestWorkspaceShellClosePromotesDeferredTabWithRestoredViewState);
  AddTest(tests, "WorkspaceShell/AfterDelayAutosaveSurvivesTabSwitch",
          TestWorkspaceShellAfterDelayAutosaveSurvivesTabSwitch);
  AddTest(tests, "WorkspaceShell/RestoreSessionPreservesEditorGroupSplit",
          TestWorkspaceShellRestoreSessionPreservesEditorGroupSplit);
  AddTest(tests, "WorkspaceShell/RestoreSessionPreservesBranchCompareState",
          TestWorkspaceShellRestoreSessionPreservesBranchCompareState);
  AddTest(tests, "WorkspaceShell/RestoreSessionPreservesRenamedWorkingTreeCompareState",
          TestWorkspaceShellRestoreSessionPreservesRenamedWorkingTreeCompareState);
  AddTest(tests, "WorkspaceShell/RestoreClampsOutOfRangeSelectedLaunchConfig",
          TestWorkspaceShellRestoreClampsOutOfRangeSelectedLaunchConfig);
  AddTest(tests, "WorkspaceShell/ReopenFileReloadsCleanEditorTab",
          TestWorkspaceShellReopenFileReloadsCleanEditorTab);
  AddTest(tests, "WorkspaceShell/RefreshReloadsCleanOpenEditorBuffers",
          TestWorkspaceShellRefreshReloadsCleanOpenEditorBuffers);
  AddTest(tests, "WorkspaceShell/ReopenClearsFoldCollapseState",
          TestWorkspaceShellReopenClearsFoldCollapseState);
  AddTest(tests, "WorkspaceShell/RefreshClearsFoldCollapseState",
          TestWorkspaceShellRefreshClearsFoldCollapseState);
  AddTest(tests, "WorkspaceShell/EditorEditInvalidatesFoldingFingerprint",
          TestWorkspaceShellEditorEditInvalidatesFoldingFingerprint);
  AddTest(tests, "WorkspaceShell/EnsureActiveFoldingModelFreshBinding",
          TestWorkspaceShellEnsureActiveFoldingModelFreshBinding);
  AddTest(tests, "WorkspaceShell/FoldingSurvivesTabVectorReallocationAndLayout",
          TestWorkspaceShellFoldingSurvivesTabVectorReallocationAndLayout);
  AddTest(tests, "WorkspaceShell/BufferSearchRevealsCollapsedMatchAndKeepsItOnClose",
          TestWorkspaceShellBufferSearchRevealsCollapsedMatchAndKeepsItOnClose);
  AddTest(tests, "WorkspaceShell/BufferSearchKeepsRevealAfterClose",
          TestWorkspaceShellBufferSearchKeepsRevealAfterClose);
  AddTest(tests, "WorkspaceShell/FoldAllUnfoldAllCtrlKChord",
          TestWorkspaceShellFoldAllUnfoldAllCtrlKChord);
  AddTest(tests, "WorkspaceShell/CrashSafetyFlushPersistsUnsavedContent",
          TestWorkspaceShellCrashSafetyFlushPersistsUnsavedContent);
  AddTest(tests, "WorkspaceShell/CrashSafetyFlushTimerArmsOnEdit",
          TestWorkspaceShellCrashSafetyFlushTimerArmsOnEdit);
  AddTest(tests, "WorkspaceShell/RestoreSessionPreservesDirtyEditorBufferContent",
          TestWorkspaceShellRestoreSessionPreservesDirtyEditorBufferContent);
  AddTest(tests, "WorkspaceShell/RestoreSessionPreservesIndependentScrollPosition",
          TestWorkspaceShellRestoreSessionPreservesIndependentScrollPosition);
  AddTest(tests, "WorkspaceShell/RestoreSessionPreservesTreeExpansion",
          TestWorkspaceShellRestoreSessionPreservesTreeExpansion);
  AddTest(tests, "WorkspaceShell/ActivatingDeletedDeferredTabPreservesIdentity",
          TestWorkspaceShellActivatingDeletedDeferredTabPreservesIdentity);
  AddTest(tests, "WorkspaceShell/RestoreSessionPreservesDirtyUntitledBufferContent",
          TestWorkspaceShellRestoreSessionPreservesDirtyUntitledBufferContent);
  AddTest(tests, "WorkspaceShell/QuitShutdownPersistsDirtyEditorBuffers",
          TestWorkspaceShellQuitShutdownPersistsDirtyEditorBuffers);
  AddTest(tests, "WorkspaceShell/ReopenWorkingTreeComparisonRefreshesExistingTab",
          TestWorkspaceShellReopenWorkingTreeComparisonRefreshesExistingTab);
  AddTest(tests, "WorkspaceShell/CompareSyntaxTokensAreDeferredUntilRender",
          TestWorkspaceShellCompareSyntaxTokensAreDeferredUntilRender);
  AddTest(tests, "WorkspaceShell/MergeSyntaxTokensAreDeferredUntilRender",
          TestWorkspaceShellMergeSyntaxTokensAreDeferredUntilRender);
  AddTest(tests, "WorkspaceShell/ReopenMergeEditorRefreshesCleanTabFromOutput",
          TestWorkspaceShellReopenMergeEditorRefreshesCleanTabFromOutput);
  AddTest(tests, "WorkspaceShell/MergeEditorUsesWorkingTreeConflictMarkers",
          TestWorkspaceShellMergeEditorUsesWorkingTreeConflictMarkers);
  AddTest(tests, "WorkspaceShell/MergeEditorParsesLargeWorkingTreeConflictBlock",
          TestWorkspaceShellMergeEditorParsesLargeWorkingTreeConflictBlock);
  AddTest(tests, "WorkspaceShell/RawConflictMarkersOpenUnresolved",
          TestWorkspaceShellRawConflictMarkersOpenUnresolved);
  AddTest(tests, "WorkspaceShell/InsertionAtConflictStartKeepsTracking",
          TestWorkspaceShellInsertionAtConflictStartKeepsTracking);
  AddTest(tests, "WorkspaceShell/MergeBothOrdersAndBaseToggle",
          TestWorkspaceShellMergeBothOrdersAndBaseToggle);
  AddTest(tests, "WorkspaceShell/MergeChoicePreservesManualEditsAroundConflicts",
          TestWorkspaceShellMergeChoicePreservesManualEditsAroundConflicts);
  AddTest(tests, "WorkspaceShell/MergeConflictTrackingShiftsAfterInsertion",
          TestWorkspaceShellMergeConflictTrackingShiftsAfterInsertion);
  AddTest(tests, "WorkspaceShell/MergeHoverPreviewDoesNotCommitState",
          TestWorkspaceShellMergeHoverPreviewDoesNotCommitState);
  AddTest(tests, "WorkspaceShell/MergeHoverPrefersIncomingAcceptButton",
          TestWorkspaceShellMergeHoverPrefersIncomingAcceptButton);
  AddTest(tests, "WorkspaceShell/MergeHoverPrefersResultActionButton",
          TestWorkspaceShellMergeHoverPrefersResultActionButton);
  AddTest(tests, "WorkspaceShell/MergeResultDragSelectionTracksPointer",
          TestWorkspaceShellMergeResultDragSelectionTracksPointer);
  AddTest(tests, "WorkspaceShell/MergeDividerDragUpdatesPaneFractions",
          TestWorkspaceShellMergeDividerDragUpdatesPaneFractions);
  AddTest(tests, "WorkspaceShell/MergeWheelScrollsRows",
          TestWorkspaceShellMergeWheelScrollsRows);
  AddTest(tests, "WorkspaceShell/MergeToolbarButtonsNavigateConflicts",
          TestWorkspaceShellMergeToolbarButtonsNavigateConflicts);
  AddTest(tests, "WorkspaceShell/MergeHorizontalNavigationInvalidatesResultPane",
          TestWorkspaceShellMergeHorizontalNavigationInvalidatesResultPane);
  AddTest(tests, "WorkspaceShell/MoveMergeSelectionInvalidatesConflictBand",
          TestWorkspaceShellMoveMergeSelectionInvalidatesConflictBand);
  AddTest(tests, "WorkspaceShell/SessionSaveOmitsOverBudgetDirtySnapshot",
          TestWorkspaceShellSessionSaveOmitsOverBudgetDirtySnapshot);
  AddTest(tests, "WorkspaceShell/RestoreSessionCapsTabCount",
          TestWorkspaceShellRestoreSessionCapsTabCount);
  AddTest(tests, "WorkspaceShell/RestoreSessionPreservesMergeNavigationState",
          TestWorkspaceShellRestoreSessionPreservesMergeNavigationState);
  AddTest(tests, "WorkspaceShell/RestoreSessionRoundTripsMultiHunkMergeChoices",
          TestWorkspaceShellRestoreSessionRoundTripsMultiHunkMergeChoices);
  AddTest(tests, "WorkspaceShell/RestoreSessionDefersInactiveCleanEditorTabs",
          TestWorkspaceShellRestoreSessionDefersInactiveCleanEditorTabs);
  AddTest(tests, "WorkspaceShell/DeferredTabHydrationPreservesCursorAndScroll",
          TestWorkspaceShellDeferredTabHydrationPreservesCursorAndScroll);
  AddTest(tests, "WorkspaceShell/RenameRetargetsDeferredEditorTab",
          TestWorkspaceShellRenameRetargetsDeferredEditorTab);
  AddTest(tests, "WorkspaceShell/RestoreSessionTabSwitchSelectsTreePath",
          TestWorkspaceShellRestoreSessionTabSwitchSelectsTreePath);
  AddTest(tests, "WorkspaceShell/RestoreSessionPreservesOutgoingBaseChoice",
          TestWorkspaceShellRestoreSessionPreservesOutgoingBaseChoice);
  AddTest(tests, "WorkspaceShell/RestoreWorkspaceSessionAcrossProjects",
          TestWorkspaceShellRestoreWorkspaceSessionAcrossProjects);
  AddTest(tests, "WorkspaceShell/RestoreRemapsActiveIndexAfterMissingProjectCulled",
          TestWorkspaceShellRestoreRemapsActiveIndexAfterMissingProjectCulled);
  AddTest(tests, "WorkspaceShell/RestoredProjectTabBadgeColorsHydrateOnStartup",
          TestWorkspaceShellRestoredProjectTabBadgeColorsHydrateOnStartup);
  AddTest(tests, "WorkspaceShell/ShutdownPreservesDistinctWorkspaceProjectRoots",
          TestWorkspaceShellShutdownPreservesDistinctWorkspaceProjectRoots);
}

}  // namespace microide::tests
