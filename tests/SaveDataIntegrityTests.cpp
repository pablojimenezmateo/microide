#include "TestSupport.h"

#include "editor/TextViewport.h"
#include "project/GitRepository.h"
#include "util/DurableFile.h"
#include "util/TextFileIO.h"

#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace microide::tests {
namespace {

using microide::editor::TextViewport;
using microide::util::WriteTextFileAtomically;

#if !defined(_WIN32)
// True when the current process can bypass DAC permission checks (root), which would
// defeat a "make the directory unwritable" failure-injection test.
bool RunningAsRoot() { return ::geteuid() == 0; }

// POSIX permission bits (mode & 07777) of an existing path, or -1 if it cannot be
// stat'd.
int PosixModeBits(const std::filesystem::path& path) {
  struct stat status{};
  if (::stat(path.c_str(), &status) != 0) {
    return -1;
  }
  return static_cast<int>(status.st_mode & 07777);
}

// (inode, hardlink count) of a path, or (0, 0) if it cannot be stat'd. Used to prove that an
// atomic save replaces the inode (and thus breaks a hardlink) rather than writing in place.
std::pair<ino_t, nlink_t> PosixInodeAndLinks(const std::filesystem::path& path) {
  struct stat status{};
  if (::stat(path.c_str(), &status) != 0) {
    return {0, 0};
  }
  return {status.st_ino, status.st_nlink};
}
#endif

// No directory entry beside `path` should be a leftover staging temp once a save
// settles. The staging name is "<file>.tmp.<pid>.<seq>" — note the trailing dot, so a
// stray exact "<file>.tmp" sibling used as a decoy elsewhere is intentionally excluded.
bool NoStagingTempLeftBehind(const std::filesystem::path& path) {
  const std::string stem = path.filename().string() + ".tmp.";
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(path.parent_path(), error)) {
    if (entry.path().filename().string().rfind(stem, 0) == 0) {
      return false;
    }
  }
  return true;
}

// A2/A3: the atomic writer stages to a unique per-write temp, so a pre-existing file
// that happens to share the old fixed ".tmp" name is neither consumed nor clobbered,
// and no staging temp survives the write.
void TestAtomicWriteUsesUniqueTempAndSpareTmpSurvives() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path path = temp_dir.path() / "doc.txt";
  Expect(WriteTextFileAtomically(path, "v1\n"), "initial atomic write should succeed");

  // A stray sibling named exactly "<path>.tmp" (the old fixed staging name) must be
  // left completely untouched by the unique-temp writer.
  const std::filesystem::path decoy = path.string() + ".tmp";
  WriteFile(decoy, "decoy-should-survive");

  Expect(WriteTextFileAtomically(path, "v2\n"), "overwrite should succeed");
  Expect(ReadFile(path) == "v2\n", "atomic write should replace the payload");
  Expect(std::filesystem::exists(decoy), "a foreign .tmp sibling must not be consumed");
  Expect(ReadFile(decoy) == "decoy-should-survive",
         "a foreign .tmp sibling must not be clobbered");
  Expect(NoStagingTempLeftBehind(path),
         "the unique staging temp must be renamed/removed, not left behind");
}

#if !defined(_WIN32)
// A1: an atomic save must preserve the destination's permission bits instead of
// resetting the file to a fresh 0644 inode. Executable and restrictive modes are the
// data-integrity-critical cases.
void TestAtomicWritePreservesFileMode() {
  TemporaryDirectory temp_dir;

  const std::filesystem::path secret = temp_dir.path() / "secret.txt";
  Expect(WriteTextFileAtomically(secret, "a\n"), "seed write should succeed");
  Expect(::chmod(secret.c_str(), 0600) == 0, "chmod 0600 should succeed");
  Expect(WriteTextFileAtomically(secret, "b\n"), "resave should succeed");
  Expect(PosixModeBits(secret) == 0600,
         "a restrictive 0600 file must keep its mode across an atomic save");

  const std::filesystem::path script = temp_dir.path() / "run.sh";
  Expect(WriteTextFileAtomically(script, "#!/bin/sh\n"), "seed write should succeed");
  Expect(::chmod(script.c_str(), 0755) == 0, "chmod 0755 should succeed");
  Expect(WriteTextFileAtomically(script, "#!/bin/sh\necho hi\n"), "resave should succeed");
  Expect(PosixModeBits(script) == 0755,
         "an executable 0755 file must keep its +x bits across an atomic save");
}

