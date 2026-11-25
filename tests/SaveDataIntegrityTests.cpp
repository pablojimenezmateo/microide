#include "TestSupport.h"

#include "editor/TextViewport.h"
#include "project/GitRepository.h"
#include "util/DurableFile.h"
#include "util/TextFileIO.h"

#include <cctype>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

#include <sys/stat.h>
#include <unistd.h>

namespace microide::tests {
namespace {

using microide::editor::TextViewport;
namespace util = microide::util;
using microide::util::WriteTextFileAtomically;

bool RunningAsRoot() { return ::geteuid() == 0; }

int PosixModeBits(const std::filesystem::path& path) {
  struct stat status{};
  if (::stat(path.c_str(), &status) != 0) {
    return -1;
  }
  return static_cast<int>(status.st_mode & 07777);
}

std::pair<ino_t, nlink_t> PosixInodeAndLinks(const std::filesystem::path& path) {
  struct stat status{};
  if (::stat(path.c_str(), &status) != 0) {
    return {0, 0};
  }
  return {status.st_ino, status.st_nlink};
}

bool NoStagingTempLeftBehind(const std::filesystem::path& path) {
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(path.parent_path(), error)) {
    if (util::IsTemporaryStagingFilename(entry.path().filename().string())) {
      return false;
    }
  }
  return true;
}

void TestStagingTempNameIsRecognizedByItsOwnPredicate() {
  const std::filesystem::path target = std::filesystem::path("/tmp/project") / "main.cpp";
  const std::filesystem::path staging = util::UniqueTemporaryPath(target);
  Expect(staging.parent_path() == target.parent_path(),
         "the staging temp must sit beside its target so the rename stays atomic");
  Expect(util::IsTemporaryStagingFilename(staging.filename().string()),
         "UniqueTemporaryPath output must be recognized by IsTemporaryStagingFilename");
  Expect(staging.filename().string().front() == '.',
         "the staging temp must be hidden so the default file index skips it");
  Expect(util::UniqueTemporaryPath(target) != staging,
         "two staging paths for the same target must be distinct");

  for (const char* name : {"build.tmp.json", "notes.tmp", ".gitignore", "main.cpp",
                           ".microide-staging"}) {
    Expect(!util::IsTemporaryStagingFilename(name),
           "an ordinary project file must not be mistaken for a staging temp");
  }
}

void TestAtomicWriteUsesUniqueTempAndSpareTmpSurvives() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path path = temp_dir.path() / "doc.txt";
  Expect(WriteTextFileAtomically(path, "v1\n"), "initial atomic write should succeed");

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

void TestAtomicWriteThroughSymlinkWithMissingTargetPreservesLink() {
  TemporaryDirectory temp_dir;
  std::error_code error;
  std::filesystem::create_directory(temp_dir.path() / "sub", error);
  Expect(!error, "creating the subdirectory should succeed");

  const std::filesystem::path link = temp_dir.path() / "link.txt";
  std::filesystem::create_symlink("sub/missing.txt", link, error);
  Expect(!error, "creating the dangling symlink should succeed");

  Expect(WriteTextFileAtomically(link, "created\n"),
         "saving through a dangling symlink should succeed");
  Expect(std::filesystem::is_symlink(std::filesystem::symlink_status(link)),
         "the symlink must survive the save, not be replaced by a regular file");
  const std::filesystem::path target = temp_dir.path() / "sub" / "missing.txt";
  Expect(std::filesystem::is_regular_file(target),
         "the link's target must be created as a regular file");
  Expect(ReadFile(target) == "created\n", "the link's target must receive the new content");
  Expect(NoStagingTempLeftBehind(link), "no staging temp should be left beside the link");
}

void TestSaveWriteFailureKeepsDirtyAndFileIntact() {
  if (RunningAsRoot()) {
    return;
  }
  TemporaryDirectory temp_dir;
  const std::filesystem::path path = temp_dir.path() / "doc.txt";
  WriteFile(path, "original\n");

  TextViewport viewport;
  Expect(viewport.OpenFile(path), "opening the seed file should succeed");
  Expect(!viewport.dirty(), "a freshly opened file is clean");

  viewport.InsertText("X");
  Expect(viewport.dirty(), "an edit marks the buffer dirty");

  Expect(::chmod(temp_dir.path().c_str(), 0500) == 0, "chmod dir 0500 should succeed");
  std::error_code why;
  const bool saved = viewport.Save(microide::project::LocalFileWriteGate(), &why);
  Expect(::chmod(temp_dir.path().c_str(), 0700) == 0, "restoring dir perms should succeed");

  Expect(!saved, "a save into an unwritable directory must fail");
  Expect(why == std::errc::permission_denied, "the failure says why: " + why.message());
  Expect(microide::util::DescribeWriteError(why) == "permission denied",
         "in the row's words: " + microide::util::DescribeWriteError(why));
  Expect(viewport.dirty(), "a failed save must leave the buffer dirty (no silent data loss)");
  Expect(ReadFile(path) == "original\n",
         "a failed save must leave the on-disk file untouched, not truncated");
  Expect(NoStagingTempLeftBehind(path), "a failed save must not leave a staging temp behind");

  Expect(viewport.Undo(), "undo should reverse the edit");
  Expect(viewport.lines().Snapshot() == std::vector<std::string>({"original", ""}) ||
             viewport.lines().Snapshot() == std::vector<std::string>({"original"}),
         "undo restores the original loaded content");
  Expect(!viewport.dirty(),
         "after a failed save, undoing to the loaded content must read clean (MarkSaved not applied)");
}

void TestSavePreservesCrlfLineEnding() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path path = temp_dir.path() / "crlf.txt";
  WriteFile(path, "a\r\nb\r\n");

