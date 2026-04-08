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

}  // namespace

void RegisterFileOperationServiceTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Project/FileOperationService", TestFileOperationService);
}

}  // namespace microide::tests
