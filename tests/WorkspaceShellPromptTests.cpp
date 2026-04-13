#include "TestSupport.h"

#include "project/GitCompareService.h"
#include "WorkspaceShellTestAccess.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <optional>
#include <string>
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

std::optional<std::filesystem::path> FirstRegularFileIn(const std::filesystem::path& directory) {
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
    if (error) {
      return std::nullopt;
    }
    if (entry.is_regular_file()) {
      return entry.path();
    }
  }
  return std::nullopt;
}

void TestWorkspaceShellRenamePromptSavesDirtyTabs() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  std::filesystem::create_directories(root);
  const std::filesystem::path source = root / "notes.txt";
  WriteFile(source, "base text\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, source);
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("edited ");

  WorkspaceShellTestAccess::PrepareRenamePrompt(shell, source, "renamed.txt");
  WorkspaceShellTestAccess::ConfirmPromptSurface(shell);

  Expect(WorkspaceShellTestAccess::PromptSurfaceVisible(shell),
         "rename prompt should remain open while waiting on dirty confirmation");
  Expect(WorkspaceShellTestAccess::DirtyPromptVisible(shell),
         "rename should show a dirty confirmation instead of blocking");

  WorkspaceShellTestAccess::ConfirmDirtyPrompt(shell, 0);

  const std::filesystem::path renamed = root / "renamed.txt";
  Expect(!WorkspaceShellTestAccess::DirtyPromptVisible(shell),
         "rename save flow should close the dirty confirmation");
  Expect(!WorkspaceShellTestAccess::PromptSurfaceVisible(shell),
         "rename save flow should close the rename prompt");
  Expect(!std::filesystem::exists(source), "rename save flow should remove the source path");
  Expect(std::filesystem::is_regular_file(renamed),
         "rename save flow should create the destination path");
  Expect(ReadFile(renamed) == "edited base text\n",
         "rename save flow should persist the dirty editor content before renaming");
  Expect(WorkspaceShellTestAccess::OpenTabs(shell).size() == 1,
         "rename save flow should keep the affected editor tab open");
  Expect(WorkspaceShellTestAccess::OpenTabs(shell).front().path == renamed.lexically_normal(),
         "rename save flow should retarget the editor tab path");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).path() == renamed.lexically_normal(),
         "rename save flow should retarget the live editor viewport");
  Expect(!WorkspaceShellTestAccess::ActiveEditor(shell).dirty(),
         "rename save flow should clear the dirty flag after saving");
}

