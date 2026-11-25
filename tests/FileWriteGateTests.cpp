#include "TestSupport.h"

#include "editor/TextViewport.h"
#include "workspace/shell/WorkspaceShell.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"
#include "project/FileWriteGate.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace microide::tests {
namespace {

using microide::project::FileWriteGate;
using microide::project::LocalFileWriteGate;

void ForceDistinctModificationTime(const std::filesystem::path& path) {
  std::error_code error;
  std::filesystem::last_write_time(
      path, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(2), error);
}

void TestLocalGateWritesAndReportsTheSignature() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path file = temp_dir.path() / "note.txt";

  const FileWriteGate::Result created =
      LocalFileWriteGate().WriteText(file, "one\n", FileWriteGate::Signature::Capture);
  Expect(created.ok, "the gate writes a file that did not exist");
  Expect(ReadFile(file) == "one\n", "the gate writes the text it was given");
  Expect(created.signature.exists, "the returned signature describes an existing file");
  Expect(created.signature.SameStatAs(util::StatFileSignature(file)),
         "the gate's signature matches the file it just wrote");

  const FileWriteGate::Result replaced =
      LocalFileWriteGate().WriteText(file, "two two\n", FileWriteGate::Signature::Capture);
  Expect(replaced.ok && ReadFile(file) == "two two\n", "the gate replaces existing contents");
  Expect(!replaced.signature.SameContentAs(created.signature),
         "a write that changed the file reports a different signature");
}

void TestLocalGateReportsFailureWithoutTouchingTheFile() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path directory = temp_dir.path() / "not-a-file";
  std::filesystem::create_directories(directory);

  const FileWriteGate::Result result =
      LocalFileWriteGate().WriteText(directory, "x\n", FileWriteGate::Signature::Capture);
  Expect(!result.ok, "writing over a directory must fail");
  Expect(!result.signature.exists,
         "a failed write reports no signature: the caller's existing one still describes "
         "what is on disk");
  Expect(std::filesystem::is_directory(directory), "the failed write left the path alone");
}

void TestEditorSaveRecordsTheGatesSignature() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path file = temp_dir.path() / "buffer.txt";
  WriteFile(file, "original\n");

  editor::TextViewport viewport;
  Expect(viewport.OpenFile(file), "the fixture file opens");
  viewport.InsertText("edited ");
  Expect(viewport.dirty(), "the buffer is dirty before saving");
  Expect(viewport.Save(microide::project::LocalFileWriteGate()), "the save succeeds through the gate");

  Expect(ReadFile(file) == "edited original\n", "the save wrote through the gate");
  Expect(!viewport.dirty(), "a saved buffer is clean");
  Expect(viewport.DetectDiskConflict() == editor::TextViewport::DiskConflict::None,
         "the signature recorded from the gate matches the file on disk, so the very "
         "next conflict check does not report the save as an external change");
}

void TestASaveWhereNoFileWasNeverReplacesOneThatAppeared() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path original = temp_dir.path() / "original.txt";
  const std::filesystem::path file = temp_dir.path() / "new.txt";
  WriteFile(original, "mine\n");
  editor::TextViewport viewport;
  Expect(viewport.OpenFile(original), "the fixture opens");
  viewport.RebindToPath(file);
  WriteFile(file, "theirs\n");
  std::error_code error;
  project::FileWriteGate& gate = LocalFileWriteGate();
  Expect(!viewport.Save(gate, &error), "the save is refused");
  Expect(error == std::errc::file_exists, "because a file appeared there: " + error.message());
  Expect(ReadFile(file) == "theirs\n", "and that file is untouched");

  std::filesystem::remove(file);
  Expect(viewport.Save(gate, &error), "with the name free again, it saves: " + error.message());
  Expect(ReadFile(file) == "mine\n", "and creates the file: '" + ReadFile(file) + "'");
  Expect(ReadFile(original) == "mine\n", "the original is left as it was");
  viewport.InsertText("more ");
  Expect(viewport.Save(gate, &error), "the next save replaces its own file: " + error.message());
}

