#include "TestSupport.h"

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
#endif

}  // namespace

void RegisterFileOperationServiceTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Project/FileOperationService", TestFileOperationService);
#if defined(__linux__)
  AddTest(tests, "Project/TrashReservationDoesNotOverwriteExistingMetadata",
          TestTrashReservationDoesNotOverwriteExistingMetadata);
#endif
}

}  // namespace microide::tests