  TextViewport viewport;
  Expect(viewport.OpenFile(path), "opening the CRLF file should succeed");
  Expect(viewport.Save(microide::project::LocalFileWriteGate()), "save should succeed");
  Expect(ReadFile(path) == "a\r\nb\r\n",
         "a clean CRLF round-trip must preserve CRLF endings on disk");

  viewport.InsertText("X");
  Expect(viewport.Save(microide::project::LocalFileWriteGate()), "save after an edit should succeed");
  Expect(ReadFile(path) == "Xa\r\nb\r\n",
         "an edited CRLF file must still be written back with CRLF endings");
}

void TestSavePreservesCrOnlyLineEnding() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path path = temp_dir.path() / "cr.txt";
  WriteFile(path, "a\rb\r");

  TextViewport viewport;
  Expect(viewport.OpenFile(path), "opening the CR file should succeed");
  Expect(viewport.Save(microide::project::LocalFileWriteGate()), "save should succeed");
  Expect(ReadFile(path) == "a\rb\r", "a clean CR round-trip must preserve CR endings on disk");
}

void TestSaveLineEndingOverrideRewritesEndings() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path path = temp_dir.path() / "lf.txt";
  WriteFile(path, "a\nb\n");

  TextViewport viewport;
  Expect(viewport.OpenFile(path), "opening the LF file should succeed");
  viewport.SetSaveLineEnding(microide::util::LineEnding::CRLF);
  Expect(viewport.Save(microide::project::LocalFileWriteGate()), "save with a CRLF override should succeed");
  Expect(ReadFile(path) == "a\r\nb\r\n",
         "an explicit CRLF save override must rewrite LF endings to CRLF on disk");
}

void TestSavePreservesTrailingNewlinePresence() {
  TemporaryDirectory temp_dir;

  const std::filesystem::path no_nl = temp_dir.path() / "no_final_nl.txt";
  WriteFile(no_nl, "a\nb");
  {
    TextViewport viewport;
    Expect(viewport.OpenFile(no_nl), "opening the no-final-newline file should succeed");
    Expect(viewport.Save(microide::project::LocalFileWriteGate()), "save should succeed");
    Expect(ReadFile(no_nl) == "a\nb",
           "a clean save must not fabricate a trailing newline");
    viewport.InsertText("X");
    Expect(viewport.Save(microide::project::LocalFileWriteGate()), "save after an edit should succeed");
    Expect(ReadFile(no_nl) == "Xa\nb",
           "an edited file with no trailing newline must stay newline-free at EOF");
  }

  const std::filesystem::path with_nl = temp_dir.path() / "with_final_nl.txt";
  WriteFile(with_nl, "a\nb\n");
  {
    TextViewport viewport;
    Expect(viewport.OpenFile(with_nl), "opening the trailing-newline file should succeed");
    Expect(viewport.Save(microide::project::LocalFileWriteGate()), "save should succeed");
    Expect(ReadFile(with_nl) == "a\nb\n",
           "a clean save must preserve exactly one trailing newline");
  }
}

