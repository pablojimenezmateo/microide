#include "TestSupport.h"

#include "workspace/WorkspaceShell.h"
#include "workspace/WorkspaceShellTestAccess.h"

#include <chrono>
#include <thread>

namespace microide::tests {
namespace {

using microide::workspace::WorkspaceShell;
using WorkspaceShellTestAccess = microide::workspace::WorkspaceShell::TestAccess;

bool WaitForProjectReload(WorkspaceShell& shell, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, false)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

void TestWorkspaceShellExternalChangeReloadsCleanBuffer() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file_path = root / "notes.txt";
  WriteFile(file_path, "clean\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "clean reload fixture should open the project");
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, file_path);
  for (int attempt = 0; attempt < 20; ++attempt) {
    if (!WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, false)) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  WriteFile(file_path, "clean updated\n");
  Expect(WaitForProjectReload(shell, std::chrono::seconds(1)),
         "external file change should trigger a project reload");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).lines()[0] == "clean updated",
         "clean buffers should reload from disk after an external change");
}

// Drains any pending project-change events so the watcher baseline is settled.
void DrainProjectChanges(WorkspaceShell& shell) {
  for (int attempt = 0; attempt < 20; ++attempt) {
    if (!WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, false)) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

bool WaitForExternalChangeBanner(WorkspaceShell& shell,
                                 const std::filesystem::path& path,
                                 std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (WorkspaceShellTestAccess::HasExternalChangeBanner(shell, path)) {
      return true;
    }
    WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return WorkspaceShellTestAccess::HasExternalChangeBanner(shell, path);
}

void TestWorkspaceShellExternalChangeBannerForDirtyBuffer() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file_path = root / "notes.txt";
  WriteFile(file_path, "original\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "dirty external-change fixture should open the project");
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, file_path);
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("dirty ");
  DrainProjectChanges(shell);

  WriteFile(file_path, "on disk\n");
  Expect(WaitForExternalChangeBanner(shell, file_path, std::chrono::seconds(1)),
         "dirty buffers should raise a non-blocking external-change banner");
  Expect(!WorkspaceShellTestAccess::DirtyPromptVisible(shell),
         "external changes should no longer raise a blocking modal prompt");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).lines()[0].starts_with("dirty "),
         "dirty buffers should keep in-memory edits until the user acts");
}

void TestWorkspaceShellSelfWriteDoesNotRaiseBanner() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file_path = root / "notes.txt";
  WriteFile(file_path, "original\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "self-write fixture should open the project");
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, file_path);
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("mine ");
  DrainProjectChanges(shell);

  Expect(WorkspaceShellTestAccess::SaveTab(shell, WorkspaceShellTestAccess::ActiveTabIndex(shell)),
         "saving a buffer with no external change should succeed");
  // Pump the watcher: its echo of our own write must be recognized by signature
  // and produce no banner (neither external-change nor reloaded notice).
  for (int attempt = 0; attempt < 20; ++attempt) {
    WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  Expect(WorkspaceShellTestAccess::EditorBannerCount(shell) == 0,
         "the editor's own save must not raise any banner");
  Expect(ReadFile(file_path) == "mine original\n",
         "the saved file should contain the in-memory edits");
}

void TestWorkspaceShellSaveTimeConflictGuardBlocksClobber() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file_path = root / "notes.txt";
  WriteFile(file_path, "original\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "conflict-guard fixture should open the project");
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, file_path);
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("dirty ");
  DrainProjectChanges(shell);

  // External writer changes the file. We deliberately do NOT pump the watcher,
  // simulating a missed event; the save-time guard must still refuse to clobber.
  WriteFile(file_path, "newer on disk\n");
  Expect(!WorkspaceShellTestAccess::SaveTab(shell, WorkspaceShellTestAccess::ActiveTabIndex(shell)),
         "save must fail when the file changed on disk since load");
  Expect(ReadFile(file_path) == "newer on disk\n",
         "the save-time guard must not overwrite the newer on-disk content");
  Expect(WorkspaceShellTestAccess::HasExternalChangeBanner(shell, file_path),
         "a blocked save should raise the external-change banner");
}

