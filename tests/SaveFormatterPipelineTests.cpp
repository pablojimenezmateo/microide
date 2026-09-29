#include "TestSupport.h"

#include "workspace/shell/WorkspaceShell.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"

#include <filesystem>
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

}  // namespace

void RegisterSaveFormatterPipelineTests(std::vector<TestCase>& tests) {
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
  AddTest(tests, "SaveFormatterPipeline/ClosingATabFlushesItsDeferredSave",
          TestClosingATabFlushesItsDeferredSave);
  AddTest(tests, "SaveFormatterPipeline/BlockingSaveWritesFormattedBeforeItReturns",
          TestBlockingSaveWritesFormattedBeforeItReturns);
}

}  // namespace microide::tests
