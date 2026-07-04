#include "TestSupport.h"

#include "project/GitCompareService.h"
#include "workspace/LaunchConfig.h"
#include "workspace/WorkspaceShellTestAccess.h"

#include <algorithm>
#include <chrono>
#include <cmath>
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

void TestWorkspaceShellRenamePromptRetargetsDiagnostics() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  std::filesystem::create_directories(root);
  const std::filesystem::path source = root / "notes.txt";
  WriteFile(source, "alpha\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, source);
  Expect(WorkspaceShellTestAccess::PublishDiagnostics(
             shell, "linter", source,
             {microide::editor::Diagnostic{
                 .range =
                     microide::editor::SelectionRange{
                         .start = microide::editor::TextPosition{.line = 0, .column = 0},
                         .end = microide::editor::TextPosition{.line = 0, .column = 5},
                     },
                 .severity = microide::editor::DiagnosticSeverity::Warning,
                 .message = "rename warning",
             }}),
         "rename diagnostics fixture should publish a diagnostic for the source file");

  WorkspaceShellTestAccess::ShowProblemsSidebar(shell);
  Expect(WorkspaceShellTestAccess::ProblemsSidebarEntries(shell).size() == 1 &&
             WorkspaceShellTestAccess::ProblemsSidebarEntries(shell).front().detail_label ==
                 "notes.txt:1:1 | linter",
         "rename diagnostics fixture should expose the original problems entry");

  WorkspaceShellTestAccess::PrepareRenamePrompt(shell, source, "renamed.txt");
  WorkspaceShellTestAccess::ConfirmPromptSurface(shell);

  const std::filesystem::path renamed = root / "renamed.txt";
  Expect(std::filesystem::is_regular_file(renamed),
         "rename diagnostics flow should create the destination path");
  Expect(WorkspaceShellTestAccess::DiagnosticsForPath(shell, source) == nullptr,
         "rename diagnostics flow should remove diagnostics from the old path");
  const auto* renamed_diagnostics = WorkspaceShellTestAccess::DiagnosticsForPath(shell, renamed);
  Expect(renamed_diagnostics != nullptr && renamed_diagnostics->size() == 1 &&
             renamed_diagnostics->front().path == renamed.lexically_normal() &&
             renamed_diagnostics->front().message == "rename warning",
         "rename diagnostics flow should retarget diagnostics to the new path");
  Expect(WorkspaceShellTestAccess::ProblemsSidebarEntries(shell).size() == 1 &&
             WorkspaceShellTestAccess::ProblemsSidebarEntries(shell).front().detail_label ==
                 "renamed.txt:1:1 | linter",
         "rename diagnostics flow should refresh the Problems sidebar entry");
}