// #6: the after-delay autosave flush must honor the same disk-conflict guard as a manual
// save. If the file changed on disk since load, an autosave firing (e.g. debounce elapsed
// while the user was away) must NOT silently overwrite the external change -- it routes
// through SaveTab, so it refuses and raises the banner instead of clobbering.
void TestWorkspaceShellAutosaveFlushRespectsDiskConflict() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file_path = root / "notes.txt";
  WriteFile(file_path, "original\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "autosave-conflict fixture should open the project");
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, file_path);
  // Set the mode AFTER opening: OpenProjectTab loads the project/user config, which would
  // otherwise reset editor.autosave back to its default.
  Expect(WorkspaceShellTestAccess::SetSettingValue(shell, "editor.autosave", "after_delay"),
         "after_delay autosave should be settable");
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("dirty ");
  DrainProjectChanges(shell);

  // External writer changes the file; the watcher event is deliberately missed.
  WriteFile(file_path, "newer on disk\n");

  // Fire the autosave flush directly (as the debounce wake would).
  WorkspaceShellTestAccess::MaybeAutosaveDirtyTabs(shell, /*on_focus_change=*/false);

  Expect(ReadFile(file_path) == "newer on disk\n",
         "an autosave flush must not overwrite an external change (no silent clobber)");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).dirty(),
         "the buffer must stay dirty when autosave is blocked by a disk conflict");
  Expect(WorkspaceShellTestAccess::HasExternalChangeBanner(shell, file_path),
         "a blocked autosave should raise the external-change banner just like a manual save");
}

void TestWorkspaceShellBannerOverwriteWritesInMemoryEdits() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file_path = root / "notes.txt";
  WriteFile(file_path, "original\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "overwrite fixture should open the project");
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, file_path);
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("dirty ");
  DrainProjectChanges(shell);
  WriteFile(file_path, "newer on disk\n");
  Expect(WaitForExternalChangeBanner(shell, file_path, std::chrono::seconds(1)),
         "overwrite fixture should reach the external-change banner");

  WorkspaceShellTestAccess::EditorBannerOverwrite(shell, file_path);
  Expect(ReadFile(file_path) == "dirty original\n",
         "Overwrite should write the in-memory edits over the disk content");
  Expect(WorkspaceShellTestAccess::EditorBannerCount(shell) == 0,
         "Overwrite should clear the banner");
  Expect(!WorkspaceShellTestAccess::ActiveEditor(shell).dirty(),
         "Overwrite should leave the buffer clean after saving");
}

void TestWorkspaceShellBannerReloadReplacesBuffer() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file_path = root / "notes.txt";
  WriteFile(file_path, "original\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "reload fixture should open the project");
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, file_path);
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("dirty ");
  DrainProjectChanges(shell);
  WriteFile(file_path, "newer on disk\n");
  Expect(WaitForExternalChangeBanner(shell, file_path, std::chrono::seconds(1)),
         "reload fixture should reach the external-change banner");

  WorkspaceShellTestAccess::EditorBannerReload(shell, file_path);
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).lines()[0] == "newer on disk",
         "Reload should replace the buffer with the on-disk content");
  Expect(WorkspaceShellTestAccess::EditorBannerCount(shell) == 0,
         "Reload should clear the banner");
}

void TestWorkspaceShellBannerKeepPreservesBoth() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file_path = root / "notes.txt";
  WriteFile(file_path, "original\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "keep fixture should open the project");
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, file_path);
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("dirty ");
  DrainProjectChanges(shell);
  WriteFile(file_path, "newer on disk\n");
  Expect(WaitForExternalChangeBanner(shell, file_path, std::chrono::seconds(1)),
         "keep fixture should reach the external-change banner");

  WorkspaceShellTestAccess::EditorBannerKeep(shell, file_path);
  Expect(WorkspaceShellTestAccess::EditorBannerCount(shell) == 0,
         "Keep should dismiss the banner");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).lines()[0].starts_with("dirty "),
         "Keep should preserve the in-memory edits");
  Expect(ReadFile(file_path) == "newer on disk\n",
         "Keep should leave the on-disk content untouched");
}