void TestWorkspaceShellRenamePromptOnlySavesAffectedSplitEditor() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  std::filesystem::create_directories(root);
  const std::filesystem::path keep = root / "keep.txt";
  const std::filesystem::path source = root / "rename-me.txt";
  WriteFile(keep, "keep text\n");
  WriteFile(source, "source text\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, keep);
  Expect(WorkspaceShellTestAccess::SplitActiveEditor(shell),
         "split rename fixture should open a second editor pane");
  Expect(WorkspaceShellTestAccess::ReplaceActiveEditorWithFile(shell, source),
         "split rename fixture should load the renamed file into the active pane");

  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("renamed ");
  Expect(WorkspaceShellTestAccess::ActivateOrderedEditorSplit(shell, 0),
         "split rename fixture should move focus to the first pane");
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("keep ");
  Expect(WorkspaceShellTestAccess::ActivateOrderedEditorSplit(shell, 1),
         "split rename fixture should move focus back to the renamed pane");

  WorkspaceShellTestAccess::PrepareRenamePrompt(shell, source, "renamed.txt");
  WorkspaceShellTestAccess::ConfirmPromptSurface(shell);

  Expect(WorkspaceShellTestAccess::DirtyPromptVisible(shell),
         "split rename should still prompt for the affected dirty pane");
  Expect(WorkspaceShellTestAccess::DirtyPromptMessage(shell).find("affected dirty editor") !=
             std::string::npos,
         "split rename prompt should describe affected editors instead of tabs");

  WorkspaceShellTestAccess::ConfirmDirtyPrompt(shell, 0);

  const std::filesystem::path renamed = root / "renamed.txt";
  Expect(std::filesystem::is_regular_file(renamed),
         "split rename save flow should create the destination path");
  Expect(ReadFile(renamed) == "renamed source text\n",
         "split rename save flow should persist the affected dirty pane");
  Expect(ReadFile(keep) == "keep text\n",
         "split rename save flow should not save unrelated dirty panes");
  Expect(WorkspaceShellTestAccess::OpenTabs(shell).size() == 1,
         "split rename save flow should keep the editor tab open");
  Expect(WorkspaceShellTestAccess::OpenTabs(shell).front().editor_state->views.size() == 2,
         "split rename save flow should preserve unaffected split panes");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).path() == renamed.lexically_normal(),
         "split rename save flow should keep the renamed pane active");
  Expect(!WorkspaceShellTestAccess::ActiveEditor(shell).dirty(),
         "split rename save flow should clear the renamed pane dirty flag");

  bool kept_view_found = false;
  bool kept_view_dirty = false;
  for (const auto& view : WorkspaceShellTestAccess::OpenTabs(shell).front().editor_state->views) {
    const std::filesystem::path view_path =
        (view.needs_restore ? view.restored_path : view.viewport.path()).lexically_normal();
    if (view_path == keep.lexically_normal()) {
      kept_view_found = true;
      kept_view_dirty = view.viewport.dirty();
      break;
    }
  }
  Expect(kept_view_found, "split rename save flow should keep the unrelated pane");
  Expect(kept_view_dirty, "split rename save flow should preserve unrelated dirty pane state");
}

#if defined(__linux__) || defined(__APPLE__)
void TestWorkspaceShellDeletePromptDiscardsDirtyTabs() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  std::filesystem::create_directories(root);
  const std::filesystem::path source = root / "trash-me.txt";
  WriteFile(source, "original text\n");

  const std::filesystem::path home = temp_dir.path() / "home";
  const std::filesystem::path xdg_data_home = temp_dir.path() / "xdg-data-home";
  std::filesystem::create_directories(home);
  std::filesystem::create_directories(xdg_data_home);
  ScopedEnvVar scoped_home("HOME", home.string());
  ScopedEnvVar scoped_xdg_data_home("XDG_DATA_HOME", xdg_data_home.string());

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, source);
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("discarded ");

  WorkspaceShellTestAccess::PrepareDeletePrompt(shell, source);
  WorkspaceShellTestAccess::ConfirmPromptSurface(shell);

  Expect(WorkspaceShellTestAccess::PromptSurfaceVisible(shell),
         "delete prompt should remain open while waiting on dirty confirmation");
  Expect(WorkspaceShellTestAccess::DirtyPromptVisible(shell),
         "delete should show a dirty confirmation instead of blocking");

  WorkspaceShellTestAccess::ConfirmDirtyPrompt(shell, 1);

  Expect(!WorkspaceShellTestAccess::DirtyPromptVisible(shell),
         "delete discard flow should close the dirty confirmation");
  Expect(!WorkspaceShellTestAccess::PromptSurfaceVisible(shell),
         "delete discard flow should close the delete prompt");
  Expect(!std::filesystem::exists(source),
         "delete discard flow should remove the project path");
  Expect(WorkspaceShellTestAccess::OpenTabs(shell).empty(),
         "delete discard flow should close affected tabs");

#if defined(__linux__)
  const std::filesystem::path trash_files = xdg_data_home / "Trash" / "files";
#else
  const std::filesystem::path trash_files = home / ".Trash";
#endif
  const auto trashed_file = FirstRegularFileIn(trash_files);
  Expect(trashed_file.has_value(), "delete discard flow should create a trash entry");
  Expect(ReadFile(*trashed_file) == "original text\n",
         "delete discard flow should discard unsaved editor changes before trashing");
}