// A2: saving a symlinked file must update the link's target and keep the link itself,
// not replace the link node with a regular file.
void TestAtomicWriteThroughSymlinkPreservesLink() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path target = temp_dir.path() / "real.txt";
  Expect(WriteTextFileAtomically(target, "orig\n"), "seed target write should succeed");
  Expect(::chmod(target.c_str(), 0640) == 0, "chmod 0640 should succeed");

  const std::filesystem::path link = temp_dir.path() / "link.txt";
  std::error_code error;
  std::filesystem::create_symlink(target, link, error);
  Expect(!error, "creating the symlink should succeed");

  Expect(WriteTextFileAtomically(link, "updated\n"), "saving through a symlink should succeed");
  Expect(std::filesystem::is_symlink(std::filesystem::symlink_status(link)),
         "the symlink must survive the save, not be replaced by a regular file");
  Expect(ReadFile(target) == "updated\n", "the symlink's target must receive the new content");
  Expect(PosixModeBits(target) == 0640,
         "the target's mode must be preserved when saving through the link");
  Expect(NoStagingTempLeftBehind(link), "no staging temp should be left beside the link");
}

// C1: a failed save must not lose data — the buffer stays dirty, the on-disk file is
// left intact (not truncated), no staging temp lingers, and the undo baseline is NOT
// re-based (MarkSaved must not run), so undoing back to the loaded text reads clean.
void TestSaveWriteFailureKeepsDirtyAndFileIntact() {
  if (RunningAsRoot()) {
    return;  // root bypasses the directory-permission failure injection.
  }
  TemporaryDirectory temp_dir;
  const std::filesystem::path path = temp_dir.path() / "doc.txt";
  WriteFile(path, "original\n");

  TextViewport viewport;
  Expect(viewport.OpenFile(path), "opening the seed file should succeed");
  Expect(!viewport.dirty(), "a freshly opened file is clean");

  viewport.InsertText("X");
  Expect(viewport.dirty(), "an edit marks the buffer dirty");

  // Make the parent directory unwritable so staging the temp fails.
  Expect(::chmod(temp_dir.path().c_str(), 0500) == 0, "chmod dir 0500 should succeed");
  const bool saved = viewport.Save();
  Expect(::chmod(temp_dir.path().c_str(), 0700) == 0, "restoring dir perms should succeed");

  Expect(!saved, "a save into an unwritable directory must fail");
  Expect(viewport.dirty(), "a failed save must leave the buffer dirty (no silent data loss)");
  Expect(ReadFile(path) == "original\n",
         "a failed save must leave the on-disk file untouched, not truncated");
  Expect(NoStagingTempLeftBehind(path), "a failed save must not leave a staging temp behind");

  // The failed save must not have re-baselined the undo history: undoing back to the
  // loaded content reports clean, proving MarkSaved did not run on the failure path.
  Expect(viewport.Undo(), "undo should reverse the edit");
  Expect(viewport.lines().Snapshot() == std::vector<std::string>({"original", ""}) ||
             viewport.lines().Snapshot() == std::vector<std::string>({"original"}),
         "undo restores the original loaded content");
  Expect(!viewport.dirty(),
         "after a failed save, undoing to the loaded content must read clean (MarkSaved not applied)");
}
#endif  // !_WIN32

// C3: an in-place round-trip through the editor must preserve the file's original line
// ending. Opening canonicalizes CRLF/CR to an LF buffer; saving must restore the
// detected ending byte-for-byte.
void TestSavePreservesCrlfLineEnding() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path path = temp_dir.path() / "crlf.txt";
  WriteFile(path, "a\r\nb\r\n");

  TextViewport viewport;
  Expect(viewport.OpenFile(path), "opening the CRLF file should succeed");
  Expect(viewport.Save(), "save should succeed");
  Expect(ReadFile(path) == "a\r\nb\r\n",
         "a clean CRLF round-trip must preserve CRLF endings on disk");

  viewport.InsertText("X");
  Expect(viewport.Save(), "save after an edit should succeed");
  Expect(ReadFile(path) == "Xa\r\nb\r\n",
         "an edited CRLF file must still be written back with CRLF endings");
}

void TestSavePreservesCrOnlyLineEnding() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path path = temp_dir.path() / "cr.txt";
  WriteFile(path, "a\rb\r");

  TextViewport viewport;
  Expect(viewport.OpenFile(path), "opening the CR file should succeed");
  Expect(viewport.Save(), "save should succeed");
  Expect(ReadFile(path) == "a\rb\r", "a clean CR round-trip must preserve CR endings on disk");
}