void TestWorkspaceShellCleanReloadRaisesNotice() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file_path = root / "notes.txt";
  WriteFile(file_path, "clean\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "clean-notice fixture should open the project");
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, file_path);
  DrainProjectChanges(shell);

  WriteFile(file_path, "clean updated\n");
  Expect(WaitForProjectReload(shell, std::chrono::seconds(1)),
         "external change to a clean buffer should trigger a reload");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).lines()[0] == "clean updated",
         "clean buffers should silently reload from disk");
  Expect(WorkspaceShellTestAccess::HasReloadedNoticeBanner(shell, file_path),
         "a silent clean reload should surface a passive reloaded-from-disk notice");
}

void TestWorkspaceShellExternalHeadChangeMarksGitSnapshotStale() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  std::filesystem::create_directories(root / ".git");
  WriteFile(root / ".git/HEAD", "ref: refs/heads/main\n");
  WriteFile(root / ".git/index", "index\n");
  WriteFile(root / "README.md", "hello\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "git metadata fixture should open the project");
  for (int attempt = 0; attempt < 20; ++attempt) {
    if (!WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, false)) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  WriteFile(root / ".git/HEAD", "ref: refs/heads/other\n");
  Expect(WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, true),
         "external HEAD changes should trigger project-change processing");
  Expect(WorkspaceShellTestAccess::GitSidebarSnapshotStale(shell),
         "external HEAD changes should mark the git snapshot stale");
}

// Data-integrity (A4): an external change to a file open as a clean view in BOTH split
// groups must reload every group, not just the focused one — a non-focused split view
// left showing stale content can later be saved over the external change.
void TestWorkspaceShellExternalChangeReloadsBothSplitGroups() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file_path = root / "shared.txt";
  WriteFile(file_path, "clean\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "split-reload fixture should open the project");

  // Group 0 gets a clean view of the file.
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, file_path);
  // Split, then open the same file as a clean view in the new (now focused) group.
  Expect(WorkspaceShellTestAccess::SplitEditorGroup(
             shell, microide::workspace::EditorSplitOrientation::Vertical),
         "splitting the editor group should succeed");
  Expect(WorkspaceShellTestAccess::EditorGroupCount(shell) == 2, "there should be two groups");
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, file_path);
  // Refocus group 0 so group 1 (also holding the file) is the NON-focused group.
  if (WorkspaceShellTestAccess::FocusedGroupIndex(shell) != 0) {
    WorkspaceShellTestAccess::FocusOtherEditorGroup(shell);
  }
  Expect(WorkspaceShellTestAccess::FocusedGroupIndex(shell) == 0,
         "group 0 should be focused, leaving group 1 non-focused");
  DrainProjectChanges(shell);

  WriteFile(file_path, "clean updated\n");
  Expect(WaitForProjectReload(shell, std::chrono::seconds(1)),
         "external file change should trigger a project reload");

  Expect(WorkspaceShellTestAccess::GroupActiveViewport(shell, 0).lines()[0] == "clean updated",
         "the focused group's clean view should reload");
  Expect(WorkspaceShellTestAccess::GroupActiveViewport(shell, 1).lines()[0] == "clean updated",
         "the NON-focused split group's clean view must also reload (no stale content)");
}