void TestWorkspaceShellRenamePromptPasteShortcutUsesSharedTextInputPath() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "notes.txt";
  WriteFile(source, "alpha\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, source);
  WorkspaceShellTestAccess::PrepareRenamePrompt(shell, source, "");
  Expect(WorkspaceShellTestAccess::PromptSurfaceVisible(shell),
         "rename prompt paste fixture should open the rename prompt");

  WorkspaceShellTestAccess::SetClipboardTextReader(
      shell, []() -> std::optional<std::string> { return std::string("renamed.txt"); });

  Expect(SendKeyDown(shell, SDLK_V, SDL_KMOD_CTRL),
         "Ctrl+V should be handled by the rename prompt");
  Expect(WorkspaceShellTestAccess::PromptSurfaceInput(shell) == "renamed.txt",
         "Ctrl+V should route clipboard text through the shared prompt text-input path");
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

void TestWorkspaceShellDeletePromptClearsDiagnostics() {
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
  Expect(WorkspaceShellTestAccess::PublishDiagnostics(
             shell, "linter", source,
             {microide::editor::Diagnostic{
                 .range =
                     microide::editor::SelectionRange{
                         .start = microide::editor::TextPosition{.line = 0, .column = 0},
                         .end = microide::editor::TextPosition{.line = 0, .column = 7},
                     },
                 .severity = microide::editor::DiagnosticSeverity::Error,
                 .message = "delete error",
             }}),
         "delete diagnostics fixture should publish a diagnostic for the doomed file");

  WorkspaceShellTestAccess::ShowProblemsSidebar(shell);
  Expect(WorkspaceShellTestAccess::ProblemsSidebarEntries(shell).size() == 1,
         "delete diagnostics fixture should expose one Problems entry before deletion");

  WorkspaceShellTestAccess::PrepareDeletePrompt(shell, source);
  WorkspaceShellTestAccess::ConfirmPromptSurface(shell);

  Expect(!std::filesystem::exists(source),
         "delete diagnostics flow should remove the doomed path");
  Expect(WorkspaceShellTestAccess::DiagnosticsForPath(shell, source) == nullptr,
         "delete diagnostics flow should clear diagnostics for the deleted path");
  Expect(WorkspaceShellTestAccess::ProblemsSidebarEntries(shell).empty(),
         "delete diagnostics flow should remove stale Problems entries");
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
  RequireGitCommandSuccess(root, {"add", "-A"}, "prepare staged changes");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::ShowGitSidebar(shell);
  {
    const auto git_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < git_deadline &&
           WorkspaceShellTestAccess::GitSidebarRefreshing(shell)) {
      WorkspaceShellTestAccess::ConsumeGitSidebarRefresh(shell);
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
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
  {
    const auto git_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < git_deadline &&
           WorkspaceShellTestAccess::GitSidebarRefreshing(shell)) {
      WorkspaceShellTestAccess::ConsumeGitSidebarRefresh(shell);
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

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
  RequireGitCommandSuccess(root, {"add", "-A"}, "prepare staged changes");

  WorkspaceShellTestAccess::ShowGitSidebar(shell);
  {
    const auto git_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < git_deadline &&
           WorkspaceShellTestAccess::GitSidebarRefreshing(shell)) {
      WorkspaceShellTestAccess::ConsumeGitSidebarRefresh(shell);
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
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
    const auto& modified_lines = WorkspaceShellTestAccess::ActiveEditor(shell).lines().Snapshot();
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

  const auto history = microide::project::CollectGitFileHistory(root, source).commits;
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

void TestWorkspaceShellQuitDoesNotPromptForDirtyTabs() {
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

  shell.RequestQuit();

  Expect(!WorkspaceShellTestAccess::DirtyPromptVisible(shell),
         "quit with dirty tabs should not show the dirty prompt");
  Expect(ReadFile(file_a) == "alpha\n",
         "quit should not flush unsaved file-backed changes to disk");
  Expect(ReadFile(file_b) == "beta\n",
         "quit should leave file-backed dirty tabs unsaved on disk");
  Expect(shell.ConsumeQuitRequested(),
         "quit should set the pending quit request immediately");
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

// Quit-with-save must persist dirty tabs across every project, including inactive
// ones, while the optimized path skips switching into clean projects. This guards
// the DirtyIndicesForProject-based skip in ConfirmQuit against wrongly dropping a
// dirty inactive project (the one behavior the skip could regress).
void TestWorkspaceShellQuitWithSavePersistsInactiveDirtyProject() {
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
  // Open a second, clean project so project A becomes inactive+dirty and B is the
  // active+clean project the quit path should not need to re-save.
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root_b, false, false),
         "second project should open");

  WorkspaceShellTestAccess::ShowDirtyPromptForQuit(shell);
  Expect(WorkspaceShellTestAccess::DirtyPromptVisible(shell),
         "quitting with an inactive dirty project should show the quit dirty prompt");

  WorkspaceShellTestAccess::ConfirmDirtyPrompt(shell, 0);

  Expect(!WorkspaceShellTestAccess::DirtyPromptVisible(shell),
         "confirming save-and-quit should dismiss the dirty prompt");
  Expect(ReadFile(file_a) == "saved alpha\n",
         "save-and-quit should persist the inactive project's dirty tab");
  Expect(ReadFile(file_b) == "beta\n",
         "save-and-quit should leave the clean project's file untouched");
  Expect(WorkspaceShellTestAccess::ProjectRoot(shell) == root_b.lexically_normal(),
         "save-and-quit should restore the originally active project");
  Expect(shell.ConsumeQuitRequested(),
         "save-and-quit should set the pending quit request");
}

void TestWorkspaceShellEditorBreadcrumbUsesRelativePathForLargeFixtures() {
  WorkspaceShell shell;
  const std::filesystem::path project_root = FixturePath("large");
  const std::filesystem::path file_path = FixturePath("large/code/large_sample.cpp");
  WorkspaceShellTestAccess::SetProjectRoot(shell, project_root);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, file_path);

  const std::string breadcrumb = WorkspaceShellTestAccess::BreadcrumbLabel(shell);
  Expect(breadcrumb == "code/large_sample.cpp",
         "editor breadcrumbs should no longer append a large-file mode marker");
}

void TestWorkspaceShellRenamePromptMouseClickPositionsCaret() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  std::filesystem::create_directories(root);
  const std::filesystem::path source = root / "notes.txt";
  WriteFile(source, "base\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, source);

  WorkspaceShellTestAccess::PrepareRenamePrompt(shell, source, "renamed-long-name.txt");
  Expect(WorkspaceShellTestAccess::PromptSurfaceVisible(shell),
         "rename prompt should be visible after setup");
  Expect(WorkspaceShellTestAccess::PromptSurfaceInput(shell) == "renamed-long-name.txt",
         "rename prompt should hold the proposed name");
  const std::size_t initial_caret = WorkspaceShellTestAccess::PromptSurfaceInputCaret(shell);
  Expect(initial_caret == std::string_view("renamed-long-name.txt").size(),
         "caret should default to end of text after PrepareRenamePrompt");

  const SDL_FRect input_rect = WorkspaceShellTestAccess::PromptSurfaceInputRect(shell);
  // Click near the left edge of the input — caret should move to the start area.
  const float left_x = input_rect.x + 8.0f;
  const float mid_y = input_rect.y + input_rect.h * 0.5f;
  Expect(SendMouseDown(shell, left_x, mid_y, SDL_BUTTON_LEFT, 1),
         "mouse-down in prompt input should be handled");
  const std::size_t after_click_caret =
      WorkspaceShellTestAccess::PromptSurfaceInputCaret(shell);
  Expect(after_click_caret < initial_caret,
         "click near left edge should move caret toward start");
  Expect(!WorkspaceShellTestAccess::PromptSurfaceInputSelection(shell).has_value(),
         "single click should not create a selection");
  Expect(SendMouseUp(shell, left_x, mid_y, SDL_BUTTON_LEFT),
         "mouse-up after click in prompt input should be handled");

  // Double-click selects a word.
  const float double_click_x = input_rect.x + 20.0f;
  Expect(SendMouseDown(shell, double_click_x, mid_y, SDL_BUTTON_LEFT, 2),
         "double-click in prompt input should be handled");
  const auto selection = WorkspaceShellTestAccess::PromptSurfaceInputSelection(shell);
  Expect(selection.has_value(), "double-click should create a word selection");
  Expect(SendMouseUp(shell, double_click_x, mid_y, SDL_BUTTON_LEFT),
         "mouse-up after double-click should be handled");

  // Triple-click selects everything.
  Expect(SendMouseDown(shell, double_click_x, mid_y, SDL_BUTTON_LEFT, 3),
         "triple-click in prompt input should be handled");
  const auto triple_selection = WorkspaceShellTestAccess::PromptSurfaceInputSelection(shell);
  Expect(triple_selection.has_value() && triple_selection->first == 0 &&
             triple_selection->second == WorkspaceShellTestAccess::PromptSurfaceInput(shell).size(),
         "triple-click should select the entire input");
  Expect(SendMouseUp(shell, double_click_x, mid_y, SDL_BUTTON_LEFT),
         "mouse-up after triple-click should be handled");
}

// Phase 9 launch-config picker: opening seeds one row per launch config with
// prebuilt name/type labels; the query filters case-insensitively across both
// columns; confirming a match persists its index as the selected launch config.
void TestWorkspaceShellLaunchConfigPicker() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  std::filesystem::create_directories(root);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  std::vector<microide::workspace::LaunchConfig> configs;
  configs.push_back({.name = "Run pytest", .type = "debugpy", .request = "launch"});
  configs.push_back({.name = "Attach to server", .type = "debugpy", .request = "attach"});
  configs.push_back({.name = "Debug binary", .type = "lldb", .request = "launch"});
  WorkspaceShellTestAccess::SeedLaunchConfigs(shell, configs);

  WorkspaceShellTestAccess::OpenLaunchConfigPicker(shell);
  Expect(WorkspaceShellTestAccess::LaunchConfigPickerMatchLabels(shell).size() == 3,
         "opening the picker should list every launch config");

  // Filter by adapter type (secondary column) — matches both debugpy configs.
  WorkspaceShellTestAccess::SetLaunchConfigPickerQuery(shell, "debugpy");
  Expect(WorkspaceShellTestAccess::LaunchConfigPickerMatchLabels(shell).size() == 2,
         "the query should filter across the type column");

  // Filter by name (primary column, case-insensitive) down to one match.
  WorkspaceShellTestAccess::SetLaunchConfigPickerQuery(shell, "ATTACH");
  const auto labels = WorkspaceShellTestAccess::LaunchConfigPickerMatchLabels(shell);
  Expect(labels.size() == 1 && labels[0] == "Attach to server",
         "the query should match the name column case-insensitively");

  // Confirming the sole match persists its original config index (1) even though
  // launching fails here (no adapter registered) — the selection is written first.
  WorkspaceShellTestAccess::SelectAndConfirmLaunchConfig(shell, 0);
  Expect(WorkspaceShellTestAccess::SelectedLaunchConfigIndex(shell) == 1,
         "confirming a filtered match persists the underlying launch-config index");
}

void TestWorkspaceShellClosingNonActiveDirtyTabDoesNotStrandFocus() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  std::filesystem::create_directories(root);
  const std::filesystem::path file_a = root / "a.txt";
  const std::filesystem::path file_b = root / "b.txt";
  WriteFile(file_a, "alpha\n");
  WriteFile(file_b, "beta\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, file_a);
  // Dirty tab 0 while it is active, then open (and switch to) tab 1 so tab 0 is a
  // *non-active* dirty tab.
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("edited ");
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file_b),
         "second tab should open");
  Expect(WorkspaceShellTestAccess::OpenTabs(shell).size() == 2, "two tabs should be open");
  Expect(WorkspaceShellTestAccess::ActiveTabIndex(shell) == 1, "tab 1 should be active");
  Expect(WorkspaceShellTestAccess::FocusIsEditor(shell), "editor should hold focus");

  // Close the non-active dirty tab; the dirty prompt appears with focus on the
  // overlay.
  WorkspaceShellTestAccess::RequestCloseTab(shell, 0);
  Expect(WorkspaceShellTestAccess::DirtyPromptVisible(shell),
         "closing a dirty tab should raise the dirty prompt");
  Expect(WorkspaceShellTestAccess::FocusIsOverlay(shell),
         "the dirty prompt should take focus while visible");

  // Discard (Don't Save = action 1) and close the tab.
  WorkspaceShellTestAccess::ConfirmDirtyPrompt(shell, 1);

  Expect(!WorkspaceShellTestAccess::DirtyPromptVisible(shell),
         "confirming should dismiss the dirty prompt");
  Expect(WorkspaceShellTestAccess::OpenTabs(shell).size() == 1,
         "the non-active tab should be closed");
  // Regression: focus must not remain stranded on the now-hidden overlay handler,
  // which would silently swallow every keystroke until the user clicked a surface.
  Expect(!WorkspaceShellTestAccess::FocusIsOverlay(shell),
         "closing a non-active dirty tab must not strand focus on the hidden overlay");
  Expect(WorkspaceShellTestAccess::FocusIsEditor(shell),
         "focus should return to the editor after the prompt closes");
}

}  // namespace

void RegisterWorkspaceShellPromptTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceShell/ClosingNonActiveDirtyTabDoesNotStrandFocus",
          TestWorkspaceShellClosingNonActiveDirtyTabDoesNotStrandFocus);
  AddTest(tests, "WorkspaceShell/RenamePromptSavesDirtyTabs",
          TestWorkspaceShellRenamePromptSavesDirtyTabs);
  AddTest(tests, "WorkspaceShell/RenamePromptMouseClickPositionsCaret",
          TestWorkspaceShellRenamePromptMouseClickPositionsCaret);
  AddTest(tests, "WorkspaceShell/RenamePromptRetargetsDiagnostics",
          TestWorkspaceShellRenamePromptRetargetsDiagnostics);
  AddTest(tests, "WorkspaceShell/RenamePromptPasteShortcutUsesSharedTextInputPath",
          TestWorkspaceShellRenamePromptPasteShortcutUsesSharedTextInputPath);
  AddTest(tests, "WorkspaceShell/RenamePreservesWorkingTreeCompareState",
          TestWorkspaceShellRenamePreservesWorkingTreeCompareState);
  AddTest(tests, "WorkspaceShell/RenamePreservesBranchCompareSemantics",
          TestWorkspaceShellRenamePreservesBranchCompareSemantics);
  AddTest(tests, "WorkspaceShell/RenamePreservesMergeTabState",
          TestWorkspaceShellRenamePreservesMergeTabState);
  AddTest(tests, "WorkspaceShell/QuitDoesNotPromptForDirtyTabs",
          TestWorkspaceShellQuitDoesNotPromptForDirtyTabs);
  AddTest(tests, "WorkspaceShell/CloseInactiveDirtyProjectPreservesOriginalActiveProject",
          TestWorkspaceShellCloseInactiveDirtyProjectPreservesOriginalActiveProject);
  AddTest(tests, "WorkspaceShell/QuitWithSavePersistsInactiveDirtyProject",
          TestWorkspaceShellQuitWithSavePersistsInactiveDirtyProject);
  AddTest(tests, "WorkspaceShell/EditorBreadcrumbUsesRelativePathForLargeFixtures",
          TestWorkspaceShellEditorBreadcrumbUsesRelativePathForLargeFixtures);
#if defined(__linux__) || defined(__APPLE__)
  AddTest(tests, "WorkspaceShell/DeletePromptDiscardsDirtyTabs",
          TestWorkspaceShellDeletePromptDiscardsDirtyTabs);
  AddTest(tests, "WorkspaceShell/DeletePromptClearsDiagnostics",
          TestWorkspaceShellDeletePromptClearsDiagnostics);
  AddTest(tests, "WorkspaceShell/DiscardAllGitPromptDiscardsWorkingTreeChanges",
          TestWorkspaceShellDiscardAllGitPromptDiscardsWorkingTreeChanges);
  AddTest(tests, "WorkspaceShell/DiscardAllGitPromptBlocksDirtyEditors",
          TestWorkspaceShellDiscardAllGitPromptBlocksDirtyEditors);
  AddTest(tests, "WorkspaceShell/DiscardAllGitPromptReconcilesOpenTabs",
          TestWorkspaceShellDiscardAllGitPromptReconcilesOpenTabs);
  AddTest(tests, "WorkspaceShell/LaunchConfigPicker", TestWorkspaceShellLaunchConfigPicker);
#endif
}

}  // namespace microide::tests
