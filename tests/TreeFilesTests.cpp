#include "TestSupport.h"

#include "project/remote/TreeFiles.h"

#include <filesystem>
#include <string>
#include <vector>

namespace microide::tests {
namespace {

using project::remote::Precondition;
using project::remote::FileOpResult;
using util::HashContent;

#if defined(__unix__) || defined(__APPLE__)

void TestTreeFilesWriteIsCompareAndSwap() {
  TemporaryDirectory temp;
  const auto root = temp.path();
  // A create: Absent holds, parents appear, the new hash comes back.
  FileOpResult created =
      project::remote::WriteTreeFile(root, "src/new.txt", "one", Precondition::NotThere(), std::nullopt);
  Expect(created.ok() && created.current == HashContent("one"), "a create lands: " + created.error);
  Expect(ReadFile(root / "src/new.txt") == "one", "with the bytes");
  Expect((std::filesystem::status(root / "src/new.txt").permissions() & std::filesystem::perms::owner_write) !=
             std::filesystem::perms::none,
         "a new file is writable");

  // A second create of the same path is a conflict naming what is there.
  const FileOpResult again =
      project::remote::WriteTreeFile(root, "src/new.txt", "two", Precondition::NotThere(), std::nullopt);
  Expect(again.status == FileOpResult::Status::Conflict && again.current == HashContent("one"),
         "create over an existing file conflicts");
  Expect(ReadFile(root / "src/new.txt") == "one", "and writes nothing");

  // An update against the right base lands; against a stale one it conflicts.
  const FileOpResult update = project::remote::WriteTreeFile(
      root, "src/new.txt", "two", Precondition::Of(HashContent("one")), std::nullopt);
  Expect(update.ok() && ReadFile(root / "src/new.txt") == "two", "an update on its base lands");
  const FileOpResult stale = project::remote::WriteTreeFile(
      root, "src/new.txt", "three", Precondition::Of(HashContent("one")), std::nullopt);
  Expect(stale.status == FileOpResult::Status::Conflict && stale.current == HashContent("two") &&
             ReadFile(root / "src/new.txt") == "two",
         "an update on a stale base conflicts and writes nothing");
  const FileOpResult forced =
      project::remote::WriteTreeFile(root, "src/new.txt", "three", Precondition::Anything(), std::nullopt);
  Expect(forced.ok() && ReadFile(root / "src/new.txt") == "three", "Any overwrites");

  // Modes: an explicit one applies, an omitted one keeps the existing file's.
  Expect(project::remote::WriteTreeFile(root, "run.sh", "#!/bin/sh\n", Precondition::NotThere(), 0755).ok(),
         "an executable is created");
  Expect(project::remote::WriteTreeFile(root, "run.sh", "#!/bin/sh\necho\n",
                                    Precondition::Of(HashContent("#!/bin/sh\n")), std::nullopt)
             .ok(),
         "and updated");
  Expect((std::filesystem::status(root / "run.sh").permissions() & std::filesystem::perms::owner_exec) !=
             std::filesystem::perms::none,
         "an update keeps the exec bit");
}

void TestTreeFilesStayUnderTheRoot() {
  TemporaryDirectory temp;
  const auto root = temp.path() / "root";
  std::filesystem::create_directories(root);
  std::filesystem::create_directories(temp.path() / "outside");
  std::filesystem::create_directory_symlink("../outside", root / "escape");
  const FileOpResult through_link = project::remote::WriteTreeFile(
      root, "escape/planted.txt", "x", Precondition::NotThere(), std::nullopt);
  Expect(through_link.status == FileOpResult::Status::Error &&
             !std::filesystem::exists(temp.path() / "outside/planted.txt"),
         "a parent that is a link is not followed");
  for (const char* path : {"../x", "/etc/x", "a/../../x"}) {
    Expect(project::remote::WriteTreeFile(root, path, "x", Precondition::NotThere(), std::nullopt).status ==
               FileOpResult::Status::Error,
           std::string("an unsafe path is refused: ") + path);
  }
  WriteFile(temp.path() / "outside/target.txt", "secret");
  std::filesystem::create_symlink("../outside/target.txt", root / "link.txt");
  Expect(project::remote::WriteTreeFile(root, "link.txt", "x", Precondition::Anything(), std::nullopt).status ==
             FileOpResult::Status::Error,
         "a link at the path is refused rather than replaced");
  Expect(ReadFile(temp.path() / "outside/target.txt") == "secret", "its target is untouched");
}

void TestTreeFilesRead() {
  TemporaryDirectory temp;
  const auto root = temp.path();
  const std::string content(200 * 1024 + 5, 'q');
  WriteFile(root / "big.bin", content);
  std::string received;
  const FileOpResult read = project::remote::ReadTreeFile(
      root, "big.bin", 1 << 20, [&](std::string_view chunk) { received.append(chunk); });
  Expect(read.ok() && read.current == HashContent(content) && received == content,
         "a read streams the bytes and hashes exactly them");
  Expect(project::remote::ReadTreeFile(root, "big.bin", 1024, {}).status == FileOpResult::Status::Error,
         "a file over the limit is refused");
  Expect(project::remote::ReadTreeFile(root, "missing", 1024, {}).status ==
             FileOpResult::Status::Conflict,
         "a missing file reads as a conflict with nothing there");
}

void TestTreeFilesTreeOps() {
  TemporaryDirectory temp;
  const auto root = temp.path();
  WriteFile(root / "a.txt", "a");
  WriteFile(root / "b.txt", "b");
  Expect(project::remote::MakeTreeDirectory(root, "dir/sub").ok() &&
             std::filesystem::is_directory(root / "dir/sub"),
         "mkdir creates parents");
  Expect(project::remote::MakeTreeDirectory(root, "dir/sub").ok(), "mkdir of an existing dir is fine");
  const FileOpResult onto =
      project::remote::RenameTreeEntry(root, "a.txt", "b.txt", Precondition::Of(HashContent("a")));
  Expect(onto.status == FileOpResult::Status::Conflict && onto.current == HashContent("b") &&
             ReadFile(root / "b.txt") == "b",
         "a rename never clobbers its destination");
  Expect(project::remote::RenameTreeEntry(root, "a.txt", "moved/a.txt", Precondition::Of(HashContent("x")))
                 .status == FileOpResult::Status::Conflict,
         "a rename on a stale source conflicts");
  Expect(project::remote::RenameTreeEntry(root, "a.txt", "moved/a.txt", Precondition::Of(HashContent("a")))
                 .ok() &&
             ReadFile(root / "moved/a.txt") == "a" && !std::filesystem::exists(root / "a.txt"),
         "a rename on its base moves the file, creating the destination's parents");
  Expect(project::remote::DeleteTreeEntry(root, "b.txt", Precondition::Of(HashContent("x"))).status ==
                 FileOpResult::Status::Conflict &&
             std::filesystem::exists(root / "b.txt"),
         "a delete on a stale base conflicts");
  Expect(project::remote::DeleteTreeEntry(root, "b.txt", Precondition::Of(HashContent("b"))).ok() &&
             !std::filesystem::exists(root / "b.txt"),
         "a delete on its base removes the file");
  Expect(project::remote::DeleteTreeEntry(root, "dir/sub", Precondition::Anything()).ok() &&
             !std::filesystem::exists(root / "dir/sub"),
         "an empty directory is deleted under Any");
}

#endif

}  // namespace

void RegisterTreeFilesTests(std::vector<TestCase>& tests) {
#if defined(__unix__) || defined(__APPLE__)
  AddTest(tests, "TreeFiles/WriteIsCompareAndSwap", TestTreeFilesWriteIsCompareAndSwap);
  AddTest(tests, "TreeFiles/StayUnderTheRoot", TestTreeFilesStayUnderTheRoot);
  AddTest(tests, "TreeFiles/Read", TestTreeFilesRead);
  AddTest(tests, "TreeFiles/TreeOps", TestTreeFilesTreeOps);
#else
  (void)tests;
#endif
}

}  // namespace microide::tests
