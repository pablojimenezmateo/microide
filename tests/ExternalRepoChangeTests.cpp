#include "TestSupport.h"
#include "support/SoftwareCanvas.h"

#include "editor/LineSpan.h"
#include "util/PerformanceCounters.h"
#include "util/StringUtil.h"
#include "util/TextFileIO.h"

#include "workspace/shell/WorkspaceShell.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"

#include "editor/TextBuffer.h"
#include "platform/FileIndexWatcher.h"
#include "platform/Filesystem.h"

#include <chrono>
#include <optional>
#include <string>
#include <thread>

namespace microide::tests {
namespace {

using microide::workspace::WorkspaceShell;
using WorkspaceShellTestAccess = microide::workspace::WorkspaceShell::TestAccess;

bool WaitForProjectReload(WorkspaceShell& shell, std::chrono::milliseconds timeout) {
  return WaitUntil(
      [&shell]() { return WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, false); },
      timeout, std::chrono::milliseconds(10));
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

void TestWorkspaceShellExternalChangeReloadsAnOpenFileTheProjectDoesNotTrack() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path head = root / ".git" / "HEAD";
  const std::filesystem::path outside = temp_dir.path() / "outside.txt";
  WriteFile(root / "notes.txt", "notes\n");
  WriteFile(head, "ref: refs/heads/main\n");
  WriteFile(outside, "before\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false), "open the project");
  WorkspaceShellTestAccess::OpenFile(shell, outside);
  WorkspaceShellTestAccess::OpenFile(shell, head);
  SoftwareCanvas canvas(1280, 720);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::RenderFrameWithRenderer(shell, canvas.renderer());
  for (int attempt = 0; attempt < 20; ++attempt) {
    if (!WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, false)) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  WriteFile(head.parent_path() / "HEAD.lock", "ref: refs/heads/topic\n");
  std::filesystem::rename(head.parent_path() / "HEAD.lock", head);
  WriteFile(outside, "after\n");
  const auto line_of = [&shell](const std::filesystem::path& path) -> std::string {
    const editor::TextViewport* view = WorkspaceShellTestAccess::EditorViewportForPath(shell, path);
    return view != nullptr && !view->lines().empty() ? view->lines()[0] : std::string("<none>");
  };
  (void)WaitUntil(
      [&]() {
        return line_of(head) == "ref: refs/heads/topic" && line_of(outside) == "after";
      },
      std::chrono::seconds(10), std::chrono::milliseconds(10), [&]() {
        (void)WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, false);
        WorkspaceShellTestAccess::FlushPendingFileReads(shell);
      });
  Expect(line_of(head) == "ref: refs/heads/topic",
         "the open .git/HEAD reloads after a branch switch (" + line_of(head) + ")");
  Expect(line_of(outside) == "after",
         "an open file outside the root reloads after a write (" + line_of(outside) + ")");
}

void TestWorkspaceShellExternalChangeReachesAFileOpenOnlyInADiff() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path inside = root / "notes.txt";
  const std::filesystem::path outside = temp_dir.path() / "outside.txt";
  WriteFile(inside, "notes\n");
  WriteFile(outside, "before\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false), "open the project");
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(
             shell, "compare-files " + inside.string() + " " + outside.string()),
         "compare-files opens the diff");
  Expect(WorkspaceShellTestAccess::ActiveTabIsCompare(shell) &&
             WorkspaceShellTestAccess::ActiveCompare(shell).right_editable &&
             WorkspaceShellTestAccess::ActiveCompare(shell).right_viewport.path() == outside,
         "its editable side is the file outside the root");
  Expect(WorkspaceShellTestAccess::EditorViewportForPath(shell, outside) == nullptr,
         "and no editor tab has that file open");
  SoftwareCanvas canvas(1280, 720);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::RenderFrameWithRenderer(shell, canvas.renderer());
  for (int attempt = 0; attempt < 20; ++attempt) {
    if (!WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, false)) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  WriteFile(outside, "after\n");
  const auto diff_line = [&shell]() -> std::string {
    if (!WorkspaceShellTestAccess::ActiveTabIsCompare(shell)) {
      return "<no diff>";
    }
    const auto& lines = WorkspaceShellTestAccess::ActiveCompare(shell).right_viewport.lines();
    return lines.empty() ? std::string() : std::string(lines[0]);
  };
  (void)WaitUntil([&]() { return diff_line() == "after"; }, std::chrono::seconds(10),
                  std::chrono::milliseconds(10), [&]() {
                    (void)WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, false);
                    WorkspaceShellTestAccess::FlushPendingFileReads(shell);
                  });
  Expect(diff_line() == "after", "the diff's side shows the file as it now is (" + diff_line() + ")");

  WorkspaceShellTestAccess::OpenFile(shell, outside);
  const editor::TextViewport* editor = WorkspaceShellTestAccess::EditorViewportForPath(shell, outside);
  Expect(editor != nullptr && !editor->dirty() && !editor->lines().empty() && editor->lines()[0] == "after",
         "and an editor tab opened on it holds what the file holds");
}

void TestWorkspaceShellAChangeKeepsAFileCompareAFileCompare() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path left = root / "v1.left.py";
  const std::filesystem::path right = root / "v1.right.py";
  WriteFile(left, "a\nb\n");
  WriteFile(right, "a\nB\n");
  InitializeGitRepo(root);
  CommitAll(root, "base", "base");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false), "open the project");
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "compare-files " + left.string() + " " + right.string()),
         "compare-files opens the diff");
  const std::string title = WorkspaceShellTestAccess::ActiveCompare(shell).title;
  SoftwareCanvas canvas(1280, 720);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::RenderFrameWithRenderer(shell, canvas.renderer());
  for (int attempt = 0; attempt < 20 && WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, false); ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  WriteFile(right, "a\nC\n");
  const auto right_line = [&shell]() -> std::string {
    if (!WorkspaceShellTestAccess::ActiveTabIsCompare(shell)) {
      return "<no diff>";
    }
    const auto& lines = WorkspaceShellTestAccess::ActiveCompare(shell).right_viewport.lines();
    return lines.size() < 2 ? std::string("<") + std::to_string(lines.size()) + " lines>" : std::string(lines[1]);
  };
  (void)WaitUntil([&]() { return right_line() == "C"; }, std::chrono::seconds(10), std::chrono::milliseconds(10),
                  [&]() {
                    (void)WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, false);
                    WorkspaceShellTestAccess::FlushPendingFileReads(shell);
                  });
  WorkspaceShellTestAccess::FlushPendingFileReads(shell);
  WorkspaceShellTestAccess::RenderFrameWithRenderer(shell, canvas.renderer());
  Expect(WorkspaceShellTestAccess::ActiveTabIsCompare(shell), "the diff is still in front");
  const auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  Expect(compare.plain_compare && compare.title == title,
         "still a compare of the two files, titled '" + title + "', got '" + compare.title + "'");
  Expect(compare.right_editable && right_line() == "C",
         "its editable side shows the file as it now is (" + right_line() + ")");
  bool row_seen = false;
  for (const auto& row : compare.model.rows) {
    row_seen = row_seen || (row.left_text == "b" && row.right_text == "C");
  }
  Expect(row_seen, "and the diff is of the left file against it");
}

