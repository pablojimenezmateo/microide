#include "TestSupport.h"

#include "workspace/shell/WorkspaceShell.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"

#include <filesystem>
#include <string_view>
#include <string>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::WorkspaceShell;
using WorkspaceShellTestAccess = microide::workspace::WorkspaceShell::TestAccess;

// A formatter is any program that reads the buffer on stdin and writes the formatted
// text on stdout — so `sed` is a perfectly good one, and it needs no plugin, no node
// and no network. `tr` and `false` cover the other two outcomes.
std::vector<std::string> UppercasingFormatter() {
  return {"sed", "s/hello/HELLO/"};
}

std::filesystem::path OpenOneFileProject(WorkspaceShell& shell,
                                         const TemporaryDirectory& temp_dir,
                                         std::string_view contents) {
  const std::filesystem::path root = temp_dir.path() / "project";
  std::filesystem::create_directories(root);
  const std::filesystem::path file = root / "main.txt";
  WriteFile(file, std::string(contents));
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, file);
  return file;
}

// The headline: an interactive save hands the formatter to a worker and returns. The
// file is written when the formatter comes back, with its output applied.
void TestDeferredSaveAppliesTheFormatterWhenItReturns() {
  TemporaryDirectory temp_dir;
  WorkspaceShell shell;
  const std::filesystem::path file = OpenOneFileProject(shell, temp_dir, "hello world\n");
  WorkspaceShellTestAccess::RegisterFormatterForTesting(
      shell, std::string(WorkspaceShellTestAccess::ActiveEditor(shell).language_id()),
      UppercasingFormatter());
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("x");

  Expect(WorkspaceShellTestAccess::SaveTabDeferred(shell, 0),
         "a deferred save reports that it started");
  // Nothing has been written yet: that is the whole point, and asserting it is what
  // stops this from silently becoming a synchronous save again.
  Expect(ReadFile(file) == "hello world\n",
         "a deferred save must not have written before its formatter returned");

  WorkspaceShellTestAccess::FlushPendingSaveFormatters(shell);
  WorkspaceShellTestAccess::DrainSaveFormatterCompletions(shell);

  Expect(ReadFile(file) == "xHELLO world\n",
         "the completion applies the formatter's output and writes, got: " + ReadFile(file));
  Expect(!WorkspaceShellTestAccess::ActiveEditor(shell).dirty(),
         "the buffer is clean once the deferred save has written");
}

// The revision guard. A formatter's answer is about the buffer as it was when the run
// started; applying it over an edit the user made meanwhile would silently undo that
// edit. The buffer as it stands is what gets written instead.
void TestDeferredSaveDropsTheFormatterWhenTheBufferChanged() {
  TemporaryDirectory temp_dir;
  WorkspaceShell shell;
  const std::filesystem::path file = OpenOneFileProject(shell, temp_dir, "hello world\n");
  WorkspaceShellTestAccess::RegisterFormatterForTesting(
      shell, std::string(WorkspaceShellTestAccess::ActiveEditor(shell).language_id()),
      UppercasingFormatter());
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("x");

  Expect(WorkspaceShellTestAccess::SaveTabDeferred(shell, 0), "the deferred save started");
  // The user keeps typing while the formatter runs.
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("y");

  WorkspaceShellTestAccess::FlushPendingSaveFormatters(shell);
  WorkspaceShellTestAccess::DrainSaveFormatterCompletions(shell);

  Expect(ReadFile(file) == "xyhello world\n",
         "a buffer edited during formatting is written as it stands, unformatted, got: " +
             ReadFile(file));
  Expect(!WorkspaceShellTestAccess::ActiveEditor(shell).dirty(),
         "the save still completes; only the formatter's answer is dropped");
}

// A formatter that fails must not cost the user their save. The file is written
// unformatted and the failure is surfaced, which is what the inline path did too.
void TestDeferredSaveStillWritesWhenTheFormatterFails() {
  TemporaryDirectory temp_dir;
  WorkspaceShell shell;
  const std::filesystem::path file = OpenOneFileProject(shell, temp_dir, "hello world\n");
  WorkspaceShellTestAccess::RegisterFormatterForTesting(
      shell, std::string(WorkspaceShellTestAccess::ActiveEditor(shell).language_id()),
      {"false"});
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("x");

  Expect(WorkspaceShellTestAccess::SaveTabDeferred(shell, 0), "the deferred save started");
  WorkspaceShellTestAccess::FlushPendingSaveFormatters(shell);
  WorkspaceShellTestAccess::DrainSaveFormatterCompletions(shell);

  Expect(ReadFile(file) == "xhello world\n",
         "a failed formatter still saves the buffer, got: " + ReadFile(file));
  Expect(!WorkspaceShellTestAccess::ActiveEditor(shell).dirty(),
         "a failed formatter must not leave the tab dirty");
}