void TestWorkspaceShellDiscardAllGitPromptDiscardsWorkingTreeChanges() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  CopyTree(FixturePath("diff/git/base"), root);
  InitializeGitRepo(root);
  CommitAll(root, "base fixture", "base fixture");

  const std::filesystem::path modified = root / "README.md";
  const std::filesystem::path deleted = root / "src/session.cpp";
  const std::filesystem::path staged_added = root / "src/new_panel.cpp";
  const std::filesystem::path untracked = root / "scratch.txt";
  WriteFile(modified, ReadFile(modified) + "\nthrowaway change\n");
  std::filesystem::remove(deleted);
  WriteFile(staged_added, "int meaning = 42;\n");
  WriteFile(untracked, "scratch\n");
  RequireCommandSuccess("git -C '" + EscapedRepoPath(root) + "' add -A >/dev/null 2>/dev/null",
                        "prepare staged changes");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::ShowGitSidebar(shell);
  WorkspaceShellTestAccess::PrepareDiscardAllGitPrompt(shell);

  Expect(WorkspaceShellTestAccess::PromptSurfaceVisible(shell),
         "discard all should show a confirmation prompt");
  Expect(WorkspaceShellTestAccess::PromptSurfaceTitle(shell) == "Discard All Changes",
         "discard all prompt should have the expected title");
  Expect(WorkspaceShellTestAccess::PromptSurfaceMessage(shell).find("tracked, untracked, and conflicted") !=
             std::string::npos,
         "discard all prompt should describe the destructive scope");

  WorkspaceShellTestAccess::ConfirmPromptSurface(shell);

  Expect(!WorkspaceShellTestAccess::PromptSurfaceVisible(shell),
         "discard all should close the confirmation prompt after success");
  Expect(ReadFile(modified) == ReadFile(FixturePath("diff/git/base/README.md")),
         "discard all should restore modified tracked files");
  Expect(std::filesystem::exists(deleted),
         "discard all should restore deleted tracked files");
  Expect(!std::filesystem::exists(staged_added),
         "discard all should remove staged added files");
  Expect(!std::filesystem::exists(untracked),
         "discard all should remove untracked files");
}

void TestWorkspaceShellDiscardAllGitPromptBlocksDirtyEditors() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  std::filesystem::create_directories(root);
  const std::filesystem::path file_path = root / "notes.txt";
  WriteFile(file_path, "original\n");
  InitializeGitRepo(root);
  CommitAll(root, "base fixture", "base fixture");

  WriteFile(file_path, "on disk change\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, file_path);
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("dirty ");
  WorkspaceShellTestAccess::RefreshGitSidebar(shell);

  WorkspaceShellTestAccess::PrepareDiscardAllGitPrompt(shell);
  Expect(WorkspaceShellTestAccess::PromptSurfaceVisible(shell),
         "discard all should show a prompt before the destructive action");

  WorkspaceShellTestAccess::ConfirmPromptSurface(shell);

  Expect(!WorkspaceShellTestAccess::PromptSurfaceVisible(shell),
         "blocked discard all should close the confirmation prompt");
  Expect(ReadFile(file_path) == "on disk change\n",
         "blocked bulk discard should leave working-tree files untouched");
}