void DrainProjectChangesBeforeTheWrite(WorkspaceShell& shell) {
  for (int attempt = 0; attempt < 20; ++attempt) {
    if (!WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, false)) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

void TestWorkspaceShellAChangeReachesADiffOpenWithNoProject() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path left = temp_dir.path() / "loose" / "v2.left.js";
  const std::filesystem::path right = temp_dir.path() / "loose" / "v2.right.js";
  WriteFile(root / "a.txt", "a\n");
  WriteFile(left, "one\n");
  WriteFile(right, "two\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false), "open the project");
  WorkspaceShellTestAccess::CloseProject(shell, 0);
  Expect(WorkspaceShellTestAccess::CurrentProjectState(shell).root.empty(), "and close it: no project is open");
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "compare-files " + left.string() + " " + right.string()),
         "compare-files opens the diff with no project");
  SoftwareCanvas canvas(1280, 720);
  WorkspaceShellTestAccess::RenderFrameWithRenderer(shell, canvas.renderer());
  DrainProjectChangesBeforeTheWrite(shell);

  WriteFile(right, "three\n");
  const auto line = [&shell]() -> std::string {
    if (!WorkspaceShellTestAccess::ActiveTabIsCompare(shell)) {
      return "<no diff>";
    }
    const auto& lines = WorkspaceShellTestAccess::ActiveCompare(shell).right_viewport.lines();
    return lines.empty() ? std::string() : std::string(lines[0]);
  };
  (void)WaitUntil([&]() { return line() == "three"; }, std::chrono::seconds(10), std::chrono::milliseconds(10),
                  [&]() {
                    (void)WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, false);
                    WorkspaceShellTestAccess::FlushPendingFileReads(shell);
                  });
  Expect(line() == "three", "the diff's side shows the file as it now is (" + line() + ")");
}


void TestWorkspaceShellAProjectWhoseRootIsGoneStillWatchesItsOpenFiles() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path first = temp_dir.path() / "first";
  const std::filesystem::path second = temp_dir.path() / "second";
  const std::filesystem::path shared = temp_dir.path() / "shared.txt";
  WriteFile(first / "a.txt", "a\n");
  WriteFile(second / "b.txt", "b\n");
  WriteFile(shared, "before\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  SoftwareCanvas canvas(1280, 720);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, first, false, false), "open the first project");
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, second, false, false), "open the second project");
  WorkspaceShellTestAccess::OpenFile(shell, shared);
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "project-next"), "the first project comes to the front");
  std::filesystem::remove_all(second);
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "project-next"),
         "the second project comes back, its root gone");
  WorkspaceShellTestAccess::RenderFrameWithRenderer(shell, canvas.renderer());
  DrainProjectChangesBeforeTheWrite(shell);

  WriteFile(shared, "after\n");
  const auto line = [&shell, &shared]() -> std::string {
    const editor::TextViewport* view = WorkspaceShellTestAccess::EditorViewportForPath(shell, shared);
    return view != nullptr && !view->lines().empty() ? view->lines()[0] : std::string("<none>");
  };
  (void)WaitUntil([&]() { return line() == "after"; }, std::chrono::seconds(10), std::chrono::milliseconds(10),
                  [&]() {
                    (void)WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, false);
                    WorkspaceShellTestAccess::FlushPendingFileReads(shell);
                  });
  Expect(line() == "after", "the open file reloads after a write (" + line() + ")");
}

void DrainProjectChanges(WorkspaceShell& shell) {
  for (int attempt = 0; attempt < 20; ++attempt) {
    if (!WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, false)) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

void DispatchAndApply(WorkspaceShell& shell,
                      platform::IndexUpdateBatch batch,
                      const char* what) {
  const std::uint64_t before =
      WorkspaceShellTestAccess::LastAppliedProjectChangeGeneration(shell);
  Expect(WorkspaceShellTestAccess::DispatchFileIndexWatcherBatchForTesting(shell,
                                                                           std::move(batch)),
         std::string("the fixture must actually deliver a watcher batch (") + what + ")");
  (void)WaitUntil(
      [&]() { return WorkspaceShellTestAccess::LastAppliedProjectChangeGeneration(shell) > before; },
      std::chrono::seconds(10), std::chrono::milliseconds(10),
      [&]() { (void)WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, false); });
  DrainProjectChanges(shell);
  Expect(WorkspaceShellTestAccess::LastAppliedProjectChangeGeneration(shell) > before,
         std::string("the dispatched batch must actually be APPLIED, or every assertion "
                     "after it is about a sweep that never ran (") +
             what + ")");
  WorkspaceShellTestAccess::FlushPendingFileReads(shell);
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

void TestWorkspaceShellUndoAfterADiskChangeDoesNotReadClean() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file_path = root / "notes.txt";
  WriteFile(file_path, "original\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "undo-after-disk-change fixture should open the project");
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, file_path);
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("//");
  DrainProjectChanges(shell);

  WriteFile(file_path, "changed on disk\noriginal\n");
  Expect(WaitForExternalChangeBanner(shell, file_path, std::chrono::seconds(1)),
         "the change on disk should be noticed under the unsaved edit");
  auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  Expect(editor.Undo(), "the typed edit should undo");
  Expect(editor.lines()[0] == "original", "the undo should restore the content as read");
  Expect(editor.dirty(),
         "a buffer undone to content the file no longer holds must not read clean");
}

void TestWorkspaceShellUndoAfterADiskChangeInTheBackgroundDoesNotReadClean() {
  TemporaryDirectory temp_dir;
  ScopedAppHomes homes(temp_dir.path() / "state", temp_dir.path() / "config");
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path other = temp_dir.path() / "other";
  const std::filesystem::path file_path = root / "notes.txt";
  WriteFile(file_path, "original\n");
  WriteFile(other / "b.txt", "b\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, true, false),
         "the first project should open");
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, file_path);
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("//");
  DrainProjectChanges(shell);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, other, true, false),
         "the second project should open in front");

  WriteFile(file_path, "changed on disk\noriginal\n");
  DrainProjectChanges(shell);
  Expect(WorkspaceShellTestAccess::SwitchProject(shell, 0), "the first project comes back");
  DrainProjectChanges(shell);

  Expect(WorkspaceShellTestAccess::HasExternalChangeBanner(shell, file_path),
         "the change made in the background should be shown under the unsaved edit");
  WorkspaceShellTestAccess::EditorBannerKeep(shell, file_path);
  Expect(WorkspaceShellTestAccess::SwitchProject(shell, 1), "to the second project again");
  Expect(WorkspaceShellTestAccess::SwitchProject(shell, 0), "and back");
  DrainProjectChanges(shell);
  Expect(!WorkspaceShellTestAccess::HasExternalChangeBanner(shell, file_path),
         "a change already kept over should not raise its banner again");
  auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  Expect(editor.Undo(), "the typed edit should undo");
  Expect(editor.lines()[0] == "original", "the undo should restore the content as read");
  Expect(editor.dirty(),
         "a buffer undone to content the file no longer holds must not read clean");
}