void TestCleanSaveStreamsMultiLineBufferByteExact() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path path = temp_dir.path() / "multiline.txt";
  const std::string content = "alpha\nbeta\n\ngamma\n    indented\nlast line no nl";
  WriteFile(path, content);

  TextViewport viewport;
  Expect(viewport.OpenFile(path), "opening the multi-line file should succeed");
  Expect(viewport.Save(microide::project::LocalFileWriteGate()), "clean save should succeed");
  Expect(ReadFile(path) == content,
         "a clean multi-line save must reproduce the buffer byte-for-byte via streaming");

  Expect(viewport.Save(microide::project::LocalFileWriteGate()), "a second clean save should succeed");
  Expect(ReadFile(path) == content, "repeated clean saves stay byte-stable");
}

void TestSaveBinaryContentRoundTripsExactly() {
  const std::string cases[] = {
      std::string("bin\0data\n", 9),
      std::string("a\0b\r\nc\0\n", 8),
      std::string("x\0y\rz\0", 6),
      std::string("h\0\r\n\ni\0\r", 8),
      std::string("\0\0\0", 3),
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
    Expect(viewport.Save(microide::project::LocalFileWriteGate()), "saving unedited binary content should succeed");
    Expect(ReadFile(path) == payload,
           "an unedited save of binary content must reproduce the exact bytes");
  }
}

void TestSavePreservesUtf8Bom() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path path = temp_dir.path() / "bom.txt";
  const std::string bom = "\xEF\xBB\xBF";
  const std::string original = bom + "hello\nworld\n";
  WriteFile(path, original);

  TextViewport viewport;
  Expect(viewport.OpenFile(path), "opening a UTF-8 BOM file should succeed");
  Expect(viewport.encoding() != TextViewport::TextEncoding::Bytes,
         "a UTF-8 BOM is valid UTF-8 and must not be misclassified as binary");
  Expect(viewport.has_utf8_bom() && viewport.lines().LineView(0) == "hello",
         "the BOM is remembered and kept out of line 0");
  Expect(viewport.Save(microide::project::LocalFileWriteGate()), "clean save should succeed");
  Expect(ReadFile(path) == original, "a clean save must preserve the UTF-8 BOM bytes verbatim");
}

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

void TestSaveDetectsExternalChangeConflict() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path path = temp_dir.path() / "shared.txt";
  WriteFile(path, "loaded\n");

  TextViewport viewport;
  Expect(viewport.OpenFile(path), "opening the file should succeed");
  Expect(viewport.DetectDiskConflict() == TextViewport::DiskConflict::None,
         "a freshly opened file has no disk conflict");

  viewport.InsertText("X");
  WriteFile(path, "changed-elsewhere\n");
  Expect(viewport.DetectDiskConflict() == TextViewport::DiskConflict::Changed,
         "an external write after open must be detected as a conflict (guards against clobber)");

  std::error_code error;
  std::filesystem::remove(path, error);
  Expect(viewport.DetectDiskConflict() == TextViewport::DiskConflict::Vanished,
         "a file deleted on disk after open must be reported as vanished");

  Expect(viewport.Save(microide::project::LocalFileWriteGate()), "save should succeed and re-record the disk signature");
  Expect(viewport.DetectDiskConflict() == TextViewport::DiskConflict::None,
         "our own save must not leave a phantom disk conflict behind");
}

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

  WriteFile(path, "b\n");
  Expect(viewport.OpenFile(path), "reload should succeed");
  Expect(viewport.lines()[0] == "b", "reload replaces the buffer with the on-disk content");
  Expect(!viewport.dirty(), "a freshly reloaded buffer is clean");
  Expect(!viewport.Undo(), "reload must discard pre-reload undo history");
  Expect(!viewport.Redo(), "reload must discard pre-reload redo history");
  Expect(viewport.lines()[0] == "b",
         "no stale content can be resurrected: the buffer stays at the reloaded disk content");
}

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

  microide::project::GitRepository repo(root, microide::platform::LocalProcessLauncher());
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

  microide::project::GitRepository repo(root, microide::platform::LocalProcessLauncher());
  Expect(repo.DiscardAll(), "discard-all should succeed");
  Expect(ReadFile(root / "a.txt") == a_committed, "discard-all must restore a.txt exactly");
  Expect(ReadFile(root / "b.txt") == b_committed, "discard-all must restore b.txt exactly");
}