void TestWorkspaceShellDiscardAllGitPromptReconcilesOpenTabs() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  CopyTree(FixturePath("diff/git/base"), root);
  InitializeGitRepo(root);
  CommitAll(root, "base fixture", "base fixture");

  const std::filesystem::path modified = root / "README.md";
  const std::filesystem::path deleted = root / "src/session.cpp";
  const std::filesystem::path staged_added = root / "src/new_panel.cpp";
  const std::filesystem::path untracked = root / "scratch.txt";

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, modified);
  WorkspaceShellTestAccess::OpenFile(shell, deleted);

  WriteFile(modified, ReadFile(modified) + "\nthrowaway change\n");
  std::filesystem::remove(deleted);
  WriteFile(staged_added, "int meaning = 42;\n");
  WriteFile(untracked, "scratch\n");
  WorkspaceShellTestAccess::OpenFile(shell, staged_added);
  WorkspaceShellTestAccess::OpenFile(shell, untracked);
  RequireCommandSuccess("git -C '" + EscapedRepoPath(root) + "' add -A >/dev/null 2>/dev/null",
                        "prepare staged changes");

  WorkspaceShellTestAccess::ShowGitSidebar(shell);
  WorkspaceShellTestAccess::PrepareDiscardAllGitPrompt(shell);
  WorkspaceShellTestAccess::ConfirmPromptSurface(shell);

  const auto find_tab_index = [&](const std::filesystem::path& path) -> std::optional<std::size_t> {
    const auto& tabs = WorkspaceShellTestAccess::OpenTabs(shell);
    const std::filesystem::path normalized = path.lexically_normal();
    for (std::size_t i = 0; i < tabs.size(); ++i) {
      if (tabs[i].path == normalized) {
        return i;
      }
    }
    return std::nullopt;
  };

  const auto modified_tab = find_tab_index(modified);
  const auto deleted_tab = find_tab_index(deleted);

  Expect(!find_tab_index(staged_added).has_value(),
         "bulk discard should close open tabs for removed staged-added files");
  Expect(!find_tab_index(untracked).has_value(),
         "bulk discard should close open tabs for removed untracked files");
  Expect(modified_tab.has_value(),
         "bulk discard should keep tracked modified files open");
  Expect(deleted_tab.has_value(),
         "bulk discard should keep restored tracked files open");

  if (modified_tab.has_value()) {
    WorkspaceShellTestAccess::ActivateTab(shell, *modified_tab);
    const auto& modified_lines = WorkspaceShellTestAccess::ActiveEditor(shell).lines();
    Expect(std::find(modified_lines.begin(), modified_lines.end(), "throwaway change") ==
               modified_lines.end(),
           "bulk discard should reload open modified tabs from the restored file");
  }

  if (deleted_tab.has_value()) {
    WorkspaceShellTestAccess::ActivateTab(shell, *deleted_tab);
    Expect(WorkspaceShellTestAccess::ActiveEditor(shell).path() == deleted.lexically_normal(),
           "bulk discard should leave restored tracked files attached to their editor tabs");
  }
}

void TestWorkspaceShellDeletePromptOnlyClosesAffectedSplitEditor() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  std::filesystem::create_directories(root);
  const std::filesystem::path keep = root / "keep.txt";
  const std::filesystem::path source = root / "delete-me.txt";
  WriteFile(keep, "keep text\n");
  WriteFile(source, "source text\n");

  const std::filesystem::path home = temp_dir.path() / "home";
  const std::filesystem::path xdg_data_home = temp_dir.path() / "xdg-data-home";
  std::filesystem::create_directories(home);
  std::filesystem::create_directories(xdg_data_home);
  ScopedEnvVar scoped_home("HOME", home.string());
  ScopedEnvVar scoped_xdg_data_home("XDG_DATA_HOME", xdg_data_home.string());

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, keep);
  Expect(WorkspaceShellTestAccess::SplitActiveEditor(shell),
         "split delete fixture should open a second editor pane");
  Expect(WorkspaceShellTestAccess::ReplaceActiveEditorWithFile(shell, source),
         "split delete fixture should load the deleted file into the active pane");

  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("saved ");
  Expect(WorkspaceShellTestAccess::ActivateOrderedEditorSplit(shell, 0),
         "split delete fixture should move focus to the first pane");
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("keep ");
  Expect(WorkspaceShellTestAccess::ActivateOrderedEditorSplit(shell, 1),
         "split delete fixture should move focus back to the deleted pane");

  WorkspaceShellTestAccess::PrepareDeletePrompt(shell, source);
  WorkspaceShellTestAccess::ConfirmPromptSurface(shell);

  Expect(WorkspaceShellTestAccess::DirtyPromptVisible(shell),
         "split delete should still prompt for the affected dirty pane");
  Expect(WorkspaceShellTestAccess::DirtyPromptMessage(shell).find("affected dirty editor") !=
             std::string::npos,
         "split delete prompt should describe affected editors instead of tabs");

  WorkspaceShellTestAccess::ConfirmDirtyPrompt(shell, 0);

  Expect(!std::filesystem::exists(source),
         "split delete save flow should remove the deleted project path");
  Expect(WorkspaceShellTestAccess::OpenTabs(shell).size() == 1,
         "split delete save flow should keep the split editor tab open");
  Expect(WorkspaceShellTestAccess::OpenTabs(shell).front().editor_state->views.size() == 1,
         "split delete save flow should remove only the affected split pane");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).path() == keep.lexically_normal(),
         "split delete save flow should activate the remaining pane");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).dirty(),
         "split delete save flow should preserve unrelated dirty panes");
  Expect(ReadFile(keep) == "keep text\n",
         "split delete save flow should not save unrelated dirty panes");