void TestIdenticalRewriteIsNotADiskConflict() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path file = temp_dir.path() / "touched.txt";
  WriteFile(file, "same bytes\n");

  editor::TextViewport viewport;
  Expect(viewport.OpenFile(file), "the fixture file opens");
  Expect(viewport.DetectDiskConflict() == editor::TextViewport::DiskConflict::None,
         "an untouched file is not a conflict");

  WriteFile(file, "same bytes\n");
  ForceDistinctModificationTime(file);
  Expect(!util::StatFileSignature(file).SameStatAs(viewport.disk_signature()),
         "the fixture must actually move the stat, or this test proves nothing");
  Expect(viewport.DetectDiskConflict() == editor::TextViewport::DiskConflict::None,
         "a byte-identical rewrite is not a disk conflict");

  Expect(util::StatFileSignature(file).SameStatAs(viewport.disk_signature()),
         "a confirmed-unchanged file is re-baselined to the new stat");
}

void TestSameTickRewriteIsStillADiskConflict() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path file = temp_dir.path() / "racy.txt";
  WriteFile(file, "same bytes\n");
  editor::TextViewport viewport;
  Expect(viewport.OpenFile(file), "the fixture file opens");
  const auto opened_mtime = std::filesystem::last_write_time(file);
  WriteFile(file, "SAME BYTES\n");
  std::filesystem::last_write_time(file, opened_mtime);
  Expect(util::StatFileSignature(file).SameStatAs(viewport.disk_signature()),
         "the fixture leaves the stat exactly as it was, or this test proves nothing");
  Expect(viewport.DetectDiskConflict() == editor::TextViewport::DiskConflict::Changed,
         "a same-tick, same-size rewrite is still a conflict");
}

void TestAReadThatRacedAWriterIsNotTheFile() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path file = temp_dir.path() / "streamed.txt";
  std::string whole;
  for (int i = 0; i < 2000; ++i) {
    whole += std::to_string(i) + " the quick brown fox\n";
  }
  WriteFile(file, whole);
  std::filesystem::last_write_time(file, std::filesystem::last_write_time(file) - std::chrono::seconds(30));
  editor::TextViewport viewport;
  Expect(viewport.AdoptFileContent(file, whole.substr(0, whole.size() / 2)), "the prefix the read got loads");
  Expect(viewport.DetectDiskConflict() == editor::TextViewport::DiskConflict::Changed,
         "the file on disk is not what the buffer read: it changed");
}

void TestRealEditIsStillADiskConflict() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path file = temp_dir.path() / "edited.txt";
  WriteFile(file, "same bytes\n");

  editor::TextViewport viewport;
  Expect(viewport.OpenFile(file), "the fixture file opens");
  WriteFile(file, "SAME BYTES\n");
  ForceDistinctModificationTime(file);
  Expect(viewport.DetectDiskConflict() == editor::TextViewport::DiskConflict::Changed,
         "a same-length content change is still a conflict");

  WriteFile(file, "much longer content than before\n");
  Expect(viewport.DetectDiskConflict() == editor::TextViewport::DiskConflict::Changed,
         "a different-length change is still a conflict");
}

void TestSaveRecordsContentSoTheNextCheckIsClean() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path file = temp_dir.path() / "saved.txt";
  WriteFile(file, "before\n");

  editor::TextViewport viewport;
  Expect(viewport.OpenFile(file), "the fixture file opens");
  viewport.InsertText("after ");
  Expect(viewport.Save(microide::project::LocalFileWriteGate()), "the save succeeds");
  Expect(viewport.disk_signature().has_content_hash,
         "a save records the content it wrote, not just a stat");

  WriteFile(file, ReadFile(file));
  ForceDistinctModificationTime(file);
  Expect(viewport.DetectDiskConflict() == editor::TextViewport::DiskConflict::None,
         "the hash recorded by the save answers the next identical rewrite too");
}