void TestWorkspaceShellABackgroundChangeReachesAFileOpenOnlyInDiffs() {
  TemporaryDirectory temp_dir;
  ScopedAppHomes homes(temp_dir.path() / "state", temp_dir.path() / "config");
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path other = temp_dir.path() / "other";
  const std::filesystem::path binary = root / "blob.txt";
  const std::filesystem::path text = root / "notes.txt";
  WriteFile(root / "left.txt", "left\n");
  WriteFile(root / "third.txt", "third\n");
  WriteFile(binary, "");
  WriteFile(text, "before\n");
  WriteFile(other / "b.txt", "b\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, true, false), "the first project opens");
  const auto diff = [&](const std::filesystem::path& left, const std::filesystem::path& right) {
    Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "compare-files " + left.string() + " " + right.string()),
           "compare-files opens a diff of " + right.filename().string());
    Expect(WorkspaceShellTestAccess::ActiveTabIsCompare(shell) &&
               WorkspaceShellTestAccess::ActiveCompare(shell).right_editable,
           "with an editable side");
  };
  diff(root / "left.txt", binary);
  diff(root / "left.txt", text);
  diff(root / "third.txt", text);
  DrainProjectChanges(shell);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, other, true, false), "the second project opens in front");

  const std::string bytes("\x7f\x80\x81\x00\xfe\xff binary", 13);
  WriteFile(binary, bytes);
  WriteFile(text, "after\n");
  DrainProjectChanges(shell);
  Expect(WorkspaceShellTestAccess::SwitchProject(shell, 0), "the first project comes back");
  DrainProjectChanges(shell);

  WorkspaceShellTestAccess::OpenFile(shell, text);
  const editor::TextViewport* notes = WorkspaceShellTestAccess::EditorViewportForPath(shell, text);
  Expect(notes != nullptr && !notes->dirty() && notes->SerializeDocumentText() == "after\n",
         "an editor tab on the file two diffs held shows the file, got '" +
             (notes != nullptr ? notes->SerializeDocumentText() : std::string("<none>")) + "'");
  WorkspaceShellTestAccess::OpenFile(shell, binary);
  const editor::TextViewport* blob = WorkspaceShellTestAccess::EditorViewportForPath(shell, binary);
  Expect(blob != nullptr && !blob->dirty() && blob->SerializeDocumentText() == bytes,
         "and one on the file that became binary holds its bytes, got " +
             std::to_string(blob != nullptr ? blob->SerializeDocumentText().size() : 0) + " of " +
             std::to_string(bytes.size()));
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
  for (int attempt = 0; attempt < 20; ++attempt) {
    WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  Expect(WorkspaceShellTestAccess::EditorBannerCount(shell) == 0,
         "the editor's own save must not raise any banner");
  Expect(ReadFile(file_path) == "mine original\n",
         "the saved file should contain the in-memory edits");
}

void ForceDistinctModificationTime(const std::filesystem::path& path) {
  std::error_code error;
  std::filesystem::last_write_time(
      path, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(2), error);
}

platform::IndexUpdateBatch BuildModifiedBatch(const std::filesystem::path& root,
                                              const std::filesystem::path& relative_path) {
  const std::filesystem::path absolute_path = root / relative_path;
  std::error_code mtime_error;
  const auto mtime = std::filesystem::last_write_time(absolute_path, mtime_error);
  std::error_code size_error;
  const auto size = std::filesystem::file_size(absolute_path, size_error);
  platform::IndexUpdateBatch batch;
  batch.is_initial = false;
  batch.changes.push_back(platform::IndexUpdateBatch::Change{
      .kind = platform::IndexUpdateBatch::Kind::CreatedOrModified,
      .entry = platform::IndexFileEntry{
          .relative_path = relative_path,
          .mtime = mtime_error ? std::filesystem::file_time_type{} : mtime,
          .size = size_error ? 0 : size,
      },
  });
  return batch;
}

std::string BufferText(const editor::TextViewport& view) {
  return util::SerializeLinesStreaming(editor::LineSpan(view.lines()), view.line_ending());
}

std::string ExternalChangeDiagnostics(WorkspaceShell& shell,
                                      const std::filesystem::path& file_path) {
  const auto& viewport = WorkspaceShellTestAccess::ActiveEditor(shell);
  const util::FileSignature current = util::StatFileSignature(file_path);
  return std::string(" [recorded mtime=") +
         std::to_string(viewport.disk_signature().mtime_ticks) +
         " size=" + std::to_string(viewport.disk_signature().size) +
         " hash=" + (viewport.disk_signature().has_content_hash ? "yes" : "no") +
         "; on disk mtime=" + std::to_string(current.mtime_ticks) +
         " size=" + std::to_string(current.size) +
         "; buffer first line='" + std::string(viewport.lines().LineView(0)) +
         "' dirty=" + (viewport.dirty() ? "yes" : "no") +
         "; open views=" + std::to_string(WorkspaceShellTestAccess::CountOpenBufferViews(
                               shell, file_path)) +
         "; echo-suppressed=" +
         (WorkspaceShellTestAccess::DiskSignatureMatchesOpenView(shell, file_path) ? "yes" : "no") +
         "; banners=" + WorkspaceShellTestAccess::DescribeEditorBanners(shell) +
         "; applied generation=" +
         std::to_string(WorkspaceShellTestAccess::LastAppliedProjectChangeGeneration(shell)) +
         "; index version=" +
         std::to_string(WorkspaceShellTestAccess::ProjectFileIndexVersion(shell)) + "]";
}

void TestWorkspaceShellIdenticalRewriteRaisesNoReloadNotice() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path relative = "notes.txt";
  const std::filesystem::path file_path = root / relative;
  WriteFile(file_path, "same bytes\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "identical-rewrite fixture should open the project");
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, file_path);
  Expect(WorkspaceShellTestAccess::QuiesceFileIndexWatcherForTesting(shell),
         "the fixture must have a live watcher to quiesce");
  DrainProjectChanges(shell);
  WorkspaceShellTestAccess::ClearEditorBannersForTesting(shell);

  WriteFile(file_path, "same bytes\n");
  ForceDistinctModificationTime(file_path);
  DispatchAndApply(shell, BuildModifiedBatch(root, relative), "the identical-rewrite batch");

  Expect(!WorkspaceShellTestAccess::HasReloadedNoticeBanner(shell, file_path),
         "a byte-identical rewrite must not announce a reload of a file that did not change");
  Expect(!WorkspaceShellTestAccess::HasExternalChangeBanner(shell, file_path),
         "a byte-identical rewrite must not raise the external-change banner");
}

