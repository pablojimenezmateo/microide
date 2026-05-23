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

bool WaitForDirtyPrompt(WorkspaceShell& shell, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (WorkspaceShellTestAccess::DirtyPromptVisible(shell)) {
      return true;
    }
    WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return WorkspaceShellTestAccess::DirtyPromptVisible(shell);
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

void TestWorkspaceShellExternalChangePromptsDirtyBuffer() {
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
  for (int attempt = 0; attempt < 20; ++attempt) {
    if (!WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, false)) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  WriteFile(file_path, "on disk\n");
  Expect(WaitForDirtyPrompt(shell, std::chrono::seconds(1)),
         "dirty buffers should show an external-change prompt");
  Expect(WorkspaceShellTestAccess::DirtyPromptMessage(shell).find("on disk") != std::string::npos ||
             WorkspaceShellTestAccess::DirtyPromptMessage(shell).find("Reload") != std::string::npos,
         "external-change prompt should describe the disk conflict");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).lines()[0].starts_with("dirty "),
         "dirty buffers should keep in-memory edits until the user reloads");
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

}  // namespace

void RegisterExternalRepoChangeTests(std::vector<TestCase>& tests) {
  AddTest(tests, "ExternalRepoChange/ReloadsCleanBuffer",
          TestWorkspaceShellExternalChangeReloadsCleanBuffer);
  AddTest(tests, "ExternalRepoChange/PromptsDirtyBuffer",
          TestWorkspaceShellExternalChangePromptsDirtyBuffer);
  AddTest(tests, "ExternalRepoChange/MarksGitSnapshotStaleOnHeadChange",
          TestWorkspaceShellExternalHeadChangeMarksGitSnapshotStale);
}

}  // namespace microide::tests