// C3: an explicit SetSaveLineEnding override must actually rewrite the on-disk endings.
void TestSaveLineEndingOverrideRewritesEndings() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path path = temp_dir.path() / "lf.txt";
  WriteFile(path, "a\nb\n");

  TextViewport viewport;
  Expect(viewport.OpenFile(path), "opening the LF file should succeed");
  viewport.SetSaveLineEnding(microide::util::LineEnding::CRLF);
  Expect(viewport.Save(), "save with a CRLF override should succeed");
  Expect(ReadFile(path) == "a\r\nb\r\n",
         "an explicit CRLF save override must rewrite LF endings to CRLF on disk");
}

// C11: a file with no trailing newline must stay that way across an in-place round-trip,
// and a file that ends in a newline must keep exactly one. The line-based buffer models a
// trailing newline as a final empty line, so this pins that the split/join is byte-exact
// and that save-normalization stays off by default (POSIX tools and git treat a spurious
// or missing final newline as a real content change).
void TestSavePreservesTrailingNewlinePresence() {
  TemporaryDirectory temp_dir;

  const std::filesystem::path no_nl = temp_dir.path() / "no_final_nl.txt";
  WriteFile(no_nl, "a\nb");  // deliberately no trailing newline
  {
    TextViewport viewport;
    Expect(viewport.OpenFile(no_nl), "opening the no-final-newline file should succeed");
    Expect(viewport.Save(), "save should succeed");
    Expect(ReadFile(no_nl) == "a\nb",
           "a clean save must not fabricate a trailing newline");
    viewport.InsertText("X");
    Expect(viewport.Save(), "save after an edit should succeed");
    Expect(ReadFile(no_nl) == "Xa\nb",
           "an edited file with no trailing newline must stay newline-free at EOF");
  }

  const std::filesystem::path with_nl = temp_dir.path() / "with_final_nl.txt";
  WriteFile(with_nl, "a\nb\n");
  {
    TextViewport viewport;
    Expect(viewport.OpenFile(with_nl), "opening the trailing-newline file should succeed");
    Expect(viewport.Save(), "save should succeed");
    Expect(ReadFile(with_nl) == "a\nb\n",
           "a clean save must preserve exactly one trailing newline");
  }
}

// C12: binary / non-text ("Bytes"-encoded) content must survive an unedited open->save
// round-trip byte-for-byte. The editor canonicalizes CR/CRLF to an LF buffer for text, but
// for opaque bytes a 0x0D or 0x0A is payload, not a line ending -- rewriting it silently
// corrupts the file. Each case here is a distinct newline shape embedded in binary data.
void TestSaveBinaryContentRoundTripsExactly() {
  const std::string cases[] = {
      std::string("bin\0data\n", 9),               // NUL + a single LF
      std::string("a\0b\r\nc\0\n", 8),             // NUL + CRLF + lone LF (mixed)
      std::string("x\0y\rz\0", 6),                 // NUL + a lone CR (old-Mac-style byte)
      std::string("h\0\r\n\ni\0\r", 8),            // NUL + CRLF + LF + trailing CR (mixed)
      std::string("\0\0\0", 3),                    // pure NUL run, no newline at all
  };
  int index = 0;
  for (const std::string& payload : cases) {
    TemporaryDirectory temp_dir;
    const std::filesystem::path path =
        temp_dir.path() / ("bin_" + std::to_string(index++) + ".dat");
    WriteFile(path, payload);

    TextViewport viewport;
    Expect(viewport.OpenFile(path), "opening the binary file should succeed");
    Expect(viewport.encoding() == TextViewport::TextEncoding::Bytes,
           "a file with an embedded NUL must classify as Bytes");
    Expect(viewport.Save(), "saving unedited binary content should succeed");
    Expect(ReadFile(path) == payload,
           "an unedited save of binary content must reproduce the exact bytes");
  }
}

// #4: a UTF-8 BOM (EF BB BF) is valid UTF-8, so it stays content on line 0 rather than being
// stripped or misread as binary. A clean round-trip must reproduce the BOM byte-for-byte;
// losing it would corrupt files that tools (MSVC, some CSV readers) require a BOM on.
void TestSavePreservesUtf8Bom() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path path = temp_dir.path() / "bom.txt";
  const std::string bom = "\xEF\xBB\xBF";
  const std::string original = bom + "hello\nworld\n";
  WriteFile(path, original);

  TextViewport viewport;
  Expect(viewport.OpenFile(path), "opening a UTF-8 BOM file should succeed");
  Expect(viewport.encoding() == TextViewport::TextEncoding::UTF8,
         "a UTF-8 BOM is valid UTF-8 and must not be misclassified as binary");
  Expect(viewport.Save(), "clean save should succeed");
  Expect(ReadFile(path) == original, "a clean save must preserve the UTF-8 BOM bytes verbatim");
}