void TestWorkspaceShellIdenticalRewriteWithSplitViewsReadsOnce() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path relative = "notes.txt";
  const std::filesystem::path file_path = root / relative;
  WriteFile(file_path, "same bytes\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "split-view fixture should open the project");
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, file_path);
  Expect(WorkspaceShellTestAccess::QuiesceFileIndexWatcherForTesting(shell),
         "the fixture must have a live watcher to quiesce");
  Expect(WorkspaceShellTestAccess::SplitEditorGroup(shell, workspace::EditorSplitOrientation::Vertical),
         "the fixture must actually split, or this is the single-view test again");
  Expect(WorkspaceShellTestAccess::EditorGroupCount(shell) == 2 &&
             WorkspaceShellTestAccess::FocusedGroupOpenTabCount(shell) == 1,
         "the split must leave TWO groups each holding a view of the file; one group "
         "would make this the single-view test under a different name");
  DrainProjectChanges(shell);
  WorkspaceShellTestAccess::ClearEditorBannersForTesting(shell);

  WriteFile(file_path, "same bytes\n");
  ForceDistinctModificationTime(file_path);
  DispatchAndApply(shell, BuildModifiedBatch(root, relative), "the identical-rewrite batch");

  Expect(!WorkspaceShellTestAccess::HasReloadedNoticeBanner(shell, file_path),
         "neither view announces a reload of a file that did not change");
  Expect(!WorkspaceShellTestAccess::HasExternalChangeBanner(shell, file_path),
         "neither view raises the external-change banner");

  WriteFile(file_path, "SAME BYTES\n");
  ForceDistinctModificationTime(file_path);
  DispatchAndApply(shell, BuildModifiedBatch(root, relative), "the real-rewrite batch");
  Expect(WorkspaceShellTestAccess::HasReloadedNoticeBanner(shell, file_path),
         WorkspaceShellTestAccess::HasReloadedNoticeBanner(shell, file_path)
             ? std::string()
             : "a real external change still reaches a split view" +
                   ExternalChangeDiagnostics(shell, file_path));
}

void TestWorkspaceShellStaleEchoDigestDoesNotSwallowALaterChange() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file_path = root / "notes.txt";
  WriteFile(file_path, "same bytes\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "stale-digest fixture should open the project");
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, file_path);
  Expect(WorkspaceShellTestAccess::QuiesceFileIndexWatcherForTesting(shell),
         "the fixture must have a live watcher to quiesce");
  DrainProjectChanges(shell);

  WriteFile(file_path, "same bytes\n");
  ForceDistinctModificationTime(file_path);
  const util::FileSignature read_signature = util::StatFileSignature(file_path);
  const std::uint64_t old_digest = util::QuickContentHash("same bytes\n");

  WriteFile(file_path, "SAME BYTES\n");
  ForceDistinctModificationTime(file_path);
  Expect(!WorkspaceShellTestAccess::ExternalChangeIsOwnEchoForTesting(shell, file_path, old_digest,
                                                                      read_signature),
         "a digest read before the file changed again must not excuse the change");

  WriteFile(file_path, "same bytes\n");
  ForceDistinctModificationTime(file_path);
  Expect(WorkspaceShellTestAccess::ExternalChangeIsOwnEchoForTesting(
             shell, file_path, old_digest, util::StatFileSignature(file_path)),
         "a digest read under the current stat confirms an identical rewrite");
}

void TestWorkspaceShellIdenticalRewriteConfirmsOffTheShellThread() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path relative = "notes.txt";
  const std::filesystem::path file_path = root / relative;
  WriteFile(file_path, "same bytes\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "off-thread-confirm fixture should open the project");
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, file_path);
  Expect(WorkspaceShellTestAccess::QuiesceFileIndexWatcherForTesting(shell),
         "the fixture must have a live watcher to quiesce");
  DrainProjectChanges(shell);
  WorkspaceShellTestAccess::ClearEditorBannersForTesting(shell);

  const std::uint64_t reads_before = WorkspaceShellTestAccess::PostedFileReadCount(shell);
  const std::uint64_t inline_reads_before =
      util::ReadPerformanceCounter(util::PerfCounterId::ExternalChangeConfirmInlineReads);
  WriteFile(file_path, "same bytes\n");
  ForceDistinctModificationTime(file_path);
  DispatchAndApply(shell, BuildModifiedBatch(root, relative), "the identical-rewrite batch");

  Expect(WorkspaceShellTestAccess::PostedFileReadCount(shell) == reads_before + 1,
         "the confirming read is posted to the reader thread, exactly once");
  Expect(!WorkspaceShellTestAccess::HasReloadedNoticeBanner(shell, file_path) &&
             !WorkspaceShellTestAccess::HasExternalChangeBanner(shell, file_path),
         "and the verdict it comes back with is still 'our own write'");

  const std::uint64_t reads_after_confirm = WorkspaceShellTestAccess::PostedFileReadCount(shell);
  WriteFile(file_path, "different length entirely\n");
  ForceDistinctModificationTime(file_path);
  DispatchAndApply(shell, BuildModifiedBatch(root, relative), "the resized batch");
  Expect(WorkspaceShellTestAccess::PostedFileReadCount(shell) == reads_after_confirm,
         "a size change is decided by the stat alone, with no read posted");
  Expect(WorkspaceShellTestAccess::HasReloadedNoticeBanner(shell, file_path),
         "and it still reaches the clean buffer as a reload");

  WriteFile(file_path, "DIFFERENT LENGTH ENTIRELY\n");
  ForceDistinctModificationTime(file_path);
  DispatchAndApply(shell, BuildModifiedBatch(root, relative), "the same-size rewrite batch");
  Expect(WorkspaceShellTestAccess::PostedFileReadCount(shell) == reads_after_confirm + 1,
         "the same-size rewrite posts one digest");
  Expect(BufferText(WorkspaceShellTestAccess::ActiveEditor(shell)) ==
             "DIFFERENT LENGTH ENTIRELY\n",
         "and the clean buffer is reloaded with the new content");

  Expect(util::ReadPerformanceCounter(
             util::PerfCounterId::ExternalChangeConfirmInlineReads) == inline_reads_before,
         "no confirming read ran on the shell thread");
}

