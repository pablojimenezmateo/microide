#include "TestSupport.h"

#include "editor/LineSpan.h"
#include "util/PerformanceCounters.h"
#include "util/StringUtil.h"

#include "workspace/shell/WorkspaceShell.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"

#include "editor/TextBuffer.h"
#include "platform/FileIndexWatcher.h"

#include <chrono>
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

// Drains any pending project-change events so the watcher baseline is settled.
void DrainProjectChanges(WorkspaceShell& shell) {
  for (int attempt = 0; attempt < 20; ++attempt) {
    if (!WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, false)) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

// Dispatch a synthetic watcher batch and let the shell apply it — then CHECK that
// it applied. A batch that is delivered but never acted on leaves every "no banner
// was raised" assertion below passing for the wrong reason, which is exactly how
// TD-2026-09-29-311 hid: under load the initial scan had not finished when the
// fixture stopped the watcher, so the dispatch wrapper buffered every synthetic
// batch waiting for a baseline that was never coming, and reported success.
void DispatchAndApply(WorkspaceShell& shell,
                      platform::IndexUpdateBatch batch,
                      const char* what) {
  const std::uint64_t before =
      WorkspaceShellTestAccess::LastAppliedProjectChangeGeneration(shell);
  Expect(WorkspaceShellTestAccess::DispatchFileIndexWatcherBatchForTesting(shell,
                                                                           std::move(batch)),
         std::string("the fixture must actually deliver a watcher batch (") + what + ")");
  DrainProjectChanges(shell);
  Expect(WorkspaceShellTestAccess::LastAppliedProjectChangeGeneration(shell) > before,
         std::string("the dispatched batch must actually be APPLIED, or every assertion "
                     "after it is about a sweep that never ran (") +
             what + ")");
  // The echo check answers with a content digest computed OFF the shell thread,
  // so the sweep can leave a path undecided and act when the digest lands. Wait
  // for it here rather than letting the drain's 10 ms sleeps decide: the
  // assertions below are about the verdict, not about how fast this machine is.
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

// A synthetic "this file was modified" batch, pushed through the LIVE watcher's
// dispatch so the shell's own change handling runs. The real watcher may or may not
// deliver an inotify event inside a test's window, and a test that asserts NOTHING
// happened cannot tell "the fix worked" from "the event never arrived" — so the
// event is made certain rather than waited for.
// Move `path`'s modification time somewhere unmistakably different. Both tests
// below depend on the STAT differing — that is the whole precondition for the
// content confirmation they exercise — and a rewrite that lands inside the same
// filesystem mtime tick as the previous one does not move it. Leaving that to
// timing made one of these pass or fail with machine load, which is the opposite
// of what a test for a content check should depend on.
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

// Regression: the watcher's "is this our own echo?" check compared mtime+size, so a
// byte-identical rewrite read as an external change. For a CLEAN buffer that reloaded
// the file and raised a "reloaded from disk" notice for a file nobody had changed;
// for a dirty one it raised the external-change banner, unprompted. An agent
// rewriting a file with the same bytes produces exactly this, which is why it is the
// common case rather than the exotic one.
// State that decides the banner assertions below, rendered only when one fails.
// These two tests have been seen to fail under heavy machine load — a real
// external change reported as our own echo — at roughly one run in five, and
// never reproducibly (TD-2026-09-29-311). Whatever the cause, the next
// occurrence should say which input it saw rather than only which banner was
// missing, because that is the difference between a fixture problem and a real
// suppression bug.
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
         // Which of the two ways "no banner" happens: the sweep found no open view
         // of this path at all (so it had nothing to reload and nothing to warn
         // about), or it found them and decided the change was our own echo.
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
  // Only the synthetic batches below may drive this test: the live inotify
  // watcher reacting to the fixture's own writes is a second, uncontrolled
  // source of batches (TD-2026-09-29-311). Unwatch also joins its threads, so
  // anything in flight has landed before the assertions run.
  Expect(WorkspaceShellTestAccess::QuiesceFileIndexWatcherForTesting(shell),
         "the fixture must have a live watcher to quiesce");
  DrainProjectChanges(shell);
  WorkspaceShellTestAccess::ClearEditorBannersForTesting(shell);

  // Rewrite with the SAME bytes: the mtime moves, the content does not.
  WriteFile(file_path, "same bytes\n");
  ForceDistinctModificationTime(file_path);
  DispatchAndApply(shell, BuildModifiedBatch(root, relative), "the identical-rewrite batch");

  Expect(!WorkspaceShellTestAccess::HasReloadedNoticeBanner(shell, file_path),
         "a byte-identical rewrite must not announce a reload of a file that did not change");
  Expect(!WorkspaceShellTestAccess::HasExternalChangeBanner(shell, file_path),
         "a byte-identical rewrite must not raise the external-change banner");
}

// The split-pane shape, which the single-view tests could not distinguish: the
// echo check walks EVERY view of the path, and each view used to re-read the file
// to confirm the mismatch. Two panes on one file therefore meant two full reads
// for one watcher event, and the answer was never shared. The read is now hoisted
// out of the loop, which only stays correct if every view still gets its own
// verdict and its own re-baseline — so assert the behaviour with two views open.
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
  // Only the synthetic batches below may drive this test: the live inotify
  // watcher reacting to the fixture's own writes is a second, uncontrolled
  // source of batches (TD-2026-09-29-311). Unwatch also joins its threads, so
  // anything in flight has landed before the assertions run.
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

  // And a real change still reaches both views after the hoist.
  WriteFile(file_path, "SAME BYTES\n");
  ForceDistinctModificationTime(file_path);
  DispatchAndApply(shell, BuildModifiedBatch(root, relative), "the real-rewrite batch");
  Expect(WorkspaceShellTestAccess::HasReloadedNoticeBanner(shell, file_path),
         WorkspaceShellTestAccess::HasReloadedNoticeBanner(shell, file_path)
             ? std::string()
             : "a real external change still reaches a split view" +
                   ExternalChangeDiagnostics(shell, file_path));
}