#if defined(__linux__)
  const std::filesystem::path trash_files = xdg_data_home / "Trash" / "files";
#else
  const std::filesystem::path trash_files = home / ".Trash";
#endif
  const auto trashed_file = FirstRegularFileIn(trash_files);
  Expect(trashed_file.has_value(), "split delete save flow should create a trash entry");
  Expect(ReadFile(*trashed_file) == "saved source text\n",
         "split delete save flow should save the affected pane before trashing it");
}
#endif

void TestWorkspaceShellRenamePreservesWorkingTreeCompareState() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "compare.txt";
  WriteFile(source, "zero\none\ntwo\nthree\n");

  InitializeGitRepo(root);
  CommitAll(root, "base fixture", "base fixture");
  WriteFile(source, "zero\none changed\ntwo changed\nthree changed\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "working-tree comparison should open");

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
         "rename should create the working-tree compare destination path");

  const auto& rebuilt = WorkspaceShellTestAccess::ActiveCompare(shell);
  Expect(rebuilt.path == renamed.lexically_normal(),
         "working-tree compare should retarget to the renamed path");
  Expect(rebuilt.left_path == source.lexically_normal(),
         "working-tree compare should keep reading historical content from the original path");
  Expect(rebuilt.right_path == renamed.lexically_normal(),
         "working-tree compare should retarget only the working-tree side to the renamed path");
  Expect(rebuilt.commit_hash == "HEAD",
         "working-tree compare should preserve the left-side ref");
  Expect(rebuilt.right_ref == "WORKTREE",
         "working-tree compare should stay attached to the working tree");
  Expect(rebuilt.left_label == "HEAD",
         "working-tree compare should preserve the left label");
  Expect(rebuilt.right_label == "Working tree",
         "working-tree compare should preserve the right label");
  Expect(rebuilt.selected_row == expected_row,
         "working-tree compare should preserve the selected row when still valid");
  Expect(rebuilt.scroll_row == 2,
         "working-tree compare should preserve vertical scroll on rename");
  Expect(rebuilt.horizontal_scroll == 5,
         "working-tree compare should preserve horizontal scroll on rename");
  const bool kept_compare_content = std::any_of(
      rebuilt.model.rows.begin(), rebuilt.model.rows.end(), [](const auto& row) {
        return row.left_text == "one" && row.right_text == "one changed";
      });
  Expect(kept_compare_content,
         "working-tree compare should preserve the pre-rename commit-vs-working-tree content");
}