// Autosave must flush a buffer dirtied in the NON-focused split group. Dirty-tab
// enumeration used to walk only the focused group, so a file open exclusively in
// the other split view was skipped by the autosave flush and never written to disk
// (VSCode "Save All" flushes every group).
void TestWorkspaceShellAutosaveFlushesNonFocusedGroupDirtyTab() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file_a = root / "a.txt";
  const std::filesystem::path file_b = root / "b.txt";
  WriteFile(file_a, "aaa\n");
  WriteFile(file_b, "bbb\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "non-focused autosave fixture should open the project");
  WorkspaceShellTestAccess::OpenFile(shell, file_a);
  // Split, then open file_b only in the new (focused) group 1.
  Expect(WorkspaceShellTestAccess::SplitEditorGroup(
             shell, microide::workspace::EditorSplitOrientation::Vertical),
         "splitting the editor group should succeed");
  Expect(WorkspaceShellTestAccess::FocusedGroupIndex(shell) == 1, "the new group should be focused");
  WorkspaceShellTestAccess::OpenFile(shell, file_b);
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("edited ");
  // Refocus group 0 so file_b is dirty and open ONLY in the non-focused group 1.
  Expect(WorkspaceShellTestAccess::FocusOtherEditorGroup(shell), "focus should return to group 0");
  Expect(WorkspaceShellTestAccess::FocusedGroupIndex(shell) == 0, "group 0 should be focused");
  Expect(WorkspaceShellTestAccess::GroupActiveViewport(shell, 1).dirty(),
         "file_b should be dirty in the non-focused group 1");
  Expect(!WorkspaceShellTestAccess::GroupActiveViewport(shell, 0).dirty(),
         "file_a should be clean in the focused group 0");
  DrainProjectChanges(shell);

  // Set the mode AFTER opening so the project/user config load does not reset it.
  Expect(WorkspaceShellTestAccess::SetSettingValue(shell, "editor.autosave", "after_delay"),
         "after_delay autosave should be settable");
  WorkspaceShellTestAccess::MaybeAutosaveDirtyTabs(shell, /*on_focus_change=*/false);

  Expect(ReadFile(file_b) == "edited bbb\n",
         "autosave must flush the non-focused split group's dirty tab to disk");
  Expect(!WorkspaceShellTestAccess::GroupActiveViewport(shell, 1).dirty(),
         "the flushed non-focused tab should be clean afterwards");
}

}  // namespace

void RegisterExternalRepoChangeTests(std::vector<TestCase>& tests) {
  AddTest(tests, "ExternalRepoChange/AutosaveFlushesNonFocusedGroupDirtyTab",
          TestWorkspaceShellAutosaveFlushesNonFocusedGroupDirtyTab);
  AddTest(tests, "ExternalRepoChange/ReloadsCleanBuffer",
          TestWorkspaceShellExternalChangeReloadsCleanBuffer);
  AddTest(tests, "ExternalRepoChange/ReloadsBothSplitGroups",
          TestWorkspaceShellExternalChangeReloadsBothSplitGroups);
  AddTest(tests, "ExternalRepoChange/BannerForDirtyBuffer",
          TestWorkspaceShellExternalChangeBannerForDirtyBuffer);
  AddTest(tests, "ExternalRepoChange/SelfWriteDoesNotRaiseBanner",
          TestWorkspaceShellSelfWriteDoesNotRaiseBanner);
  AddTest(tests, "ExternalRepoChange/AutosaveFlushRespectsDiskConflict",
          TestWorkspaceShellAutosaveFlushRespectsDiskConflict);
  AddTest(tests, "ExternalRepoChange/SaveTimeConflictGuardBlocksClobber",
          TestWorkspaceShellSaveTimeConflictGuardBlocksClobber);
  AddTest(tests, "ExternalRepoChange/BannerOverwriteWritesInMemoryEdits",
          TestWorkspaceShellBannerOverwriteWritesInMemoryEdits);
  AddTest(tests, "ExternalRepoChange/BannerReloadReplacesBuffer",
          TestWorkspaceShellBannerReloadReplacesBuffer);
  AddTest(tests, "ExternalRepoChange/BannerKeepPreservesBoth",
          TestWorkspaceShellBannerKeepPreservesBoth);
  AddTest(tests, "ExternalRepoChange/CleanReloadRaisesNotice",
          TestWorkspaceShellCleanReloadRaisesNotice);
  AddTest(tests, "ExternalRepoChange/MarksGitSnapshotStaleOnHeadChange",
          TestWorkspaceShellExternalHeadChangeMarksGitSnapshotStale);
}

}  // namespace microide::tests