void TestWorkspaceShellRealRewriteStillNotifies() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path relative = "notes.txt";
  const std::filesystem::path file_path = root / relative;
  WriteFile(file_path, "same bytes\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "real-rewrite fixture should open the project");
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, file_path);
  Expect(WorkspaceShellTestAccess::QuiesceFileIndexWatcherForTesting(shell),
         "the fixture must have a live watcher to quiesce");
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("dirty ");
  DrainProjectChanges(shell);

  WriteFile(file_path, "SAME BYTES\n");
  ForceDistinctModificationTime(file_path);
  DispatchAndApply(shell, BuildModifiedBatch(root, relative),
                   "the real-rewrite batch");

  Expect(WorkspaceShellTestAccess::HasExternalChangeBanner(shell, file_path),
         WorkspaceShellTestAccess::HasExternalChangeBanner(shell, file_path)
             ? std::string()
             : "a real external change to a dirty buffer still raises the banner" +
                   ExternalChangeDiagnostics(shell, file_path));
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

  WriteFile(file_path, "newer on disk\n");
  Expect(!WorkspaceShellTestAccess::SaveTab(shell, WorkspaceShellTestAccess::ActiveTabIndex(shell)),
         "save must fail when the file changed on disk since load");
  Expect(ReadFile(file_path) == "newer on disk\n",
         "the save-time guard must not overwrite the newer on-disk content");
  Expect(WorkspaceShellTestAccess::HasExternalChangeBanner(shell, file_path),
         "a blocked save should raise the external-change banner");
}

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
  Expect(WorkspaceShellTestAccess::SetSettingValue(shell, "editor.autosave", "after_delay"),
         "after_delay autosave should be settable");
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("dirty ");
  DrainProjectChanges(shell);

  WriteFile(file_path, "newer on disk\n");

  WorkspaceShellTestAccess::MaybeAutosaveDirtyTabs(shell, false);

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

void TestWorkspaceShellBannerCompareShowsDiskBesideTheBuffer() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file_path = root / "notes.txt";
  WriteFile(file_path, "original\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false), "opens");
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, file_path);
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("dirty ");
  DrainProjectChanges(shell);
  WriteFile(file_path, "newer on disk\n");
  Expect(WaitForExternalChangeBanner(shell, file_path, std::chrono::seconds(1)), "banner");

  Expect(!WorkspaceShellTestAccess::ExecuteCommandLine(shell, "save"), "the save is refused");
  std::string said = WorkspaceShellTestAccess::CommandFeedbackText(shell);
  for (const auto& row : WorkspaceShellTestAccess::ActiveNotifications(shell)) {
    said += " | " + row.message;
  }
  Expect(said.find("changed on disk since it was opened") != std::string::npos,
         "and says why, not just 'Save failed': " + said);

  WorkspaceShellTestAccess::EditorBannerCompare(shell, file_path);
  Expect(WorkspaceShellTestAccess::ActiveTabIsCompare(shell), "Compare opens a comparison");
  Expect(WorkspaceShellTestAccess::EditorBannerCount(shell) == 1,
         "and leaves the banner up: Reload or Overwrite is still the choice");
  Expect(ReadFile(file_path) == "newer on disk\n", "nothing was written");
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

void TestWorkspaceShellCleanReloadDoesNotSnapshotBuffers() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file_path = root / "big.txt";
  std::string content;
  for (int i = 0; i < 500; ++i) {
    content += "line ";
    content += std::to_string(i);
    content += "\n";
  }
  WriteFile(file_path, content);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "snapshot-free reload fixture should open the project");
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, file_path);
  for (int attempt = 0; attempt < 20; ++attempt) {
    if (!WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, false)) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  std::string updated;
  for (int i = 0; i < 250; ++i) {
    updated += "line ";
    updated += std::to_string(i);
    updated += "\n";
  }
  updated += "brand new tail line\n";
  WriteFile(file_path, updated);

  microide::editor::TextBuffer::reset_snapshot_build_count();
  Expect(WaitForProjectReload(shell, std::chrono::seconds(1)),
         "external change to a clean buffer should trigger a reload");
  Expect(microide::editor::TextBuffer::snapshot_build_count() == 0,
         "a clean external reload must not snapshot the old or new buffer for LSP sync");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).line_count() == 252,
         "the reloaded buffer should hold the new (shorter) content");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).lines()[250] == "brand new tail line",
         "the reloaded buffer should end with the new tail line");
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

void TestWorkspaceShellAnEditedMergeResultDoesNotKeepTheFilesCleanTabStale() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file_path = root / "scheduler.cpp";
  const std::filesystem::path base = temp_dir.path() / "base.cpp";
  const std::filesystem::path incoming = temp_dir.path() / "incoming.cpp";
  WriteFile(file_path, "<<<<<<< HEAD\nours\n=======\ntheirs\n>>>>>>> feature\n");
  WriteFile(base, "base\n");
  WriteFile(incoming, "theirs\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false), "open the project");
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, file_path);
  for (int attempt = 0; attempt < 20; ++attempt) {
    if (!WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, false)) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, file_path, file_path),
         "a merge tab opens for the same file");
  WorkspaceShellTestAccess::ActiveMerge(shell).result_viewport.InsertText("resolving ");
  Expect(WorkspaceShellTestAccess::ActiveMerge(shell).result_viewport.dirty(), "its result is edited");

  WriteFile(file_path, "ours\n");
  Expect(WaitForProjectReload(shell, std::chrono::seconds(1)), "the change reaches the project");

  const editor::TextViewport* editor = nullptr;
  for (const auto& tab : WorkspaceShellTestAccess::OpenTabs(shell)) {
    if (tab.kind == microide::workspace::TabEntry::Kind::Editor && tab.editor_state.has_value() &&
        tab.editor_state->viewport.path() == file_path) {
      editor = &tab.editor_state->viewport;
    }
  }
  Expect(editor != nullptr, "the editor tab is still open");
  Expect(!editor->dirty() && editor->lines().size() == 2 && editor->lines()[0] == "ours",
         "the clean editor tab reloads what the file now holds: '" +
             (editor->lines().empty() ? std::string() : std::string(editor->lines()[0])) + "'");
  const auto& merge = WorkspaceShellTestAccess::ActiveMerge(shell);
  Expect(merge.result_viewport.dirty() && merge.result_viewport.lines()[0].starts_with("resolving "),
         "the merge result keeps its edits");
}

void TestWorkspaceShellChangeToASymlinkTargetReloadsTheLinksCleanBuffer() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path target_path = root / "target.txt";
  const std::filesystem::path link_path = root / "link.txt";
  WriteFile(target_path, "before\n");
  std::filesystem::create_symlink("target.txt", link_path);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "symlink fixture should open the project");
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, link_path);
  DrainProjectChanges(shell);

  WriteFile(target_path, "after the target changed\n");
  ForceDistinctModificationTime(target_path);
  DispatchAndApply(shell, BuildModifiedBatch(root, "target.txt"), "the target's batch");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).lines()[0] == "after the target changed",
         "a clean buffer opened through a symlink reloads when its target changes, got: " +
             std::string(WorkspaceShellTestAccess::ActiveEditor(shell).lines()[0]));
  Expect(WorkspaceShellTestAccess::HasReloadedNoticeBanner(shell, link_path),
         "and says so, on the link's tab");
}