// What the formatter actually said. The subprocess layer captured stderr all
// along and the save pipeline dropped it, so a failing formatter produced
// "Formatter 'x' failed; saved unformatted" and nothing else — no exit status,
// no parse error, no line number. For a formatter that fails on ONE file and not
// the others, that text is the only thing that answers why.
void TestAFailingFormatterReportsWhatItSaid() {
  TemporaryDirectory temp_dir;
  WorkspaceShell shell;
  const std::filesystem::path file = OpenOneFileProject(shell, temp_dir, "hello world\n");
  WorkspaceShellTestAccess::RegisterFormatterForTesting(
      shell, std::string(WorkspaceShellTestAccess::ActiveEditor(shell).language_id()),
      {"sh", "-c", "echo 'SyntaxError: unexpected token on line 3' >&2; exit 2"});
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("x");

  Expect(WorkspaceShellTestAccess::SaveTabDeferred(shell, 0), "the deferred save started");
  WorkspaceShellTestAccess::FlushPendingSaveFormatters(shell);
  WorkspaceShellTestAccess::DrainSaveFormatterCompletions(shell);

  // Still saved, unformatted — the existing contract, unchanged.
  Expect(ReadFile(file) == "xhello world\n",
         "a failed formatter still saves the buffer, got: " + ReadFile(file));

  // The toast carries the first line, because a toast that says only "failed"
  // sends the user looking for a panel they have no reason to know exists.
  bool toast_names_the_error = false;
  for (const auto& row : WorkspaceShellTestAccess::ActiveNotifications(shell)) {
    if (row.message.find("SyntaxError: unexpected token on line 3") != std::string::npos) {
      toast_names_the_error = true;
    }
  }
  Expect(toast_names_the_error,
         "the warning must say what the formatter said, not just that it failed");

  // And the whole of it is in a channel the user can scroll.
  const std::vector<std::string>* entries =
      WorkspaceShellTestAccess::OutputChannelEntries(shell, "formatter");
  Expect(entries != nullptr && !entries->empty(),
         "a failing formatter opens an output channel with its output");
  bool channel_has_the_error = false;
  for (const std::string& line : *entries) {
    if (line.find("SyntaxError: unexpected token on line 3") != std::string::npos) {
      channel_has_the_error = true;
    }
  }
  Expect(channel_has_the_error, "the channel holds the formatter's stderr");

  // And the toast says where: a "Show Output" button that opens that channel.
  const auto& rows = WorkspaceShellTestAccess::ActiveNotifications(shell);
  std::size_t row_index = rows.size();
  for (std::size_t i = 0; i < rows.size(); ++i) {
    if (rows[i].actions.size() == 1 && rows[i].actions[0].label == "Show Output") {
      row_index = i;
    }
  }
  Expect(row_index < rows.size(), "the formatter warning carries a Show Output button");
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(
             shell, "notification-action " + std::to_string(row_index) + " 0"),
         "pressing Show Output runs");
  Expect(WorkspaceShellTestAccess::PanelContent(shell) == WorkspaceShell::PanelContentKind::Output &&
             WorkspaceShellTestAccess::OutputChannelTabOrder(shell).size() >= 1 &&
             WorkspaceShellTestAccess::OutputChannelTabOrder(shell).back() == "formatter",
         "Show Output opens the formatter channel in the panel");
  Expect(!WorkspaceShellTestAccess::ExecuteCommandLine(shell, "show-output no-such-channel"),
         "show-output refuses a channel that does not exist");
}