#if !defined(_WIN32)
// #5: the atomic temp+rename save deliberately replaces the file's inode, which breaks a
// hardlink -- other names for the old inode keep the pre-save content. This is the accepted
// trade-off for crash-atomic saves (a partial in-place write would risk corruption), so pin
// the contract explicitly: the saved path gets the new content on a fresh inode, and the
// sibling hardlink is left pointing at the original bytes rather than silently diverging
// half-written.
void TestAtomicSaveReplacesInodeAndBreaksHardlink() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path original_name = temp_dir.path() / "a.txt";
  const std::filesystem::path hardlink_name = temp_dir.path() / "b.txt";
  Expect(WriteTextFileAtomically(original_name, "shared\n"), "seed write should succeed");

  std::error_code error;
  std::filesystem::create_hard_link(original_name, hardlink_name, error);
  Expect(!error, "creating the hardlink should succeed");
  const auto [seed_ino, seed_links] = PosixInodeAndLinks(original_name);
  Expect(seed_links == 2, "precondition: the two names share one inode with link count 2");

  Expect(WriteTextFileAtomically(original_name, "rewritten\n"), "atomic resave should succeed");

  Expect(ReadFile(original_name) == "rewritten\n",
         "the saved path must hold the new content (no data loss on the file being saved)");
  const auto [new_ino, new_links] = PosixInodeAndLinks(original_name);
  Expect(new_ino != seed_ino, "an atomic save replaces the inode (rename swaps in a fresh file)");
  Expect(new_links == 1, "the saved path is a fresh single-linked inode after the rename");
  Expect(ReadFile(hardlink_name) == "shared\n",
         "the sibling hardlink keeps the original bytes intact, never a half-written file");
}
#endif  // !_WIN32

// C13: saving over a file that changed on disk since it was opened must be detectable so the
// higher layer can refuse and surface the reload/overwrite banner instead of silently
// clobbering the external change. DetectDiskConflict is the guard signal the coordinator's
// SaveTab consults (WorkspaceTabCoordinator.cpp), so pin its transitions here.
void TestSaveDetectsExternalChangeConflict() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path path = temp_dir.path() / "shared.txt";
  WriteFile(path, "loaded\n");

  TextViewport viewport;
  Expect(viewport.OpenFile(path), "opening the file should succeed");
  Expect(viewport.DetectDiskConflict() == TextViewport::DiskConflict::None,
         "a freshly opened file has no disk conflict");

  viewport.InsertText("X");
  // Another process rewrites the file after we opened and edited it.
  WriteFile(path, "changed-elsewhere\n");
  Expect(viewport.DetectDiskConflict() == TextViewport::DiskConflict::Changed,
         "an external write after open must be detected as a conflict (guards against clobber)");

  // The vanished case is data-loss-adjacent too: the file we would overwrite is gone.
  std::error_code error;
  std::filesystem::remove(path, error);
  Expect(viewport.DetectDiskConflict() == TextViewport::DiskConflict::Vanished,
         "a file deleted on disk after open must be reported as vanished");

  // After our own successful save the signature re-baselines, so there must be no false
  // conflict that would nag the user or block the next legitimate save.
  Expect(viewport.Save(), "save should succeed and re-record the disk signature");
  Expect(viewport.DetectDiskConflict() == TextViewport::DiskConflict::None,
         "our own save must not leave a phantom disk conflict behind");
}

// C7: reloading a file from disk (the external-change reload mechanism) must reset the
// undo/redo history. Otherwise a post-reload undo/redo could resurrect pre-reload content
// that no longer matches disk, silently diverging the buffer from the file.
void TestReloadResetsUndoHistory() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path path = temp_dir.path() / "reload.txt";
  WriteFile(path, "a\n");

  TextViewport viewport;
  Expect(viewport.OpenFile(path), "initial open should succeed");
  viewport.InsertText("x");
  Expect(viewport.dirty(), "an edit dirties the buffer");
  Expect(viewport.Undo(), "undo returns to the loaded content");
  Expect(!viewport.dirty(), "undone-to-loaded content is clean");

  // External change on disk, then reload through OpenFile (what the shell's clean-reload
  // path does under the hood: it replaces the viewport with a fresh OpenFile).
  WriteFile(path, "b\n");
  Expect(viewport.OpenFile(path), "reload should succeed");
  Expect(viewport.lines()[0] == "b", "reload replaces the buffer with the on-disk content");
  Expect(!viewport.dirty(), "a freshly reloaded buffer is clean");
  Expect(!viewport.Undo(), "reload must discard pre-reload undo history");
  Expect(!viewport.Redo(), "reload must discard pre-reload redo history");
  Expect(viewport.lines()[0] == "b",
         "no stale content can be resurrected: the buffer stays at the reloaded disk content");
}

