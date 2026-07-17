#include "TestSupport.h"

#include "platform/FsOps.h"
#include "project/FileOperationService.h"

#include <filesystem>

namespace microide::tests {
namespace {

using microide::project::FileOperationService;

void TestFileOperationService() {
  TemporaryDirectory temp_dir;
  const auto root = temp_dir.path() / "workspace";
  std::filesystem::create_directories(root);

  const auto created_directory = FileOperationService::CreateDirectory(root / "nested" / "folder");
  Expect(created_directory.ok, "create directory should succeed");
  Expect(std::filesystem::is_directory(root / "nested" / "folder"),
         "created directory should exist");

  const auto created_file = FileOperationService::CreateFile(root / "nested" / "folder" / "draft.txt");
  Expect(created_file.ok, "create file should succeed");
  Expect(std::filesystem::is_regular_file(root / "nested" / "folder" / "draft.txt"),
         "created file should exist");

  const auto renamed_file =
      FileOperationService::RenamePath(root / "nested" / "folder" / "draft.txt",
                                       root / "nested" / "folder" / "final.txt");
  Expect(renamed_file.ok, "rename file should succeed");
  Expect(!std::filesystem::exists(root / "nested" / "folder" / "draft.txt"),
         "source file should disappear after rename");
  Expect(std::filesystem::is_regular_file(root / "nested" / "folder" / "final.txt"),
         "destination file should exist after rename");

  const auto renamed_directory =
      FileOperationService::RenamePath(root / "nested" / "folder", root / "nested" / "renamed");
  Expect(renamed_directory.ok, "rename directory should succeed");
  Expect(!std::filesystem::exists(root / "nested" / "folder"),
         "source directory should disappear after rename");
  Expect(std::filesystem::is_directory(root / "nested" / "renamed"),
         "destination directory should exist after rename");
  Expect(std::filesystem::is_regular_file(root / "nested" / "renamed" / "final.txt"),
         "renamed directory should keep its contents");

#if defined(__linux__)
  const auto trash_home = temp_dir.path() / "xdg-data-home";
  ScopedEnvVar scoped_xdg_data_home("XDG_DATA_HOME", trash_home.string());
  const auto trash_target = root / "trash-me.txt";
  WriteFile(trash_target, "trash me");
  const auto trashed = FileOperationService::TrashPath(trash_target);
  Expect(trashed.ok, "trash should succeed on linux");
  Expect(!std::filesystem::exists(trash_target), "source file should disappear after trash");
  Expect(std::filesystem::is_regular_file(trashed.resulting_path),
         "trashed file should be moved into the trash");
  const auto info_path =
      trash_home / "Trash" / "info" / (trashed.resulting_path.filename().string() + ".trashinfo");
  Expect(std::filesystem::is_regular_file(info_path), "trash metadata file should exist");
#endif
}

void TestCreateDirectoryClassifiesExistingTargets() {
  TemporaryDirectory temp_dir;
  const auto root = temp_dir.path() / "workspace";
  std::filesystem::create_directories(root);

  // Re-creating an existing directory reports "already exists", not a hard error.
  const auto again = FileOperationService::CreateDirectory(root);
  Expect(!again.ok, "creating an existing directory should fail");
  Expect(again.error_message == "The directory already exists",
         "existing directory is classified distinctly");

  // A regular file occupying the target path is reported as a non-directory
  // clash (the create-first path classifies via status, no TOCTOU probe).
  const auto file_target = root / "occupied";
  WriteFile(file_target, "not a directory");
  const auto clash = FileOperationService::CreateDirectory(file_target);
  Expect(!clash.ok, "creating a directory over a file should fail");
  Expect(clash.error_message == "A non-directory already exists at that path",
         "file clash is classified distinctly from a real create failure");
  Expect(std::filesystem::is_regular_file(file_target),
         "the pre-existing file must be left untouched");
}

#if defined(__linux__)
// Regression: the .trashinfo file is now reserved atomically with O_EXCL, so a
// trash whose base name's metadata already exists must NOT overwrite it — it must
// skip to the next suffix. The old truncating ofstream silently clobbered the
// existing metadata (and, on a real concurrent race, the trashed file itself).
void TestTrashReservationDoesNotOverwriteExistingMetadata() {
  TemporaryDirectory temp_dir;
  const auto root = temp_dir.path() / "workspace";
  std::filesystem::create_directories(root);

  const auto trash_home = temp_dir.path() / "xdg-data-home";
  ScopedEnvVar scoped_xdg_data_home("XDG_DATA_HOME", trash_home.string());

  // Pre-occupy the base name's metadata slot with a sentinel the trash must not
  // touch (simulating a prior/concurrent trash of a same-named file).
  const auto info_dir = trash_home / "Trash" / "info";
  std::filesystem::create_directories(info_dir);
  const auto sentinel_info = info_dir / "trash-me.txt.trashinfo";
  WriteFile(sentinel_info, "SENTINEL");

  const auto trash_target = root / "trash-me.txt";
  WriteFile(trash_target, "trash me");
  const auto trashed = FileOperationService::TrashPath(trash_target);

  Expect(trashed.ok, "trash should still succeed by choosing a free slot");
  Expect(!std::filesystem::exists(trash_target), "source file should disappear after trash");
  Expect(trashed.resulting_path.filename().string() == "trash-me 2.txt",
         "reserved slot should suffix past the occupied base name");
  Expect(ReadFile(sentinel_info) == "SENTINEL",
         "the pre-existing .trashinfo must not be overwritten (O_EXCL reservation)");
  const auto new_info = info_dir / "trash-me 2.txt.trashinfo";
  Expect(std::filesystem::is_regular_file(new_info),
         "the newly reserved .trashinfo should exist");
  const std::string new_contents = ReadFile(new_info);
  Expect(new_contents.find("[Trash Info]") != std::string::npos &&
             new_contents.find("trash-me.txt") != std::string::npos,
         "the new metadata should record the trashed source path");
}

// TD-2026-07-16-49: when a file already occupies Trash/files/<name> (a slot the
// .trashinfo reservation did not gate), the final content move must NOT overwrite it —
// the trash retries the next suffix instead of clobbering the existing trashed file.
void TestTrashFinalMoveRefusesExistingFilesDestination() {
  TemporaryDirectory temp_dir;
  const auto root = temp_dir.path() / "workspace";
  std::filesystem::create_directories(root);

  const auto trash_home = temp_dir.path() / "xdg-data-home";
  ScopedEnvVar scoped_xdg_data_home("XDG_DATA_HOME", trash_home.string());

  // Pre-occupy the base name's FILES slot (not the metadata slot) with a sentinel.
  const auto files_dir = trash_home / "Trash" / "files";
  std::filesystem::create_directories(files_dir);
  const auto sentinel_file = files_dir / "collide.txt";
  WriteFile(sentinel_file, "PRE-EXISTING TRASHED FILE");

  const auto trash_target = root / "collide.txt";
  WriteFile(trash_target, "new content");
  const auto trashed = FileOperationService::TrashPath(trash_target);

  Expect(trashed.ok, "trash should succeed by choosing a free files slot");
  Expect(!std::filesystem::exists(trash_target), "source should disappear after trash");
  Expect(ReadFile(sentinel_file) == "PRE-EXISTING TRASHED FILE",
         "the pre-existing trashed file must NOT be overwritten by the final move");
  Expect(trashed.resulting_path.filename().string() == "collide 2.txt",
         "the final move must land on the next free suffix, not clobber the base slot");
  Expect(ReadFile(trashed.resulting_path) == "new content",
         "the newly trashed content lands at the retried slot");
}
#endif

}  // namespace

// Regression: renaming onto an existing destination must fail WITHOUT clobbering
// it — the move is no-overwrite (renameat2 RENAME_NOREPLACE) so it cannot destroy
// unrelated content that occupies the target.
void TestRenamePathRefusesToOverwriteExistingDestination() {
  TemporaryDirectory temp_dir;
  const auto root = temp_dir.path() / "workspace";
  std::filesystem::create_directories(root);
  const auto source = root / "src.txt";
  const auto dest = root / "dst.txt";
  WriteFile(source, "SOURCE");
  WriteFile(dest, "DESTINATION");

  const auto result = FileOperationService::RenamePath(source, dest);
  Expect(!result.ok, "rename onto an existing destination must fail");
  Expect(result.error_message == "The destination path already exists",
         "the failure is classified as an existing-destination clash");
  Expect(ReadFile(dest) == "DESTINATION", "the existing destination must NOT be overwritten");
  Expect(ReadFile(source) == "SOURCE", "the source must be left intact when the move is refused");
}

// The platform primitive itself must refuse an existing destination (this is the
// race-free guarantee RenamePath relies on, independent of any exists() pre-check).
void TestMovePathNoOverwriteRefusesExistingDestination() {
  TemporaryDirectory temp_dir;
  const auto root = temp_dir.path() / "workspace";
  std::filesystem::create_directories(root);
  const auto source = root / "a.txt";
  const auto dest = root / "b.txt";
  WriteFile(source, "A");
  WriteFile(dest, "B");

  Expect(!microide::platform::MovePathNoOverwrite(source, dest),
         "MovePathNoOverwrite must refuse to clobber an existing destination");
  Expect(ReadFile(dest) == "B", "the destination content must be preserved");
  Expect(ReadFile(source) == "A", "the source must remain when the move is refused");

  // A fresh destination still moves successfully.
  const auto fresh = root / "c.txt";
  Expect(microide::platform::MovePathNoOverwrite(source, fresh),
         "MovePathNoOverwrite must succeed onto a non-existent destination");
  Expect(!std::filesystem::exists(source), "source is gone after a successful move");
  Expect(ReadFile(fresh) == "A", "the content is at the new path");
}

// TD-2026-07-17A-125: a dangling symlink is a real directory entry the file manager
// should be able to rename. The source pre-check used std::filesystem::exists, which
// follows the link and reports a broken target as absent, so the rename was refused.
#if defined(__unix__) || defined(__APPLE__)
void TestRenamePathAcceptsDanglingSymlinkSource() {
  TemporaryDirectory temp_dir;
  const auto root = temp_dir.path() / "workspace";
  std::filesystem::create_directories(root);
  const auto link = root / "broken_link";
  std::error_code ec;
  std::filesystem::create_symlink(root / "no_such_target", link, ec);
  Expect(!ec, "dangling symlink fixture created");
  Expect(!std::filesystem::exists(link), "the symlink target is absent (dangling)");

  const auto dest = root / "renamed_link";
  const auto result = FileOperationService::RenamePath(link, dest);
  Expect(result.ok, "a dangling symlink source must be renameable, not rejected as absent");
  Expect(std::filesystem::is_symlink(std::filesystem::symlink_status(dest)),
         "the renamed node is still a symlink at the new path");
  Expect(!std::filesystem::is_symlink(std::filesystem::symlink_status(link)),
         "the original link node is gone after the rename");
}
#endif

void RegisterFileOperationServiceTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Project/FileOperationService", TestFileOperationService);
  AddTest(tests, "Project/RenamePathRefusesToOverwriteExistingDestination",
          TestRenamePathRefusesToOverwriteExistingDestination);
  AddTest(tests, "Project/MovePathNoOverwriteRefusesExistingDestination",
          TestMovePathNoOverwriteRefusesExistingDestination);
  AddTest(tests, "Project/CreateDirectoryClassifiesExistingTargets",
          TestCreateDirectoryClassifiesExistingTargets);
#if defined(__unix__) || defined(__APPLE__)
  AddTest(tests, "Project/RenamePathAcceptsDanglingSymlinkSource",
          TestRenamePathAcceptsDanglingSymlinkSource);
#endif
#if defined(__linux__)
  AddTest(tests, "Project/TrashReservationDoesNotOverwriteExistingMetadata",
          TestTrashReservationDoesNotOverwriteExistingMetadata);
  AddTest(tests, "Project/TrashFinalMoveRefusesExistingFilesDestination",
          TestTrashFinalMoveRefusesExistingFilesDestination);
#endif
}

}  // namespace microide::tests