void TestWorkspaceShellRenamePreservesBranchCompareSemantics() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "history.txt";
  WriteFile(source, "base line\n");

  InitializeGitRepo(root);
  CommitAll(root, "base fixture", "base fixture");
  WriteFile(source, "head line\n");
  CommitAll(root, "head fixture", "head fixture");

  const auto history = microide::project::CollectGitFileHistory(root, source);
  Expect(history.size() == 2, "branch compare fixture should have two commits");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenBranchHeadComparison(shell, source, history[1].hash, "base",
                                                            history[0].hash, "head"),
         "branch comparison should open");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  compare.scroll_row = 3;
  compare.horizontal_scroll = 7;

  WorkspaceShellTestAccess::PrepareRenamePrompt(shell, source, "history-renamed.txt");
  WorkspaceShellTestAccess::ConfirmPromptSurface(shell);

  const std::filesystem::path renamed = root / "src" / "history-renamed.txt";
  Expect(std::filesystem::is_regular_file(renamed),
         "rename should create the branch compare destination path");

  const auto& rebuilt = WorkspaceShellTestAccess::ActiveCompare(shell);
  Expect(rebuilt.path == renamed.lexically_normal(),
         "branch compare should retarget to the renamed path");
  Expect(rebuilt.left_path == source.lexically_normal(),
         "branch compare should keep the original left-side commit path after rename");
  Expect(rebuilt.right_path == source.lexically_normal(),
         "branch compare should keep the original right-side commit path after rename");
  Expect(rebuilt.commit_hash == history[1].hash,
         "branch compare should preserve the left-side ref");
  Expect(rebuilt.right_ref == history[0].hash,
         "branch compare should preserve the right-side ref");
  Expect(rebuilt.left_label == "base",
         "branch compare should preserve the left label");
  Expect(rebuilt.right_label == "head",
         "branch compare should preserve the right label");
  Expect(rebuilt.persistable,
         "branch compare should remain session-persistable after rename");
  Expect(rebuilt.scroll_row == 3,
         "branch compare should preserve vertical scroll on rename");
  Expect(rebuilt.horizontal_scroll == 7,
         "branch compare should preserve horizontal scroll on rename");
  const bool kept_compare_content = std::any_of(
      rebuilt.model.rows.begin(), rebuilt.model.rows.end(), [](const auto& row) {
        return row.left_text == "base line" && row.right_text == "head line";
      });
  Expect(kept_compare_content,
         "branch compare should preserve the original commit content after rename");
}

void TestWorkspaceShellRenamePreservesMergeTabState() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path base = root / "base.txt";
  const std::filesystem::path incoming = root / "incoming.txt";
  const std::filesystem::path current = root / "current.txt";
  const std::filesystem::path output = root / "result.txt";
  WriteFile(base,
            "alpha long content 0123456789 abcdefghijklmnopqrstuvwxyz alpha long content "
            "0123456789 abcdefghijklmnopqrstuvwxyz\none\ntwo\nthree\nfour\nshared\nsix\n");
  WriteFile(incoming,
            "incoming long content 0123456789 abcdefghijklmnopqrstuvwxyz incoming long content "
            "0123456789 abcdefghijklmnopqrstuvwxyz\none\ntwo\nthree\nfour\nshared\nsix\n");
  WriteFile(current,
            "current long content 0123456789 abcdefghijklmnopqrstuvwxyz current long content "
            "0123456789 abcdefghijklmnopqrstuvwxyz\none\ntwo\nthree\nfour\nshared\nsix\n");
  WriteFile(output,
            "alpha long content 0123456789 abcdefghijklmnopqrstuvwxyz alpha long content "
            "0123456789 abcdefghijklmnopqrstuvwxyz\none\ntwo\nthree\nfour\nshared\nsix\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, current, output),
         "merge editor should open");

  auto& merge = WorkspaceShellTestAccess::ActiveMerge(shell);
  const std::size_t expected_hunk = 0;
  merge.selected_hunk = expected_hunk;
  merge.left_divider_fraction = 0.28f;
  merge.right_divider_fraction = 0.76f;
  merge.result_viewport.MoveCursorTo(5, 0);
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "manual "),
         "rename merge fixture should allow manual result edits before rename");
  merge.scroll_row = 4;
  merge.horizontal_scroll = 6;
  merge.result_viewport.SetScrollLine(4);
  merge.result_viewport.SetHorizontalScroll(6);

  WorkspaceShellTestAccess::PrepareRenamePrompt(shell, output, "resolved.txt");
  WorkspaceShellTestAccess::ConfirmPromptSurface(shell);
  if (WorkspaceShellTestAccess::DirtyPromptVisible(shell)) {
    WorkspaceShellTestAccess::ConfirmDirtyPrompt(shell, 0);
  }

  const std::filesystem::path renamed = root / "resolved.txt";
  Expect(std::filesystem::is_regular_file(renamed),
         "rename should create the merge output destination path");

  const auto& rebuilt = WorkspaceShellTestAccess::ActiveMerge(shell);
  Expect(rebuilt.output_path == renamed.lexically_normal(),
         "merge tab should retarget the output path on rename");
  Expect(rebuilt.selected_hunk == expected_hunk,
         "merge tab should preserve the selected hunk on rename");
  Expect(rebuilt.scroll_row == 4,
         "merge tab should preserve vertical scroll on rename");
  Expect(rebuilt.horizontal_scroll == 6,
         "merge tab should preserve horizontal scroll on rename");
  Expect(std::fabs(rebuilt.left_divider_fraction - 0.28f) < 0.0001f,
         "merge tab should preserve the left divider fraction on rename");
  Expect(std::fabs(rebuilt.right_divider_fraction - 0.76f) < 0.0001f,
         "merge tab should preserve the right divider fraction on rename");
  Expect(rebuilt.persistable,
         "merge tab should preserve its persistable flag on rename");
  Expect(rebuilt.result_viewport.lines()[5] == "manual shared",
         "merge tab should preserve the live result buffer across rename");
}