// Closing a tab must not drop a save that is still formatting. The flush is what
// makes that true, and without it the write never happens at all.
void TestClosingATabFlushesItsDeferredSave() {
  TemporaryDirectory temp_dir;
  WorkspaceShell shell;
  const std::filesystem::path file = OpenOneFileProject(shell, temp_dir, "hello world\n");
  WorkspaceShellTestAccess::RegisterFormatterForTesting(
      shell, std::string(WorkspaceShellTestAccess::ActiveEditor(shell).language_id()),
      UppercasingFormatter());
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("x");

  Expect(WorkspaceShellTestAccess::SaveTabDeferred(shell, 0), "the deferred save started");
  WorkspaceShellTestAccess::CloseTab(shell, 0);

  Expect(ReadFile(file) == "xHELLO world\n",
         "closing the tab must let the in-flight save finish first, got: " + ReadFile(file));
}

// The close prompt's Save. It used to save in SaveMode::Blocking and WAIT, so
// choosing Save on one dirty JS file froze the window for as long as node took to
// start (TD-2026-09-28-304). It defers now — but the CLOSE has to wait for the
// write even though the shell thread does not, because closing before the write
// lands would discard exactly the edits the user just asked to keep.
void TestSaveThenCloseWaitsForTheWriteWithoutBlocking() {
  TemporaryDirectory temp_dir;
  WorkspaceShell shell;
  const std::filesystem::path file = OpenOneFileProject(shell, temp_dir, "hello world\n");
  WorkspaceShellTestAccess::RegisterFormatterForTesting(
      shell, std::string(WorkspaceShellTestAccess::ActiveEditor(shell).language_id()),
      UppercasingFormatter());
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("x");
  const std::size_t tabs_before = WorkspaceShellTestAccess::FocusedGroupOpenTabCount(shell);
  Expect(tabs_before == 1, "the fixture opens exactly one tab");

  Expect(WorkspaceShellTestAccess::SaveThenCloseTab(shell, 0),
         "save-then-close reports that it started");
  // Both halves of the point, and both must be asserted: the shell thread came
  // back (we are here) with nothing written, AND the tab is still open, because
  // closing it now would drop the edits.
  Expect(ReadFile(file) == "hello world\n",
         "save-then-close must not have written before the formatter returned");
  Expect(WorkspaceShellTestAccess::FocusedGroupOpenTabCount(shell) == 1,
         "the tab stays open until its write lands — closing early discards the edits");

  WorkspaceShellTestAccess::FlushPendingSaveFormatters(shell);
  WorkspaceShellTestAccess::DrainSaveFormatterCompletions(shell);

  Expect(ReadFile(file) == "xHELLO world\n",
         "the completion writes the formatted buffer, got: " + ReadFile(file));
  Expect(WorkspaceShellTestAccess::FocusedGroupOpenTabCount(shell) == 0,
         "and only then does the tab close");
}

// With no formatter there is nothing to defer, so save-then-close is the same
// immediate close it always was — no lingering tab waiting for a completion that
// will never come.
void TestSaveThenCloseIsImmediateWithoutAFormatter() {
  TemporaryDirectory temp_dir;
  WorkspaceShell shell;
  const std::filesystem::path file = OpenOneFileProject(shell, temp_dir, "hello world\n");
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("x");

  Expect(WorkspaceShellTestAccess::SaveThenCloseTab(shell, 0), "save-then-close succeeds");
  Expect(ReadFile(file) == "xhello world\n",
         "the file is written before it returns, got: " + ReadFile(file));
  Expect(WorkspaceShellTestAccess::FocusedGroupOpenTabCount(shell) == 0,
         "and the tab is already closed");
}

// The invariant every exit path quietly depends on. A deferred save has not
// written yet, so the buffer MUST still report dirty until it does — that is what
// makes quit, close-project and switch-project prompt over it instead of walking
// past a buffer whose write is still in flight. Nothing states this at the sites
// that rely on it, so it is pinned here: a deferral that cleared the dirty flag
// early would turn every one of those prompts into silent data loss.
void TestADeferredSaveLeavesTheBufferDirtyUntilItWrites() {
  TemporaryDirectory temp_dir;
  WorkspaceShell shell;
  const std::filesystem::path file = OpenOneFileProject(shell, temp_dir, "hello world\n");
  WorkspaceShellTestAccess::RegisterFormatterForTesting(
      shell, std::string(WorkspaceShellTestAccess::ActiveEditor(shell).language_id()),
      UppercasingFormatter());
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("x");

  Expect(WorkspaceShellTestAccess::SaveTabDeferred(shell, 0), "the deferred save started");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).dirty(),
         "a deferred save has not written, so the buffer is still dirty and every "
         "exit path still prompts over it");
  // The property that actually matters, end to end: asking to quit while the
  // write is in flight still raises the unsaved-changes prompt rather than
  // walking past a buffer whose save has not landed.
  WorkspaceShellTestAccess::ShowDirtyPromptForQuit(shell);
  Expect(WorkspaceShellTestAccess::DirtyPromptVisible(shell),
         "quitting mid-deferral must still prompt over the unwritten buffer");

  WorkspaceShellTestAccess::FlushPendingSaveFormatters(shell);
  WorkspaceShellTestAccess::DrainSaveFormatterCompletions(shell);
  Expect(!WorkspaceShellTestAccess::ActiveEditor(shell).dirty(),
         "and only once the write lands is it clean");
}

