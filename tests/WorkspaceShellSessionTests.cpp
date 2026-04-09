#include "TestSupport.h"

#include "WorkspaceShellTestAccess.h"
#include "project/GitCompareService.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::WorkspaceShell;
using microide::workspace::WorkspaceShellTestAccess;

std::string EscapedRepoPath(const std::filesystem::path& repo_path) {
  return ShellEscape(repo_path.string());
}

void InitializeGitRepo(const std::filesystem::path& repo_path) {
  const std::string escaped_repo = EscapedRepoPath(repo_path);
  RequireCommandSuccess(
      "git -c init.defaultBranch=main init '" + escaped_repo + "' >/dev/null 2>/dev/null",
      "git init");
  RequireCommandSuccess(
      "git -C '" + escaped_repo + "' config user.name 'Microide Tests' >/dev/null 2>/dev/null",
      "git config user.name");
  RequireCommandSuccess(
      "git -C '" + escaped_repo +
          "' config user.email 'microide-tests@example.com' >/dev/null 2>/dev/null",
      "git config user.email");
}

void CommitAll(const std::filesystem::path& repo_path,
               std::string_view message,
               std::string_view context) {
  const std::string escaped_repo = EscapedRepoPath(repo_path);
  RequireCommandSuccess("git -C '" + escaped_repo + "' add . >/dev/null 2>/dev/null",
                        std::string(context) + " add");
  RequireCommandSuccess(
      "git -C '" + escaped_repo + "' commit -m '" + std::string(message) +
          "' >/dev/null 2>/dev/null",
      std::string(context) + " commit");
}

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
  ScopedEnvVar scoped_xdg_state_home("XDG_STATE_HOME", xdg_state_home.string());
  ScopedEnvVar scoped_xdg_config_home("XDG_CONFIG_HOME", xdg_config_home.string());

  InitializeGitRepo(root);
  CommitAll(root, "base fixture", "base fixture");
  WriteFile(source, "head line\n");
  CommitAll(root, "head fixture", "head fixture");

  const auto history = microide::project::CollectGitFileHistory(root, source);
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
  ScopedEnvVar scoped_xdg_state_home("XDG_STATE_HOME", xdg_state_home.string());
  ScopedEnvVar scoped_xdg_config_home("XDG_CONFIG_HOME", xdg_config_home.string());

  InitializeGitRepo(repo_root);
  CommitAll(repo_root, "base fixture", "base fixture");
  WriteFile(repo_file, "head line\n");
  CommitAll(repo_root, "head fixture", "head fixture");

  const auto history = microide::project::CollectGitFileHistory(repo_root, repo_file);
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
  ScopedEnvVar scoped_xdg_state_home("XDG_STATE_HOME", xdg_state_home.string());
  ScopedEnvVar scoped_xdg_config_home("XDG_CONFIG_HOME", xdg_config_home.string());

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

}  // namespace

void RegisterWorkspaceShellSessionTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceShell/RestoreSessionPreservesBranchCompareState",
          TestWorkspaceShellRestoreSessionPreservesBranchCompareState);
  AddTest(tests, "WorkspaceShell/RestoreSessionPreservesRenamedWorkingTreeCompareState",
          TestWorkspaceShellRestoreSessionPreservesRenamedWorkingTreeCompareState);
  AddTest(tests, "WorkspaceShell/RestoreWorkspaceSessionAcrossProjects",
          TestWorkspaceShellRestoreWorkspaceSessionAcrossProjects);
}

}  // namespace microide::tests
