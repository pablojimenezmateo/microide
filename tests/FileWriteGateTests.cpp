#include "TestSupport.h"

#include "editor/TextViewport.h"
#include "project/FileWriteGate.h"

#include <filesystem>
#include <string>
#include <vector>

namespace microide::tests {
namespace {

using microide::project::FileWriteGate;
using microide::project::LocalFileWriteGate;

void TestLocalGateWritesAndReportsTheSignature() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path file = temp_dir.path() / "note.txt";

  const FileWriteGate::Result created = LocalFileWriteGate().WriteText(file, "one\n");
  Expect(created.ok, "the gate writes a file that did not exist");
  Expect(ReadFile(file) == "one\n", "the gate writes the text it was given");
  // The signature is the whole reason the gate returns anything: it is what the
  // caller records so the watcher's echo of this very write is recognised instead
  // of read as an external change. Captured by the gate, so no caller re-stats.
  Expect(created.signature.exists, "the returned signature describes an existing file");
  Expect(created.signature.SameContentAs(util::StatFileSignature(file)),
         "the gate's signature matches the file it just wrote");

  const FileWriteGate::Result replaced = LocalFileWriteGate().WriteText(file, "two two\n");
  Expect(replaced.ok && ReadFile(file) == "two two\n", "the gate replaces existing contents");
  Expect(!replaced.signature.SameContentAs(created.signature),
         "a write that changed the file reports a different signature");
}

void TestLocalGateReportsFailureWithoutTouchingTheFile() {
  TemporaryDirectory temp_dir;
  // A directory is not a file the gate can replace, and the attempt must not claim
  // success — a caller that believed it would clear its dirty flag on a buffer that
  // was never written.
  const std::filesystem::path directory = temp_dir.path() / "not-a-file";
  std::filesystem::create_directories(directory);

  const FileWriteGate::Result result = LocalFileWriteGate().WriteText(directory, "x\n");
  Expect(!result.ok, "writing over a directory must fail");
  Expect(!result.signature.exists,
         "a failed write reports no signature: the caller's existing one still describes "
         "what is on disk");
  Expect(std::filesystem::is_directory(directory), "the failed write left the path alone");
}

// The editor save is the gate's biggest caller, and the property that matters is
// that the signature it records afterwards is the one the gate captured — that is
// what makes the next DetectDiskConflict compare against what was actually written
// rather than against a second, later stat.
void TestEditorSaveRecordsTheGatesSignature() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path file = temp_dir.path() / "buffer.txt";
  WriteFile(file, "original\n");

  editor::TextViewport viewport;
  Expect(viewport.OpenFile(file), "the fixture file opens");
  viewport.InsertText("edited ");
  Expect(viewport.dirty(), "the buffer is dirty before saving");
  Expect(viewport.Save(), "the save succeeds through the gate");

  Expect(ReadFile(file) == "edited original\n", "the save wrote through the gate");
  Expect(!viewport.dirty(), "a saved buffer is clean");
  Expect(viewport.DetectDiskConflict() == editor::TextViewport::DiskConflict::None,
         "the signature recorded from the gate matches the file on disk, so the very "
         "next conflict check does not report the save as an external change");
}

// Regression: a stat mismatch is not a content change. A `touch`, a `git checkout`
// that restored byte-identical content, and an external formatter that produced no
// change all move the mtime and leave the file exactly as it was — and the editor
// refused the save and raised an external-change banner for every one of them.
// Rewriting a file with the same bytes is what an agent editing alongside you does,
// so this was the common case, not the exotic one.
void TestIdenticalRewriteIsNotADiskConflict() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path file = temp_dir.path() / "touched.txt";
  WriteFile(file, "same bytes\n");

  editor::TextViewport viewport;
  Expect(viewport.OpenFile(file), "the fixture file opens");
  Expect(viewport.DetectDiskConflict() == editor::TextViewport::DiskConflict::None,
         "an untouched file is not a conflict");

  // Rewrite with IDENTICAL content. The mtime moves; the bytes do not.
  WriteFile(file, "same bytes\n");
  Expect(!util::StatFileSignature(file).SameContentAs(viewport.disk_signature()),
         "the fixture must actually move the stat, or this test proves nothing");
  Expect(viewport.DetectDiskConflict() == editor::TextViewport::DiskConflict::None,
         "a byte-identical rewrite is not a disk conflict");

  // …and the re-baseline means the next check does not have to read the file again
  // to reach the same answer.
  Expect(util::StatFileSignature(file).SameContentAs(viewport.disk_signature()),
         "a confirmed-unchanged file is re-baselined to the new stat");
}

// The other half: a real change must still be caught. Same size, different bytes —
// the case mtime+size alone would also catch, and the case a content hash must not
// wave through.
void TestRealEditIsStillADiskConflict() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path file = temp_dir.path() / "edited.txt";
  WriteFile(file, "same bytes\n");

  editor::TextViewport viewport;
  Expect(viewport.OpenFile(file), "the fixture file opens");
  WriteFile(file, "SAME BYTES\n");  // identical length, different content
  Expect(viewport.DetectDiskConflict() == editor::TextViewport::DiskConflict::Changed,
         "a same-length content change is still a conflict");

  // A different length is a change too, and is answered without reading the file.
  WriteFile(file, "much longer content than before\n");
  Expect(viewport.DetectDiskConflict() == editor::TextViewport::DiskConflict::Changed,
         "a different-length change is still a conflict");
}

// A save records the hash of what it wrote, so the save's own mtime bump is not
// mistaken for someone else's edit on the very next check.
void TestSaveRecordsContentSoTheNextCheckIsClean() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path file = temp_dir.path() / "saved.txt";
  WriteFile(file, "before\n");

  editor::TextViewport viewport;
  Expect(viewport.OpenFile(file), "the fixture file opens");
  viewport.InsertText("after ");
  Expect(viewport.Save(), "the save succeeds");
  Expect(viewport.disk_signature().has_content_hash,
         "a save records the content it wrote, not just a stat");

  // An identical external rewrite after the save is still recognised as no change.
  WriteFile(file, ReadFile(file));
  Expect(viewport.DetectDiskConflict() == editor::TextViewport::DiskConflict::None,
         "the hash recorded by the save answers the next identical rewrite too");
}

}  // namespace

void RegisterFileWriteGateTests(std::vector<TestCase>& tests) {
  AddTest(tests, "FileWriteGate/LocalGateWritesAndReportsTheSignature",
          TestLocalGateWritesAndReportsTheSignature);
  AddTest(tests, "FileWriteGate/LocalGateReportsFailureWithoutTouchingTheFile",
          TestLocalGateReportsFailureWithoutTouchingTheFile);
  AddTest(tests, "FileWriteGate/EditorSaveRecordsTheGatesSignature",
          TestEditorSaveRecordsTheGatesSignature);
  AddTest(tests, "FileWriteGate/IdenticalRewriteIsNotADiskConflict",
          TestIdenticalRewriteIsNotADiskConflict);
  AddTest(tests, "FileWriteGate/RealEditIsStillADiskConflict",
          TestRealEditIsStillADiskConflict);
  AddTest(tests, "FileWriteGate/SaveRecordsContentSoTheNextCheckIsClean",
          TestSaveRecordsContentSoTheNextCheckIsClean);
}

}  // namespace microide::tests