// Close All over several dirty buffers. It used to run every formatter inline
// and wait for all of them, so closing a handful of JS files froze the window
// once per file (TD-2026-09-28-304). Each tab now closes when its OWN write
// lands.
void TestCloseAllDirtyTabsDefersEveryFormatter() {
  TemporaryDirectory temp_dir;
  WorkspaceShell shell;
  const std::filesystem::path root = temp_dir.path() / "project";
  std::filesystem::create_directories(root);
  const std::filesystem::path first = root / "first.txt";
  const std::filesystem::path second = root / "second.txt";
  WriteFile(first, "hello one\n");
  WriteFile(second, "hello two\n");
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, first);
  WorkspaceShellTestAccess::RegisterFormatterForTesting(
      shell, std::string(WorkspaceShellTestAccess::ActiveEditor(shell).language_id()),
      UppercasingFormatter());
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("x");
  WorkspaceShellTestAccess::OpenFile(shell, second);
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("y");
  Expect(WorkspaceShellTestAccess::FocusedGroupOpenTabCount(shell) == 2,
         "the fixture opens two dirty tabs");

  Expect(WorkspaceShellTestAccess::ExecuteCloseAllTabs(shell), "Close All runs");
  Expect(WorkspaceShellTestAccess::DirtyPromptVisible(shell),
         "two dirty buffers raise the unsaved-changes prompt");
  WorkspaceShellTestAccess::ConfirmDirtyPrompt(shell, 0);  // Save

  // Neither write has happened and neither tab has closed: both formatters are
  // on the worker and the shell thread came back without waiting for either.
  Expect(ReadFile(first) == "hello one\n" && ReadFile(second) == "hello two\n",
         "no file is written before its formatter returns");
  Expect(WorkspaceShellTestAccess::FocusedGroupOpenTabCount(shell) == 2,
         "and no tab closes before its own write lands");

  WorkspaceShellTestAccess::FlushPendingSaveFormatters(shell);
  WorkspaceShellTestAccess::DrainSaveFormatterCompletions(shell);

  Expect(ReadFile(first) == "xHELLO one\n",
         "the first file is written formatted, got: " + ReadFile(first));
  Expect(ReadFile(second) == "yHELLO two\n",
         "the second file is written formatted, got: " + ReadFile(second));
  Expect(WorkspaceShellTestAccess::FocusedGroupOpenTabCount(shell) == 0,
         "and both tabs are closed once their writes land");
}

// A blocking save is what every caller that acts on completion still gets: the file
// is on disk by the time it returns, formatter and all.
void TestBlockingSaveWritesFormattedBeforeItReturns() {
  TemporaryDirectory temp_dir;
  WorkspaceShell shell;
  const std::filesystem::path file = OpenOneFileProject(shell, temp_dir, "hello world\n");
  WorkspaceShellTestAccess::RegisterFormatterForTesting(
      shell, std::string(WorkspaceShellTestAccess::ActiveEditor(shell).language_id()),
      UppercasingFormatter());
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("x");

  Expect(WorkspaceShellTestAccess::SaveTab(shell, 0), "a blocking save reports success");
  Expect(ReadFile(file) == "xHELLO world\n",
         "a blocking save has written the formatted text when it returns, got: " +
             ReadFile(file));
}

// Two dirty buffers with a formatter, in a real project tab.
struct TwoDirtyFormattedFiles {
  std::filesystem::path root;
  std::filesystem::path first;
  std::filesystem::path second;
};