void TestWorkspaceShellQuitPromptSavesDirtyTabsAcrossProjectsAndRestoresActiveProject() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root_a = temp_dir.path() / "alpha-project";
  const std::filesystem::path root_b = temp_dir.path() / "beta-project";
  const std::filesystem::path file_a = root_a / "alpha.txt";
  const std::filesystem::path file_b = root_b / "beta.txt";
  WriteFile(file_a, "alpha\n");
  WriteFile(file_b, "beta\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root_a, false, false),
         "first project should open");
  WorkspaceShellTestAccess::OpenFile(shell, file_a);
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("saved ");

  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root_b, false, false),
         "second project should open");
  WorkspaceShellTestAccess::OpenFile(shell, file_b);
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("saved ");

  WorkspaceShellTestAccess::RequestQuit(shell);

  Expect(WorkspaceShellTestAccess::DirtyPromptVisible(shell),
         "quit with dirty tabs across projects should show the dirty prompt");
  WorkspaceShellTestAccess::ConfirmDirtyPrompt(shell, 0);

  Expect(WorkspaceShellTestAccess::ProjectRoot(shell) == root_b.lexically_normal(),
         "save-all quit should restore the originally active project before quitting");
  Expect(ReadFile(file_a) == "saved alpha\n",
         "save-all quit should persist dirty tabs from the first project");
  Expect(ReadFile(file_b) == "saved beta\n",
         "save-all quit should persist dirty tabs from the active project");
  Expect(WorkspaceShellTestAccess::ConsumeQuitRequested(shell),
         "save-all quit should set the pending quit request after saving");
}

void TestWorkspaceShellCloseInactiveDirtyProjectPreservesOriginalActiveProject() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root_a = temp_dir.path() / "alpha-project";
  const std::filesystem::path root_b = temp_dir.path() / "beta-project";
  const std::filesystem::path root_c = temp_dir.path() / "gamma-project";
  const std::filesystem::path file_a = root_a / "alpha.txt";
  WriteFile(file_a, "alpha\n");
  WriteFile(root_b / "beta.txt", "beta\n");
  WriteFile(root_c / "gamma.txt", "gamma\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root_a, false, false),
         "first project should open");
  WorkspaceShellTestAccess::OpenFile(shell, file_a);
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("saved ");
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root_b, false, false),
         "second project should open");
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root_c, false, false),
         "third project should open");

  WorkspaceShellTestAccess::RequestCloseProject(shell, 0);
  Expect(WorkspaceShellTestAccess::DirtyPromptVisible(shell),
         "closing an inactive dirty project should show the dirty prompt");

  WorkspaceShellTestAccess::ConfirmDirtyPrompt(shell, 0);

  Expect(WorkspaceShellTestAccess::ProjectRoots(shell) ==
             std::vector<std::filesystem::path>{root_b.lexically_normal(),
                                                root_c.lexically_normal()},
         "closing an inactive dirty project should remove only the requested project");
  Expect(WorkspaceShellTestAccess::ProjectRoot(shell) == root_c.lexically_normal(),
         "closing an inactive dirty project should restore the original active project");
  Expect(ReadFile(file_a) == "saved alpha\n",
         "closing an inactive dirty project should save that project's dirty tabs first");
}