void TestWorkspaceShellAViewOpenedBeforeItsPathBecameALinkFollowsTheTarget() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path target_path = root / "target.txt";
  const std::filesystem::path link_path = root / "link.txt";
  WriteFile(root / "other.txt", "other\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false), "the project opens");
  (void)WorkspaceShellTestAccess::ExecuteCommandLine(shell, "open " + link_path.string());
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).path() == link_path &&
             WorkspaceShellTestAccess::ActiveEditor(shell).lines()[0].empty(),
         "nothing at the path yet: an empty buffer bound to it");
  DrainProjectChanges(shell);

  std::filesystem::create_symlink("target.txt", link_path);
  platform::IndexUpdateBatch link_made;
  link_made.is_initial = false;
  link_made.changes.push_back(platform::IndexUpdateBatch::Change{
      .kind = platform::IndexUpdateBatch::Kind::Deleted,
      .entry = platform::IndexFileEntry{.relative_path = "link.txt"},
  });
  DispatchAndApply(shell, std::move(link_made), "the dangling link, read as gone");

  WriteFile(target_path, "arrived\n");
  ForceDistinctModificationTime(target_path);
  DispatchAndApply(shell, BuildModifiedBatch(root, "target.txt"), "the target's arrival");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).lines()[0] == "arrived",
         "the view open at the link's path follows it to the target, got: " +
             std::string(WorkspaceShellTestAccess::ActiveEditor(shell).lines()[0]));
}

void TestWorkspaceShellAReloadByPathReachesAViewOpenedThroughALink() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path target_path = root / "target.txt";
  const std::filesystem::path link_path = root / "link.txt";
  std::filesystem::create_directories(root);
  std::filesystem::create_symlink("target.txt", link_path);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false), "the project opens");
  (void)WorkspaceShellTestAccess::ExecuteCommandLine(shell, "open " + link_path.string());
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).path() == link_path &&
             WorkspaceShellTestAccess::ActiveEditor(shell).lines()[0].empty(),
         "the link opens, empty");

  WriteFile(target_path, "arrived\n");
  WorkspaceShellTestAccess::ReloadCleanEditorTabsForPath(shell, target_path);
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).lines()[0] == "arrived",
         "reloading the target reloads the view open through the link, got: " +
             std::string(WorkspaceShellTestAccess::ActiveEditor(shell).lines()[0]));
}

void TestWorkspaceShellALinksChainThatChangedStillReachesItsCleanBuffer() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path target = root / "target.txt";
  const std::filesystem::path middle = root / "middle.txt";
  const std::filesystem::path link = root / "link.txt";
  WriteFile(target, "the first target\n");
  std::filesystem::create_symlink("target.txt", middle);
  std::filesystem::create_symlink("middle.txt", link);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false), "open the project");
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, link);
  DrainProjectChanges(shell);
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).lines()[0] == "the first target", "opened through the chain");

  std::filesystem::remove(middle);
  WriteFile(middle, "middle is a file now\n");
  ForceDistinctModificationTime(middle);
  DispatchAndApply(shell, BuildModifiedBatch(root, "middle.txt"), "the middle file's batch");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).lines()[0] == "middle is a file now",
         "the link's clean buffer follows where its chain leads now, got: " +
             std::string(WorkspaceShellTestAccess::ActiveEditor(shell).lines()[0]));
}

void TestWorkspaceShellChangeReachesAViewOpenedThroughASymlinkedFolder() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path real_path = root / "dir" / "a.txt";
  const std::filesystem::path via_link = root / "dir" / "loop" / "a.txt";
  WriteFile(real_path, "before\n");
  std::filesystem::create_directory_symlink(".", root / "dir" / "loop");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "folder-link fixture should open the project");
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, via_link);
  DrainProjectChanges(shell);

  WriteFile(real_path, "after\n");
  ForceDistinctModificationTime(real_path);
  DispatchAndApply(shell, BuildModifiedBatch(root, "dir/a.txt"), "the change's batch");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).lines()[0] == "after",
         "a clean view opened through a symlinked folder reloads when its file changes, got: " +
             std::string(WorkspaceShellTestAccess::ActiveEditor(shell).lines()[0]));
  Expect(WorkspaceShellTestAccess::HasReloadedNoticeBanner(shell, via_link),
         "and says so, on that view's tab");
}

void TestWorkspaceShellSaveThroughASymlinkIsItsOwnEcho() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path target_path = root / "target.txt";
  const std::filesystem::path link_path = root / "link.txt";
  WriteFile(target_path, "original\n");
  std::filesystem::create_symlink("target.txt", link_path);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "symlink save fixture should open the project");
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, link_path);
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("mine ");
  DrainProjectChanges(shell);
  Expect(WorkspaceShellTestAccess::SaveTab(shell, WorkspaceShellTestAccess::ActiveTabIndex(shell)),
         "saving through the link should succeed");
  Expect(std::filesystem::is_symlink(link_path) && ReadFile(target_path) == "mine original\n",
         "the save writes the target and keeps the link");

  DispatchAndApply(shell, BuildModifiedBatch(root, "target.txt"), "the save's echo");
  Expect(WorkspaceShellTestAccess::EditorBannerCount(shell) == 0,
         "the echo of a save through the link raises no banner");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).lines()[0] == "mine original",
         "and leaves the buffer as saved");
}

void TestWorkspaceShellFloodPastTheCapReloadsAnOpenFileItDidNotName() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path open_file = root / "zz-open.txt";
  WriteFile(open_file, "before\n");
  constexpr int kFlood = 1100;
  for (int i = 0; i < kFlood; ++i) {
    WriteFile(root / "gen" / ("f" + std::to_string(i) + ".txt"), "x\n");
  }

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "flood fixture should open the project");
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, open_file);
  Expect(WaitUntil([&]() { return WorkspaceShellTestAccess::ProjectFileIndexVersion(shell) > 0; },
                   std::chrono::seconds(5), std::chrono::milliseconds(5)),
         "flood fixture's initial index should land");
  Expect(WorkspaceShellTestAccess::QuiesceFileIndexWatcherForTesting(shell),
         "flood fixture should have a live watcher to quiesce");
  DrainProjectChanges(shell);

  WriteFile(open_file, "after\n");
  platform::IndexUpdateBatch flood;
  const auto modified = [&](const std::filesystem::path& absolute) {
    const std::optional<platform::FileMetadata> metadata = platform::ReadFileMetadata(absolute);
    Expect(metadata.has_value(), "a flood fixture file should exist");
    return platform::IndexUpdateBatch::Change{
        .kind = platform::IndexUpdateBatch::Kind::CreatedOrModified,
        .entry = platform::IndexFileEntry{.relative_path = absolute.lexically_relative(root),
                                          .mtime = metadata->mtime,
                                          .size = metadata->size},
    };
  };
  for (int i = 0; i < kFlood; ++i) {
    flood.changes.push_back(modified(root / "gen" / ("f" + std::to_string(i) + ".txt")));
  }
  flood.changes.push_back(modified(open_file));
  Expect(WorkspaceShellTestAccess::DispatchFileIndexWatcherBatchForTesting(shell, std::move(flood)),
         "flood fixture should deliver its batch");
  Expect(WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, false),
         "the flood should be applied");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).lines()[0] == "after",
         "an open file the flood changed past the cap must reload, not keep its old text");
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