TwoDirtyFormattedFiles OpenTwoDirtyFormattedFiles(WorkspaceShell& shell,
                                                  const TemporaryDirectory& temp_dir) {
  TwoDirtyFormattedFiles files{.root = temp_dir.path() / "project"};
  files.first = files.root / "first.txt";
  files.second = files.root / "second.txt";
  WriteFile(files.first, "hello one\n");
  WriteFile(files.second, "hello two\n");
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, files.root, false, false),
         "the fixture opens its project");
  WorkspaceShellTestAccess::OpenFile(shell, files.first);
  WorkspaceShellTestAccess::RegisterFormatterForTesting(
      shell, std::string(WorkspaceShellTestAccess::ActiveEditor(shell).language_id()),
      UppercasingFormatter());
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("x");
  WorkspaceShellTestAccess::OpenFile(shell, files.second);
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("y");
  return files;
}

const microide::workspace::NotificationService::Notification* RowWithKey(
    WorkspaceShell& shell, std::string_view key) {
  for (const auto& row : WorkspaceShellTestAccess::ActiveNotifications(shell)) {
    if (row.key == key) {
      return &row;
    }
  }
  return nullptr;
}

// Quit with Save All no longer runs the formatters on the shell thread: it waits
// behind a progress row with a Cancel button, and quits once the last write lands.
void TestQuitWaitsForFormatterSavesBehindAProgressRow() {
  TemporaryDirectory temp_dir;
  WorkspaceShell shell;
  const TwoDirtyFormattedFiles files = OpenTwoDirtyFormattedFiles(shell, temp_dir);

  WorkspaceShellTestAccess::ShowDirtyPromptForQuit(shell);
  WorkspaceShellTestAccess::ConfirmDirtyPrompt(shell, 0);  // Save All

  Expect(ReadFile(files.first) == "hello one\n" && ReadFile(files.second) == "hello two\n",
         "nothing is written before the formatters return: the shell thread came back");
  Expect(!shell.ConsumeQuitRequested(), "and the quit waits for the writes");
  const auto* row = RowWithKey(shell, "save.wait.quit");
  Expect(row != nullptr && row->sticky && row->progress.has_value() &&
             row->message.find("2 files") != std::string::npos,
         "a sticky progress row says what it is waiting for");
  Expect(row != nullptr && row->actions.size() == 1 && row->actions[0].label == "Cancel",
         "and offers Cancel");

  WorkspaceShellTestAccess::FlushPendingSaveFormatters(shell);
  WorkspaceShellTestAccess::DrainSaveFormatterCompletions(shell);

  Expect(ReadFile(files.first) == "xHELLO one\n" && ReadFile(files.second) == "yHELLO two\n",
         "both files are written formatted");
  Expect(shell.ConsumeQuitRequested(), "and then the application quits");
  Expect(RowWithKey(shell, "save.wait.quit") == nullptr, "the progress row is gone");
}

// Cancel aborts the quit: the application stays, the buffers whose saves had not
// landed stay open and dirty, and the abandoned writes never happen.
void TestCancellingTheQuitLeavesBuffersDirty() {
  TemporaryDirectory temp_dir;
  WorkspaceShell shell;
  const TwoDirtyFormattedFiles files = OpenTwoDirtyFormattedFiles(shell, temp_dir);

  WorkspaceShellTestAccess::ShowDirtyPromptForQuit(shell);
  WorkspaceShellTestAccess::ConfirmDirtyPrompt(shell, 0);
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell,
                                                      "notification-action save.wait.quit Cancel"),
         "Cancel on the progress row runs");
  Expect(RowWithKey(shell, "save.wait.quit") == nullptr, "the progress row is gone");

  WorkspaceShellTestAccess::FlushPendingSaveFormatters(shell);
  WorkspaceShellTestAccess::DrainSaveFormatterCompletions(shell);

  Expect(!shell.ConsumeQuitRequested(), "the application does not quit");
  Expect(ReadFile(files.first) == "hello one\n" && ReadFile(files.second) == "hello two\n",
         "the abandoned writes never land");
  Expect(WorkspaceShellTestAccess::FocusedGroupOpenTabCount(shell) == 2 &&
             WorkspaceShellTestAccess::ActiveEditor(shell).dirty(),
         "the buffers are still open and dirty");
}