// The echo check's answer is a content digest, and computing it is a read of up to
// 8 MiB. Doing that on the shell thread is the stall the remote-projects design
// forbids ("hashing never happens on the shell thread"), and a `git checkout` that
// moves the mtime of every open file pays it once per file with the window frozen.
//
// What this pins is the MECHANISM, not the clock: the posted-read counter is
// monotonic, so "the digest went off-thread" is checkable without racing whether
// it has come back. A version that read inline would leave it unchanged.
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

  // Same bytes, moved mtime: the one case that cannot be settled by the stat and
  // therefore has to read.
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

  // A size change needs no digest, so it must not post a read at all — the stat
  // already answered, and reading to confirm what is already known is the cost
  // this whole path exists to avoid.
  const std::uint64_t reads_after_confirm = WorkspaceShellTestAccess::PostedFileReadCount(shell);
  WriteFile(file_path, "different length entirely\n");
  ForceDistinctModificationTime(file_path);
  DispatchAndApply(shell, BuildModifiedBatch(root, relative), "the resized batch");
  Expect(WorkspaceShellTestAccess::PostedFileReadCount(shell) == reads_after_confirm,
         "a size change is decided by the stat alone, with no read posted");
  Expect(WorkspaceShellTestAccess::HasReloadedNoticeBanner(shell, file_path),
         "and it still reaches the clean buffer as a reload");

  // The case where the second read actually happened: same SIZE, different bytes,
  // over a CLEAN buffer. The digest says "changed", so the sweep reloads — and the
  // reload used to re-ask the echo question for itself, reading the same file a
  // second time on the shell thread to learn what the sweep had just settled. A
  // size change (above) never reached that read, which is why asserting it there
  // proved nothing.
  WriteFile(file_path, "DIFFERENT LENGTH ENTIRELY\n");
  ForceDistinctModificationTime(file_path);
  DispatchAndApply(shell, BuildModifiedBatch(root, relative), "the same-size rewrite batch");
  Expect(WorkspaceShellTestAccess::PostedFileReadCount(shell) == reads_after_confirm + 1,
         "the same-size rewrite posts one digest");
  Expect(BufferText(WorkspaceShellTestAccess::ActiveEditor(shell)) ==
             "DIFFERENT LENGTH ENTIRELY\n",
         "and the clean buffer is reloaded with the new content");

  // The whole sweep did no confirming read on the shell thread. That is the point
  // of the exercise, and it is a separate claim from "a read was posted".
  Expect(util::ReadPerformanceCounter(
             util::PerfCounterId::ExternalChangeConfirmInlineReads) == inline_reads_before,
         "no confirming read ran on the shell thread");
}