void TestWorkspaceShellExternalChangeReloadsBothSplitGroups() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file_path = root / "shared.txt";
  WriteFile(file_path, "clean\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "split-reload fixture should open the project");

  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, file_path);
  Expect(WorkspaceShellTestAccess::SplitEditorGroup(
             shell, microide::workspace::EditorSplitOrientation::Vertical),
         "splitting the editor group should succeed");
  Expect(WorkspaceShellTestAccess::EditorGroupCount(shell) == 2, "there should be two groups");
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, file_path);
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
  Expect(WorkspaceShellTestAccess::SplitEditorGroup(
             shell, microide::workspace::EditorSplitOrientation::Vertical),
         "splitting the editor group should succeed");
  Expect(WorkspaceShellTestAccess::FocusedGroupIndex(shell) == 1, "the new group should be focused");
  WorkspaceShellTestAccess::OpenFile(shell, file_b);
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("edited ");
  Expect(WorkspaceShellTestAccess::FocusOtherEditorGroup(shell), "focus should return to group 0");
  Expect(WorkspaceShellTestAccess::FocusedGroupIndex(shell) == 0, "group 0 should be focused");
  Expect(WorkspaceShellTestAccess::GroupActiveViewport(shell, 1).dirty(),
         "file_b should be dirty in the non-focused group 1");
  Expect(!WorkspaceShellTestAccess::GroupActiveViewport(shell, 0).dirty(),
         "file_a should be clean in the focused group 0");
  DrainProjectChanges(shell);

  Expect(WorkspaceShellTestAccess::SetSettingValue(shell, "editor.autosave", "after_delay"),
         "after_delay autosave should be settable");
  WorkspaceShellTestAccess::MaybeAutosaveDirtyTabs(shell, false);

  Expect(ReadFile(file_b) == "edited bbb\n",
         "autosave must flush the non-focused split group's dirty tab to disk");
  Expect(!WorkspaceShellTestAccess::GroupActiveViewport(shell, 1).dirty(),
         "the flushed non-focused tab should be clean afterwards");
}

void TestWorkspaceShellAnAutosaveWritesTheBufferAsItIs() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file = root / "notes.txt";
  WriteFile(file, "abc");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false), "the project opens");
  WorkspaceShellTestAccess::OpenFile(shell, file);
  DrainProjectChanges(shell);
  Expect(WorkspaceShellTestAccess::SetSettingValue(shell, "editor.autosave", "on_focus_change"),
         "on_focus_change autosave is settable");
  auto& viewport = WorkspaceShellTestAccess::ActiveEditor(shell);
  viewport.InsertText("1");
  viewport.MoveCursorTo(0, 4);
  viewport.InsertText("2");
  const std::string with_both = viewport.SerializeText();
  viewport.Undo();
  const std::string with_one = viewport.SerializeText();
  Expect(viewport.dirty(), "mid-undo the buffer is dirty");

  WorkspaceShellTestAccess::MaybeAutosaveDirtyTabs(shell, true);

  Expect(ReadFile(file) == with_one, "the autosave wrote the buffer as it is: '" + ReadFile(file) + "'");
  Expect(viewport.SerializeText() == with_one, "the autosave left the buffer's text alone");
  Expect(!viewport.dirty(), "the autosaved buffer is clean");
  viewport.Redo();
  Expect(viewport.SerializeText() == with_both, "the redo history survived the autosave");
}


void TestWorkspaceShellUndoInADiffAfterADiskChangeDoesNotReadClean() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path left = root / "v1.left.py";
  const std::filesystem::path right = root / "v1.right.py";
  WriteFile(left, "left\n");
  WriteFile(right, "original\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false), "open the project");
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "compare-files " + left.string() + " " + right.string()),
         "compare-files opens the diff");
  Expect(WorkspaceShellTestAccess::ActiveTabIsCompare(shell) &&
             WorkspaceShellTestAccess::ActiveCompare(shell).right_editable &&
             WorkspaceShellTestAccess::EditorViewportForPath(shell, right) == nullptr,
         "the file is open only as the diff's editable side");
  auto& side = WorkspaceShellTestAccess::ActiveCompare(shell).right_viewport;
  side.InsertText("//");
  DrainProjectChangesBeforeTheWrite(shell);

  WriteFile(right, "changed on disk\noriginal\n");
  (void)WaitUntil([&]() { return false; }, std::chrono::milliseconds(600), std::chrono::milliseconds(10),
                  [&]() { (void)WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, true); });
  Expect(side.dirty(), "the typing is kept under the change on disk");
  Expect(side.Undo(), "the typed edit undoes");
  Expect(!side.lines().empty() && side.lines()[0] == "original", "the undo restores the content as read");
  Expect(side.dirty(), "a diff side undone to content the file no longer holds must not read clean");
}


void TestWorkspaceShellAChangeReachesADiffOpenThroughALink() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path left = root / "left.txt";
  const std::filesystem::path target = root / "target.txt";
  const std::filesystem::path link = root / "qa-ext" / "link2";
  WriteFile(left, "left\n");
  std::filesystem::create_directories(link.parent_path());
  std::filesystem::create_symlink(target, link);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false), "open the project");
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "compare-files " + left.string() + " " + link.string()),
         "compare-files opens the diff");
  Expect(WorkspaceShellTestAccess::ActiveTabIsCompare(shell) &&
             WorkspaceShellTestAccess::ActiveCompare(shell).right_editable &&
             WorkspaceShellTestAccess::EditorViewportForPath(shell, link) == nullptr,
         "the link is open only as the diff's editable side");
  SoftwareCanvas canvas(1280, 720);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::RenderFrameWithRenderer(shell, canvas.renderer());
  DrainProjectChangesBeforeTheWrite(shell);

  WriteFile(target, "after\n");
  const auto side_line = [&shell]() -> std::string {
    if (!WorkspaceShellTestAccess::ActiveTabIsCompare(shell)) {
      return "<no diff>";
    }
    const auto& lines = WorkspaceShellTestAccess::ActiveCompare(shell).right_viewport.lines();
    return lines.empty() ? std::string() : std::string(lines[0]);
  };
  (void)WaitUntil([&]() { return side_line() == "after"; }, std::chrono::seconds(10),
                  std::chrono::milliseconds(10), [&]() {
                    (void)WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, false);
                    WorkspaceShellTestAccess::FlushPendingFileReads(shell);
                  });
  Expect(side_line() == "after", "the diff's side reached through the link shows the file as it now is (" +
                                     side_line() + ")");
}

}