void TestIdenticalRewriteIsNotAConflictForADirtyBuffer() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path file = temp_dir.path() / "dirty.txt";
  WriteFile(file, "same bytes\n");

  editor::TextViewport viewport;
  Expect(viewport.OpenFile(file), "the fixture file opens");
  viewport.InsertText("unsaved edit ");
  Expect(viewport.dirty(), "the buffer differs from the file, which is the point");

  WriteFile(file, "same bytes\n");
  ForceDistinctModificationTime(file);
  Expect(viewport.DetectDiskConflict() == editor::TextViewport::DiskConflict::None,
         "a byte-identical rewrite is not a conflict even though the dirty buffer "
         "differs from the file");

  WriteFile(file, "SAME BYTES\n");
  ForceDistinctModificationTime(file);
  Expect(viewport.DetectDiskConflict() == editor::TextViewport::DiskConflict::Changed,
         "a real external edit still conflicts with a dirty buffer");
}

void TestGateSkipsTheSignatureStatByDefault() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path file = temp_dir.path() / "bulk.txt";

  const FileWriteGate::Result written = LocalFileWriteGate().WriteText(file, "bulk\n");
  Expect(written.ok && ReadFile(file) == "bulk\n", "the write still happens");
  Expect(!written.signature.exists,
         "a caller that did not ask for the signature does not pay for the stat");
}

class RecordingGate final : public FileWriteGate {
 public:
  std::vector<std::filesystem::path> writes;
  Result WriteText(const std::filesystem::path& path, std::string_view text, Signature signature,
                   util::WriteTarget target) override {
    writes.push_back(path);
    return LocalFileWriteGate().WriteText(path, text, signature, target);
  }
  TreeResult ApplyTreeOps(std::span<const TreeOp> ops) override {
    return LocalFileWriteGate().ApplyTreeOps(ops);
  }
  void DisposeStaged(std::span<const std::filesystem::path> staged) override {
    LocalFileWriteGate().DisposeStaged(staged);
  }
};

void TestAnEditorSaveGoesThroughTheProjectsGateEvenAfterAReload() {
  using microide::workspace::WorkspaceShell;
  using TA = WorkspaceShell::TestAccess;
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file = root / "note.txt";
  WriteFile(file, "one\n");

  WorkspaceShell shell;
  TA::SetProjectRoot(shell, root);
  RecordingGate gate;
  TA::SetProjectWriteGate(shell, gate);
  Expect(TA::OpenFileInNewTab(shell, file), "open the file");
  Expect(TA::HandleTextInput(shell, "x"), "edit it");
  Expect(TA::SaveTab(shell, TA::GroupActiveTabIndex(shell, 0)), "save it");
  Expect(gate.writes.size() == 1 && gate.writes.front() == file,
         "the save went through the project's gate");

  WriteFile(file, "changed on disk\n");
  TA::ReloadCleanEditorTabsForPath(shell, file);
  Expect(TA::ActiveEditor(shell).lines().LineView(0) == "changed on disk",
         "the clean buffer was reloaded — its viewport replaced");
  Expect(TA::HandleTextInput(shell, "y"), "edit again");
  Expect(TA::SaveTab(shell, TA::GroupActiveTabIndex(shell, 0)), "save again");
  Expect(gate.writes.size() == 2,
         "and the save after the reload still went through the project's gate");
}

void TestReloadingIdenticalContentKeepsTheEditHistory() {
  using microide::workspace::WorkspaceShell;
  using TA = WorkspaceShell::TestAccess;
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file = root / "note.txt";
  WriteFile(file, "one\n");

  WorkspaceShell shell;
  TA::SetProjectRoot(shell, root);
  Expect(TA::OpenFileInNewTab(shell, file), "open the file");
  Expect(TA::HandleTextInput(shell, "x"), "edit it");
  Expect(TA::SaveTab(shell, TA::GroupActiveTabIndex(shell, 0)), "save the edit");
  Expect(TA::ExecuteAction(shell, WorkspaceShell::ActionId::Undo, {}), "undo it");
  Expect(TA::SaveTab(shell, TA::GroupActiveTabIndex(shell, 0)), "save the undo");
  Expect(ReadFile(file) == "one\n", "the file holds the undone text");

  TA::ReloadCleanEditorTabsForPath(shell, file);
  Expect(TA::ExecuteAction(shell, WorkspaceShell::ActionId::Redo, {}), "redo");
  Expect(TA::ActiveEditor(shell).lines().LineView(0) == "xone",
         "a reload of identical bytes must keep the redo history");

  Expect(TA::SaveTab(shell, TA::GroupActiveTabIndex(shell, 0)), "save the redo");
  WriteFile(file, "changed on disk\n");
  TA::ReloadCleanEditorTabsForPath(shell, file);
  Expect(TA::ActiveEditor(shell).lines().LineView(0) == "changed on disk",
         "a file that really changed is reloaded");
}