void TestAtomicWriteRefusesDirectoryDestination() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path target = temp_dir.path() / "target";
  std::error_code ec;
  std::filesystem::create_directory(target, ec);
  Expect(!ec, "fixture directory should be created");
  const std::filesystem::path sentinel = target / "keep.txt";
  WriteFile(sentinel, "sentinel\n");

  std::error_code why;
  Expect(!WriteTextFileAtomically(target, "bytes\n", &why),
         "atomic write over a directory destination must fail");
  Expect(why == std::errc::is_a_directory, "and says a folder is in the way: " + why.message());
  Expect(std::filesystem::is_directory(target, ec) && !ec,
         "the directory destination must still be a directory");
  Expect(std::filesystem::exists(sentinel, ec) && !ec,
         "the directory's contents must be untouched");
}

void TestRenameReplacingRefusesDirectoryDestination() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path source = temp_dir.path() / "src.tmp";
  const std::filesystem::path dest_dir = temp_dir.path() / "dst";
  WriteFile(source, "payload\n");
  std::error_code ec;
  std::filesystem::create_directory(dest_dir, ec);
  Expect(!ec, "fixture directory should be created");

  Expect(!microide::util::RenameReplacing(source, dest_dir),
         "RenameReplacing must fail rather than replace a directory");
  Expect(std::filesystem::is_directory(dest_dir, ec) && !ec,
         "the directory destination must survive a refused rename");
}

void TestAtomicWriteSupportsRelativeFilenameInCurrentDirectory() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path prev = std::filesystem::current_path();
  std::error_code ec;
  std::filesystem::current_path(temp_dir.path(), ec);
  Expect(!ec, "should be able to enter the temp dir");
  const bool ok = WriteTextFileAtomically(std::filesystem::path("relative.txt"), "hi\n");
  const std::string readback = ok ? ReadFile(temp_dir.path() / "relative.txt") : std::string();
  std::filesystem::current_path(prev, ec);
  Expect(ok, "atomic write of a bare relative filename should succeed");
  Expect(readback == "hi\n", "relative-path atomic write must land in the current directory");
}

}

void TestDescribeWriteErrorWordsTheCommonCauses() {
  using microide::util::DescribeWriteError;
  Expect(DescribeWriteError({}).empty(), "no error, no words");
  Expect(DescribeWriteError(std::make_error_code(std::errc::no_space_on_device)) == "no space left on the device",
         "ENOSPC");
  Expect(DescribeWriteError(std::make_error_code(std::errc::read_only_file_system)) ==
             "the file system is read-only",
         "EROFS");
  Expect(DescribeWriteError(std::error_code(EDQUOT, std::generic_category())) == "the disk quota is exceeded",
         "EDQUOT");
  const std::string other = DescribeWriteError(std::make_error_code(std::errc::text_file_busy));
  Expect(!other.empty() && std::islower(static_cast<unsigned char>(other[0])) != 0,
         "anything else: the system's own message, lower-cased to sit after a colon: " + other);
}

void TestFsyncDirectoryFlushesARealDirectory() {
  TemporaryDirectory temp_dir;
  Expect(microide::util::FsyncDirectory(temp_dir.path()), "a directory flushes");
  Expect(!microide::util::FsyncDirectory(temp_dir.path() / "missing"), "a missing one says it could not");
  WriteFile(temp_dir.path() / "f.txt", "x\n");
  Expect(!microide::util::FsyncDirectory(temp_dir.path() / "f.txt"), "nor is a file a directory");
}