// The other half: a real external change must still reach the user. Without it the
// fix above could pass by suppressing everything.
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
  // Only the synthetic batches below may drive this test: the live inotify
  // watcher reacting to the fixture's own writes is a second, uncontrolled
  // source of batches (TD-2026-09-29-311). Unwatch also joins its threads, so
  // anything in flight has landed before the assertions run.
  Expect(WorkspaceShellTestAccess::QuiesceFileIndexWatcherForTesting(shell),
         "the fixture must have a live watcher to quiesce");
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("dirty ");
  DrainProjectChanges(shell);

  // Same LENGTH, different bytes — the case a content hash must not wave through.
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

// TD-2026-07-17A-015: an external clean reload replaces an open buffer's content
// and re-syncs the LSP server with a full didChange. That re-sync used to snapshot
// BOTH the pre-reload and the reloaded buffer into whole-document vectors; it now
// captures only the pre-reload line count + first differing line (compared while
// both buffers exist) and streams the didChange from the reloaded viewport, so a
// large external reload copies neither buffer.
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

  // Rewrite on disk with a DIFFERENT line count so the reload takes the diagnostic-
  // shift delta path (first differing line + net line delta), not just the equal-
  // count early-out. The first ~250 lines are unchanged so first_changed is > 0.
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
  // 250 "line i" rows + the new tail + a trailing empty line the editor tracks.
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
  AddTest(tests, "ExternalRepoChange/CleanReloadDoesNotSnapshotBuffers",
          TestWorkspaceShellCleanReloadDoesNotSnapshotBuffers);
  AddTest(tests, "ExternalRepoChange/ReloadsBothSplitGroups",
          TestWorkspaceShellExternalChangeReloadsBothSplitGroups);
  AddTest(tests, "ExternalRepoChange/BannerForDirtyBuffer",
          TestWorkspaceShellExternalChangeBannerForDirtyBuffer);
  AddTest(tests, "ExternalRepoChange/SelfWriteDoesNotRaiseBanner",
          TestWorkspaceShellSelfWriteDoesNotRaiseBanner);
  AddTest(tests, "ExternalRepoChange/AutosaveFlushRespectsDiskConflict",
          TestWorkspaceShellAutosaveFlushRespectsDiskConflict);
  AddTest(tests, "ExternalRepoChange/IdenticalRewriteRaisesNoReloadNotice",
          TestWorkspaceShellIdenticalRewriteRaisesNoReloadNotice);
  AddTest(tests, "ExternalRepoChange/IdenticalRewriteWithSplitViewsReadsOnce",
          TestWorkspaceShellIdenticalRewriteWithSplitViewsReadsOnce);
  AddTest(tests, "ExternalRepoChange/IdenticalRewriteConfirmsOffTheShellThread",
          TestWorkspaceShellIdenticalRewriteConfirmsOffTheShellThread);
  AddTest(tests, "ExternalRepoChange/RealRewriteStillNotifies",
          TestWorkspaceShellRealRewriteStillNotifies);
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