void TestWorkspaceShellLargeFileBreadcrumbLabel() {
  WorkspaceShell shell;
  const std::filesystem::path project_root = FixturePath("large");
  const std::filesystem::path file_path = FixturePath("large/code/large_sample.cpp");
  WorkspaceShellTestAccess::SetProjectRoot(shell, project_root);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, file_path);

  const std::string breadcrumb = WorkspaceShellTestAccess::BreadcrumbLabel(shell);
  Expect(breadcrumb.find("large file mode") != std::string::npos,
         "large file editors should surface the mode in the breadcrumb");
}

}  // namespace

void RegisterWorkspaceShellPromptTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceShell/RenamePromptSavesDirtyTabs",
          TestWorkspaceShellRenamePromptSavesDirtyTabs);
  AddTest(tests, "WorkspaceShell/RenamePromptOnlySavesAffectedSplitEditor",
          TestWorkspaceShellRenamePromptOnlySavesAffectedSplitEditor);
  AddTest(tests, "WorkspaceShell/RenamePreservesWorkingTreeCompareState",
          TestWorkspaceShellRenamePreservesWorkingTreeCompareState);
  AddTest(tests, "WorkspaceShell/RenamePreservesBranchCompareSemantics",
          TestWorkspaceShellRenamePreservesBranchCompareSemantics);
  AddTest(tests, "WorkspaceShell/RenamePreservesMergeTabState",
          TestWorkspaceShellRenamePreservesMergeTabState);
  AddTest(tests, "WorkspaceShell/QuitPromptSavesDirtyTabsAcrossProjectsAndRestoresActiveProject",
          TestWorkspaceShellQuitPromptSavesDirtyTabsAcrossProjectsAndRestoresActiveProject);
  AddTest(tests, "WorkspaceShell/CloseInactiveDirtyProjectPreservesOriginalActiveProject",
          TestWorkspaceShellCloseInactiveDirtyProjectPreservesOriginalActiveProject);
  AddTest(tests, "WorkspaceShell/LargeFileBreadcrumbLabel",
          TestWorkspaceShellLargeFileBreadcrumbLabel);
#if defined(__linux__) || defined(__APPLE__)
  AddTest(tests, "WorkspaceShell/DeletePromptDiscardsDirtyTabs",
          TestWorkspaceShellDeletePromptDiscardsDirtyTabs);
  AddTest(tests, "WorkspaceShell/DiscardAllGitPromptDiscardsWorkingTreeChanges",
          TestWorkspaceShellDiscardAllGitPromptDiscardsWorkingTreeChanges);
  AddTest(tests, "WorkspaceShell/DiscardAllGitPromptBlocksDirtyEditors",
          TestWorkspaceShellDiscardAllGitPromptBlocksDirtyEditors);
  AddTest(tests, "WorkspaceShell/DiscardAllGitPromptReconcilesOpenTabs",
          TestWorkspaceShellDiscardAllGitPromptReconcilesOpenTabs);
  AddTest(tests, "WorkspaceShell/DeletePromptOnlyClosesAffectedSplitEditor",
          TestWorkspaceShellDeletePromptOnlyClosesAffectedSplitEditor);
#endif
}

}  // namespace microide::tests