void RegisterSaveDataIntegrityTests(std::vector<TestCase>& tests) {
  AddTest(tests, "SaveDataIntegrity/FsyncDirectoryFlushesARealDirectory", TestFsyncDirectoryFlushesARealDirectory);
  AddTest(tests, "SaveDataIntegrity/DescribeWriteErrorWordsTheCommonCauses",
          TestDescribeWriteErrorWordsTheCommonCauses);
  AddTest(tests, "SaveDataIntegrity/StagingTempNameIsRecognizedByItsOwnPredicate",
          TestStagingTempNameIsRecognizedByItsOwnPredicate);
  AddTest(tests, "SaveDataIntegrity/AtomicWriteRefusesDirectoryDestination",
          TestAtomicWriteRefusesDirectoryDestination);
  AddTest(tests, "SaveDataIntegrity/RenameReplacingRefusesDirectoryDestination",
          TestRenameReplacingRefusesDirectoryDestination);
  AddTest(tests, "SaveDataIntegrity/AtomicWriteSupportsRelativeFilenameInCurrentDirectory",
          TestAtomicWriteSupportsRelativeFilenameInCurrentDirectory);
  AddTest(tests, "SaveDataIntegrity/AtomicWriteUsesUniqueTemp",
          TestAtomicWriteUsesUniqueTempAndSpareTmpSurvives);
  AddTest(tests, "SaveDataIntegrity/AtomicWritePreservesFileMode",
          TestAtomicWritePreservesFileMode);
  AddTest(tests, "SaveDataIntegrity/AtomicWriteThroughSymlinkPreservesLink",
          TestAtomicWriteThroughSymlinkPreservesLink);
  AddTest(tests, "SaveDataIntegrity/AtomicWriteThroughSymlinkWithMissingTargetPreservesLink",
          TestAtomicWriteThroughSymlinkWithMissingTargetPreservesLink);
  AddTest(tests, "SaveDataIntegrity/SaveWriteFailureKeepsDirtyAndFileIntact",
          TestSaveWriteFailureKeepsDirtyAndFileIntact);
  AddTest(tests, "SaveDataIntegrity/SavePreservesCrlfLineEnding",
          TestSavePreservesCrlfLineEnding);
  AddTest(tests, "SaveDataIntegrity/SavePreservesCrOnlyLineEnding",
          TestSavePreservesCrOnlyLineEnding);
  AddTest(tests, "SaveDataIntegrity/SaveLineEndingOverrideRewritesEndings",
          TestSaveLineEndingOverrideRewritesEndings);
  AddTest(tests, "SaveDataIntegrity/SavePreservesTrailingNewlinePresence",
          TestSavePreservesTrailingNewlinePresence);
  AddTest(tests, "SaveDataIntegrity/CleanSaveStreamsMultiLineBufferByteExact",
          TestCleanSaveStreamsMultiLineBufferByteExact);
  AddTest(tests, "SaveDataIntegrity/SaveBinaryContentRoundTripsExactly",
          TestSaveBinaryContentRoundTripsExactly);
  AddTest(tests, "SaveDataIntegrity/SaveDetectsExternalChangeConflict",
          TestSaveDetectsExternalChangeConflict);
  AddTest(tests, "SaveDataIntegrity/SavePreservesUtf8Bom", TestSavePreservesUtf8Bom);
  AddTest(tests, "SaveDataIntegrity/AtomicSaveReplacesInodeAndBreaksHardlink",
          TestAtomicSaveReplacesInodeAndBreaksHardlink);
  AddTest(tests, "SaveDataIntegrity/ReloadResetsUndoHistory", TestReloadResetsUndoHistory);
  AddTest(tests, "SaveDataIntegrity/GitDiscardRestoresExactCommittedBytes",
          TestGitDiscardRestoresExactCommittedBytes);
  AddTest(tests, "SaveDataIntegrity/GitDiscardAllRestoresEveryFile",
          TestGitDiscardAllRestoresEveryFile);
}

}