// C9: discarding a working-tree change is a destructive (data-loss) operation, so it must
// restore the committed content byte-for-byte.
void TestGitDiscardRestoresExactCommittedBytes() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  std::filesystem::create_directories(root);
  InitializeGitRepo(root);
  const std::string committed = "committed line 1\ncommitted line 2\n";
  WriteFile(root / "f.txt", committed);
  CommitAll(root, "seed", "git discard fixture");

  WriteFile(root / "f.txt", "CLOBBERED\n");
  Expect(ReadFile(root / "f.txt") == "CLOBBERED\n", "precondition: the working tree is modified");

  microide::project::GitRepository repo(root);
  Expect(repo.Discard("f.txt"), "discarding the tracked file should succeed");
  Expect(ReadFile(root / "f.txt") == committed,
         "discard must restore the exact committed bytes, not an approximation");
}

void TestGitDiscardAllRestoresEveryFile() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  std::filesystem::create_directories(root);
  InitializeGitRepo(root);
  const std::string a_committed = "alpha\n";
  const std::string b_committed = "beta\nsecond\n";
  WriteFile(root / "a.txt", a_committed);
  WriteFile(root / "b.txt", b_committed);
  CommitAll(root, "seed", "git discard-all fixture");

  WriteFile(root / "a.txt", "A-CLOBBERED\n");
  WriteFile(root / "b.txt", "B-CLOBBERED\n");

  microide::project::GitRepository repo(root);
  Expect(repo.DiscardAll(), "discard-all should succeed");
  Expect(ReadFile(root / "a.txt") == a_committed, "discard-all must restore a.txt exactly");
  Expect(ReadFile(root / "b.txt") == b_committed, "discard-all must restore b.txt exactly");
}

}  // namespace

void RegisterSaveDataIntegrityTests(std::vector<TestCase>& tests) {
  AddTest(tests, "SaveDataIntegrity/AtomicWriteUsesUniqueTemp",
          TestAtomicWriteUsesUniqueTempAndSpareTmpSurvives);
#if !defined(_WIN32)
  AddTest(tests, "SaveDataIntegrity/AtomicWritePreservesFileMode",
          TestAtomicWritePreservesFileMode);
  AddTest(tests, "SaveDataIntegrity/AtomicWriteThroughSymlinkPreservesLink",
          TestAtomicWriteThroughSymlinkPreservesLink);
  AddTest(tests, "SaveDataIntegrity/SaveWriteFailureKeepsDirtyAndFileIntact",
          TestSaveWriteFailureKeepsDirtyAndFileIntact);
#endif
  AddTest(tests, "SaveDataIntegrity/SavePreservesCrlfLineEnding",
          TestSavePreservesCrlfLineEnding);
  AddTest(tests, "SaveDataIntegrity/SavePreservesCrOnlyLineEnding",
          TestSavePreservesCrOnlyLineEnding);
  AddTest(tests, "SaveDataIntegrity/SaveLineEndingOverrideRewritesEndings",
          TestSaveLineEndingOverrideRewritesEndings);
  AddTest(tests, "SaveDataIntegrity/SavePreservesTrailingNewlinePresence",
          TestSavePreservesTrailingNewlinePresence);
  AddTest(tests, "SaveDataIntegrity/SaveBinaryContentRoundTripsExactly",
          TestSaveBinaryContentRoundTripsExactly);
  AddTest(tests, "SaveDataIntegrity/SaveDetectsExternalChangeConflict",
          TestSaveDetectsExternalChangeConflict);
  AddTest(tests, "SaveDataIntegrity/SavePreservesUtf8Bom", TestSavePreservesUtf8Bom);
#if !defined(_WIN32)
  AddTest(tests, "SaveDataIntegrity/AtomicSaveReplacesInodeAndBreaksHardlink",
          TestAtomicSaveReplacesInodeAndBreaksHardlink);
#endif
  AddTest(tests, "SaveDataIntegrity/ReloadResetsUndoHistory", TestReloadResetsUndoHistory);
  AddTest(tests, "SaveDataIntegrity/GitDiscardRestoresExactCommittedBytes",
          TestGitDiscardRestoresExactCommittedBytes);
  AddTest(tests, "SaveDataIntegrity/GitDiscardAllRestoresEveryFile",
          TestGitDiscardAllRestoresEveryFile);
}

}  // namespace microide::tests