void RegisterExternalRepoChangeTests(std::vector<TestCase>& tests) {
  AddTest(tests, "ExternalRepoChange/AnAutosaveWritesTheBufferAsItIs", TestWorkspaceShellAnAutosaveWritesTheBufferAsItIs);
  AddTest(tests, "ExternalRepoChange/AutosaveFlushesNonFocusedGroupDirtyTab",
          TestWorkspaceShellAutosaveFlushesNonFocusedGroupDirtyTab);
  AddTest(tests, "ExternalRepoChange/ReloadsCleanBuffer",
          TestWorkspaceShellExternalChangeReloadsCleanBuffer);
  AddTest(tests, "ExternalRepoChange/ReloadsAnOpenFileTheProjectDoesNotTrack",
          TestWorkspaceShellExternalChangeReloadsAnOpenFileTheProjectDoesNotTrack);
  AddTest(tests, "ExternalRepoChange/AChangeReachesADiffOpenWithNoProject",
          TestWorkspaceShellAChangeReachesADiffOpenWithNoProject);
  AddTest(tests, "ExternalRepoChange/AChangeKeepsAFileCompareAFileCompare",
          TestWorkspaceShellAChangeKeepsAFileCompareAFileCompare);
  AddTest(tests, "ExternalRepoChange/ReachesAFileOpenOnlyInADiff",
          TestWorkspaceShellExternalChangeReachesAFileOpenOnlyInADiff);
  AddTest(tests, "ExternalRepoChange/AChangeReachesADiffOpenThroughALink",
          TestWorkspaceShellAChangeReachesADiffOpenThroughALink);
  AddTest(tests, "ExternalRepoChange/AProjectWhoseRootIsGoneStillWatchesItsOpenFiles",
          TestWorkspaceShellAProjectWhoseRootIsGoneStillWatchesItsOpenFiles);
  AddTest(tests, "ExternalRepoChange/CleanReloadDoesNotSnapshotBuffers",
          TestWorkspaceShellCleanReloadDoesNotSnapshotBuffers);
  AddTest(tests, "ExternalRepoChange/ReloadsBothSplitGroups",
          TestWorkspaceShellExternalChangeReloadsBothSplitGroups);
  AddTest(tests, "ExternalRepoChange/BannerForDirtyBuffer",
          TestWorkspaceShellExternalChangeBannerForDirtyBuffer);
  AddTest(tests, "ExternalRepoChange/UndoAfterADiskChangeDoesNotReadClean",
          TestWorkspaceShellUndoAfterADiskChangeDoesNotReadClean);
  AddTest(tests, "ExternalRepoChange/UndoInADiffAfterADiskChangeDoesNotReadClean",
          TestWorkspaceShellUndoInADiffAfterADiskChangeDoesNotReadClean);
AddTest(tests, "ExternalRepoChange/UndoAfterADiskChangeInTheBackgroundDoesNotReadClean",
          TestWorkspaceShellUndoAfterADiskChangeInTheBackgroundDoesNotReadClean);
AddTest(tests, "ExternalRepoChange/ABackgroundChangeReachesAFileOpenOnlyInDiffs",
        TestWorkspaceShellABackgroundChangeReachesAFileOpenOnlyInDiffs);
  AddTest(tests, "ExternalRepoChange/SelfWriteDoesNotRaiseBanner",
          TestWorkspaceShellSelfWriteDoesNotRaiseBanner);
  AddTest(tests, "ExternalRepoChange/AutosaveFlushRespectsDiskConflict",
          TestWorkspaceShellAutosaveFlushRespectsDiskConflict);
  AddTest(tests, "ExternalRepoChange/IdenticalRewriteRaisesNoReloadNotice",
          TestWorkspaceShellIdenticalRewriteRaisesNoReloadNotice);
  AddTest(tests, "ExternalRepoChange/StaleEchoDigestDoesNotSwallowALaterChange",
          TestWorkspaceShellStaleEchoDigestDoesNotSwallowALaterChange);
  AddTest(tests, "ExternalRepoChange/IdenticalRewriteWithSplitViewsReadsOnce",
          TestWorkspaceShellIdenticalRewriteWithSplitViewsReadsOnce);
  AddTest(tests, "ExternalRepoChange/IdenticalRewriteConfirmsOffTheShellThread",
          TestWorkspaceShellIdenticalRewriteConfirmsOffTheShellThread);
  AddTest(tests, "ExternalRepoChange/RealRewriteStillNotifies",
          TestWorkspaceShellRealRewriteStillNotifies);
  AddTest(tests, "ExternalRepoChange/SaveTimeConflictGuardBlocksClobber",
          TestWorkspaceShellSaveTimeConflictGuardBlocksClobber);
  AddTest(tests, "ExternalRepoChange/BannerCompareShowsDiskBesideTheBuffer",
          TestWorkspaceShellBannerCompareShowsDiskBesideTheBuffer);
  AddTest(tests, "ExternalRepoChange/BannerOverwriteWritesInMemoryEdits",
          TestWorkspaceShellBannerOverwriteWritesInMemoryEdits);
  AddTest(tests, "ExternalRepoChange/BannerReloadReplacesBuffer",
          TestWorkspaceShellBannerReloadReplacesBuffer);
  AddTest(tests, "ExternalRepoChange/BannerKeepPreservesBoth",
          TestWorkspaceShellBannerKeepPreservesBoth);
  AddTest(tests, "ExternalRepoChange/CleanReloadRaisesNotice",
          TestWorkspaceShellCleanReloadRaisesNotice);
  AddTest(tests, "ExternalRepoChange/ChangeToASymlinkTargetReloadsTheLinksCleanBuffer",
          TestWorkspaceShellChangeToASymlinkTargetReloadsTheLinksCleanBuffer);
  AddTest(tests, "ExternalRepoChange/AReloadByPathReachesAViewOpenedThroughALink",
          TestWorkspaceShellAReloadByPathReachesAViewOpenedThroughALink);
  AddTest(tests, "ExternalRepoChange/AViewOpenedBeforeItsPathBecameALinkFollowsTheTarget",
          TestWorkspaceShellAViewOpenedBeforeItsPathBecameALinkFollowsTheTarget);
  AddTest(tests, "ExternalRepoChange/ALinksChainThatChangedStillReachesItsCleanBuffer",
          TestWorkspaceShellALinksChainThatChangedStillReachesItsCleanBuffer);
  AddTest(tests, "ExternalRepoChange/AnEditedMergeResultDoesNotKeepTheFilesCleanTabStale",
          TestWorkspaceShellAnEditedMergeResultDoesNotKeepTheFilesCleanTabStale);
  AddTest(tests, "ExternalRepoChange/SaveThroughASymlinkIsItsOwnEcho",
          TestWorkspaceShellSaveThroughASymlinkIsItsOwnEcho);
  AddTest(tests, "ExternalRepoChange/ChangeReachesAViewOpenedThroughASymlinkedFolder",
          TestWorkspaceShellChangeReachesAViewOpenedThroughASymlinkedFolder);
  AddTest(tests, "ExternalRepoChange/FloodPastTheCapReloadsAnOpenFileItDidNotName",
          TestWorkspaceShellFloodPastTheCapReloadsAnOpenFileItDidNotName);
  AddTest(tests, "ExternalRepoChange/MarksGitSnapshotStaleOnHeadChange",
          TestWorkspaceShellExternalHeadChangeMarksGitSnapshotStale);
}

}