void TestFileWriteGateRefusesANameWithANul() {
  TemporaryDirectory temp;
  FileWriteGate& gate = LocalFileWriteGate();
  const std::filesystem::path cut = temp.path() / "echo";
  const std::filesystem::path named = temp.path() / std::string("echo\0tail", 9);
  const auto written = gate.WriteText(named, "x\n");
  Expect(!written.ok, "a write to a name with a NUL is refused");
  Expect(!std::filesystem::exists(cut), "and nothing is written under the name cut short at it");
  WriteFile(temp.path() / "a.txt", "a\n");
  const std::vector<FileWriteGate::TreeOp> rename = {FileWriteGate::TreeOp{
      .kind = FileWriteGate::TreeOp::Kind::Rename, .path = temp.path() / "a.txt", .new_path = named}};
  const auto renamed = gate.ApplyTreeOps(rename);
  Expect(!renamed.ok && renamed.error_message.find("NUL") != std::string::npos,
         "a rename to such a name is refused, saying why: " + renamed.error_message);
  Expect(std::filesystem::exists(temp.path() / "a.txt") && !std::filesystem::exists(cut),
         "and the file stays where it was");
}

}

void RegisterFileWriteGateTests(std::vector<TestCase>& tests) {
  AddTest(tests, "FileWriteGate/RefusesANameWithANul", TestFileWriteGateRefusesANameWithANul);
  AddTest(tests, "FileWriteGate/AReadThatRacedAWriterIsNotTheFile", TestAReadThatRacedAWriterIsNotTheFile);
  AddTest(tests, "FileWriteGate/SameTickRewriteIsStillADiskConflict",
          TestSameTickRewriteIsStillADiskConflict);
  AddTest(tests, "FileWriteGate/LocalGateWritesAndReportsTheSignature",
          TestLocalGateWritesAndReportsTheSignature);
  AddTest(tests, "FileWriteGate/LocalGateReportsFailureWithoutTouchingTheFile",
          TestLocalGateReportsFailureWithoutTouchingTheFile);
  AddTest(tests, "FileWriteGate/EditorSaveRecordsTheGatesSignature",
          TestEditorSaveRecordsTheGatesSignature);
  AddTest(tests, "FileWriteGate/ASaveWhereNoFileWasNeverReplacesOneThatAppeared",
          TestASaveWhereNoFileWasNeverReplacesOneThatAppeared);
  AddTest(tests, "FileWriteGate/IdenticalRewriteIsNotADiskConflict",
          TestIdenticalRewriteIsNotADiskConflict);
  AddTest(tests, "FileWriteGate/RealEditIsStillADiskConflict",
          TestRealEditIsStillADiskConflict);
  AddTest(tests, "FileWriteGate/SaveRecordsContentSoTheNextCheckIsClean",
          TestSaveRecordsContentSoTheNextCheckIsClean);
  AddTest(tests, "FileWriteGate/IdenticalRewriteIsNotAConflictForADirtyBuffer",
          TestIdenticalRewriteIsNotAConflictForADirtyBuffer);
  AddTest(tests, "FileWriteGate/ReloadingIdenticalContentKeepsTheEditHistory",
          TestReloadingIdenticalContentKeepsTheEditHistory);
  AddTest(tests, "FileWriteGate/AnEditorSaveGoesThroughTheProjectsGateEvenAfterAReload",
          TestAnEditorSaveGoesThroughTheProjectsGateEvenAfterAReload);
  AddTest(tests, "FileWriteGate/GateSkipsTheSignatureStatByDefault",
          TestGateSkipsTheSignatureStatByDefault);
}

}