// Closing a project waits the same way, and the project's state is not torn down
// before its last write lands.
void TestCloseProjectWaitsForItsWrites() {
  TemporaryDirectory temp_dir;
  WorkspaceShell shell;
  const TwoDirtyFormattedFiles files = OpenTwoDirtyFormattedFiles(shell, temp_dir);
  const std::size_t projects_before = WorkspaceShellTestAccess::ProjectCount(shell);

  WorkspaceShellTestAccess::RequestCloseProject(shell, projects_before - 1);
  Expect(WorkspaceShellTestAccess::DirtyPromptVisible(shell), "the close asks about the edits");
  WorkspaceShellTestAccess::ConfirmDirtyPrompt(shell, 0);

  Expect(WorkspaceShellTestAccess::ProjectCount(shell) == projects_before,
         "the project stays open while its formatters run");
  Expect(RowWithKey(shell, "save.wait.close-project") != nullptr,
         "behind a progress row with Cancel");

  WorkspaceShellTestAccess::FlushPendingSaveFormatters(shell);
  WorkspaceShellTestAccess::DrainSaveFormatterCompletions(shell);

  Expect(ReadFile(files.first) == "xHELLO one\n" && ReadFile(files.second) == "yHELLO two\n",
         "both files are written formatted");
  Expect(WorkspaceShellTestAccess::ProjectCount(shell) == projects_before - 1,
         "and then the project closes");
}

// A rename waiting on a save that the external-change guard refuses is cancelled:
// the file is not renamed, and a notification says why.
void TestARefusedSaveCancelsTheRename() {
  TemporaryDirectory temp_dir;
  WorkspaceShell shell;
  const TwoDirtyFormattedFiles files = OpenTwoDirtyFormattedFiles(shell, temp_dir);
  const std::filesystem::path renamed = files.root / "renamed.txt";

  WorkspaceShellTestAccess::OpenPromptSurfaceForTest(
      shell, workspace::PromptSurfaceState::Action::RenamePath,
      workspace::PromptSurfaceState::Kind::TextInput, files.second, "", "renamed.txt");
  WorkspaceShellTestAccess::ConfirmPromptSurfaceSavingDirtyBuffers(shell);
  Expect(std::filesystem::exists(files.second) && !std::filesystem::exists(renamed),
         "the rename waits for its write");

  // Someone else writes the file while its formatter runs: the save must not
  // clobber it, so it is refused — and the rename with it.
  WriteFile(files.second, "changed on disk by someone else, longer than before\n");
  WorkspaceShellTestAccess::FlushPendingSaveFormatters(shell);
  Expect(WorkspaceShellTestAccess::HasExternalChangeBanner(shell, files.second),
         "the formatted save is refused and the external-change banner raised instead");
  WorkspaceShellTestAccess::DrainSaveFormatterCompletions(shell);

  Expect(std::filesystem::exists(files.second) && !std::filesystem::exists(renamed),
         "the file is not renamed");
  Expect(ReadFile(files.second) == "changed on disk by someone else, longer than before\n",
         "and the other writer's bytes are not clobbered");
  bool told = false;
  for (const auto& row : WorkspaceShellTestAccess::ActiveNotifications(shell)) {
    told = told || row.message.find("rename/delete was not applied") != std::string::npos;
  }
  Expect(told, "a notification says the rename was not applied");
}

// The file changes on disk while its formatter runs. Applying the formatter's
// output used to re-stat the file and adopt what it found as the new conflict
// baseline, so the save right after it overwrote the other writer's bytes without
// a word. The baseline is the one the buffer was loaded against; the save is
// refused and the external-change banner raised.
void TestAnExternalChangeDuringFormattingIsNotOverwritten() {
  TemporaryDirectory temp_dir;
  WorkspaceShell shell;
  const std::filesystem::path file = OpenOneFileProject(shell, temp_dir, "hello world\n");
  WorkspaceShellTestAccess::RegisterFormatterForTesting(
      shell, std::string(WorkspaceShellTestAccess::ActiveEditor(shell).language_id()),
      UppercasingFormatter());
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("x");
  Expect(WorkspaceShellTestAccess::SaveTabDeferred(shell, 0), "the deferred save started");

  WriteFile(file, "someone else's newer, longer content\n");
  WorkspaceShellTestAccess::FlushPendingSaveFormatters(shell);
  WorkspaceShellTestAccess::DrainSaveFormatterCompletions(shell);

  Expect(ReadFile(file) == "someone else's newer, longer content\n",
         "the other writer's bytes survive, got: " + ReadFile(file));
  Expect(WorkspaceShellTestAccess::HasExternalChangeBanner(shell, file),
         "the user is asked instead (external-change banner)");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).dirty(), "the buffer keeps its edits");
}

// Same hole on the blocking path, where the conflict check runs BEFORE the
// formatter: a formatter that is itself the other writer (it rewrites the file
// while it runs) must not have its write clobbered by the save it was part of.
void TestAnExternalChangeDuringABlockingFormatIsNotOverwritten() {
  TemporaryDirectory temp_dir;
  WorkspaceShell shell;
  const std::filesystem::path file = OpenOneFileProject(shell, temp_dir, "hello world\n");
  WorkspaceShellTestAccess::RegisterFormatterForTesting(
      shell, std::string(WorkspaceShellTestAccess::ActiveEditor(shell).language_id()),
      {"sh", "-c", "printf 'written by someone else meanwhile\\n' > '" + file.string() +
                       "'; sed s/hello/HELLO/"});
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("x");
  Expect(!WorkspaceShellTestAccess::SaveTab(shell, 0), "the blocking save is refused");
  Expect(ReadFile(file) == "written by someone else meanwhile\n",
         "the bytes written while the formatter ran survive, got: " + ReadFile(file));
}

}  // namespace

void RegisterSaveFormatterPipelineTests(std::vector<TestCase>& tests) {
  AddTest(tests, "SaveFormatterPipeline/CloseAllDirtyTabsDefersEveryFormatter",
          TestCloseAllDirtyTabsDefersEveryFormatter);
  AddTest(tests, "SaveFormatterPipeline/ADeferredSaveLeavesTheBufferDirtyUntilItWrites",
          TestADeferredSaveLeavesTheBufferDirtyUntilItWrites);
  AddTest(tests, "SaveFormatterPipeline/AnExternalChangeDuringFormattingIsNotOverwritten",
          TestAnExternalChangeDuringFormattingIsNotOverwritten);
  AddTest(tests, "SaveFormatterPipeline/AnExternalChangeDuringABlockingFormatIsNotOverwritten",
          TestAnExternalChangeDuringABlockingFormatIsNotOverwritten);
  AddTest(tests, "SaveFormatterPipeline/QuitWaitsForFormatterSavesBehindAProgressRow",
          TestQuitWaitsForFormatterSavesBehindAProgressRow);
  AddTest(tests, "SaveFormatterPipeline/CancellingTheQuitLeavesBuffersDirty",
          TestCancellingTheQuitLeavesBuffersDirty);
  AddTest(tests, "SaveFormatterPipeline/CloseProjectWaitsForItsWrites",
          TestCloseProjectWaitsForItsWrites);
  AddTest(tests, "SaveFormatterPipeline/ARefusedSaveCancelsTheRename",
          TestARefusedSaveCancelsTheRename);
  AddTest(tests, "SaveFormatterPipeline/SaveThenCloseWaitsForTheWriteWithoutBlocking",
          TestSaveThenCloseWaitsForTheWriteWithoutBlocking);
  AddTest(tests, "SaveFormatterPipeline/SaveThenCloseIsImmediateWithoutAFormatter",
          TestSaveThenCloseIsImmediateWithoutAFormatter);
  AddTest(tests, "SaveFormatterPipeline/DeferredSaveAppliesTheFormatterWhenItReturns",
          TestDeferredSaveAppliesTheFormatterWhenItReturns);
  AddTest(tests, "SaveFormatterPipeline/DeferredSaveDropsTheFormatterWhenTheBufferChanged",
          TestDeferredSaveDropsTheFormatterWhenTheBufferChanged);
  AddTest(tests, "SaveFormatterPipeline/DeferredSaveStillWritesWhenTheFormatterFails",
          TestDeferredSaveStillWritesWhenTheFormatterFails);
  AddTest(tests, "SaveFormatterPipeline/AFailingFormatterReportsWhatItSaid",
          TestAFailingFormatterReportsWhatItSaid);
  AddTest(tests, "SaveFormatterPipeline/ClosingATabFlushesItsDeferredSave",
          TestClosingATabFlushesItsDeferredSave);
  AddTest(tests, "SaveFormatterPipeline/BlockingSaveWritesFormattedBeforeItReturns",
          TestBlockingSaveWritesFormattedBeforeItReturns);
}

}  // namespace microide::tests
