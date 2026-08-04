// Drag-and-drop file open.
//
// SDL_EVENT_DROP_FILE was simply never handled — the shell consumed 23 SDL event
// types covering keyboard, mouse, text input and window, and dropping a file onto
// the window did nothing at all. This covers the decision (what a drop means) and
// the dispatch (what it turns into), both without an SDL event or a window.

#include "TestSupport.h"

#include "workspace/WorkspaceFileDrop.h"

#include <sys/types.h>
#include <sys/stat.h>

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::ApplyFileDrop;
using microide::workspace::FileDropAction;
using microide::workspace::FileDropOperations;
using microide::workspace::FileDropRequest;
using microide::workspace::ResolveFileDrop;

void TestDroppedDirectoryOpensAsProject() {
  TemporaryDirectory temp;
  const std::filesystem::path dir = temp.path() / "some-repo";
  std::filesystem::create_directories(dir);

  const FileDropRequest request = ResolveFileDrop(dir, /*has_project_root=*/false);
  Expect(request.action == FileDropAction::OpenProject,
         "dropping a directory should open it as the project");
  Expect(request.project_root == dir.lexically_normal(),
         "the dropped directory should become the project root");

  // Same answer with a project already open: dropping a folder switches projects,
  // it does not try to open the folder as a file.
  const FileDropRequest with_project = ResolveFileDrop(dir, /*has_project_root=*/true);
  Expect(with_project.action == FileDropAction::OpenProject,
         "dropping a directory should open it as the project even when one is open");
}

void TestDroppedFileWithProjectOpenOpensATab() {
  TemporaryDirectory temp;
  const std::filesystem::path file = temp.path() / "project" / "main.cpp";
  WriteFile(file, "int main() {}\n");

  const FileDropRequest request = ResolveFileDrop(file, /*has_project_root=*/true);
  Expect(request.action == FileDropAction::OpenFile,
         "dropping a file with a project open should open an editor tab");
  Expect(request.file_path == file.lexically_normal(), "the dropped file should be the tab path");
  Expect(request.project_root.empty(),
         "opening a tab should not re-point the project root");
}

// VSCode opens a dropped file in the current window regardless of whether it lives
// under the open folder, and OpenPath imposes no containment rule, so neither does
// this. A drop from outside the project must not silently switch projects.
void TestDroppedFileOutsideTheProjectStillOpensATab() {
  TemporaryDirectory temp;
  const std::filesystem::path outside = temp.path() / "elsewhere" / "notes.txt";
  WriteFile(outside, "notes\n");

  const FileDropRequest request = ResolveFileDrop(outside, /*has_project_root=*/true);
  Expect(request.action == FileDropAction::OpenFile,
         "a file outside the project root should still open as a tab");
}

// Without this the drop lands on the welcome screen, the Open action rejects with
// "No active project", and nothing happens — which is exactly the dead end the
// feature exists to remove.
void TestDroppedFileWithNoProjectOpensItsParentFirst() {
  TemporaryDirectory temp;
  const std::filesystem::path dir = temp.path() / "loose";
  const std::filesystem::path file = dir / "script.py";
  WriteFile(file, "print('hi')\n");

  const FileDropRequest request = ResolveFileDrop(file, /*has_project_root=*/false);
  Expect(request.action == FileDropAction::OpenProjectThenFile,
         "dropping a file with no project open should open its parent as the project");
  Expect(request.project_root == dir.lexically_normal(),
         "the file's parent directory should become the project root");
  Expect(request.file_path == file.lexically_normal(),
         "the dropped file should still be opened as a tab");
}

void TestDroppedPathThatDoesNotExistIsIgnored() {
  TemporaryDirectory temp;
  const FileDropRequest missing = ResolveFileDrop(temp.path() / "nope.txt", true);
  Expect(missing.action == FileDropAction::None, "a nonexistent path should be ignored");

  const FileDropRequest empty = ResolveFileDrop(std::filesystem::path{}, true);
  Expect(empty.action == FileDropAction::None, "an empty path should be ignored");
}

// A dangling symlink exists as a directory entry but resolves to nothing. Neither
// is_directory nor is_regular_file holds, and the non-throwing overloads must keep
// that from escaping the event loop as an exception.
void TestDroppedDanglingSymlinkIsIgnored() {
  TemporaryDirectory temp;
  const std::filesystem::path link = temp.path() / "dangling";
  std::error_code ec;
  std::filesystem::create_symlink(temp.path() / "does-not-exist", link, ec);
  if (ec) {
    return;  // filesystem without symlink support; nothing to assert
  }

  const FileDropRequest request = ResolveFileDrop(link, true);
  Expect(request.action == FileDropAction::None, "a dangling symlink should be ignored");
}

// A fifo is a real directory entry that exists, but handing it to the editor makes
// the open block forever. is_regular_file is what keeps that out.
void TestDroppedNonRegularFileIsIgnored() {
  TemporaryDirectory temp;
  const std::filesystem::path fifo = temp.path() / "pipe";
  if (mkfifo(fifo.c_str(), 0600) != 0) {
    return;  // no fifo support here; nothing to assert
  }
  Expect(std::filesystem::exists(fifo), "the fifo fixture should exist, or this asserts nothing");

  const FileDropRequest request = ResolveFileDrop(fifo, true);
  Expect(request.action == FileDropAction::None, "a fifo should not be opened as a file");
}

void TestApplyFileDropDispatchesInOrder() {
  std::vector<std::string> calls;
  const FileDropOperations operations{
      .open_project =
          [&calls](const std::filesystem::path& root) {
            calls.push_back("project:" + root.filename().string());
            return true;
          },
      .open_file =
          [&calls](const std::filesystem::path& path) {
            calls.push_back("file:" + path.filename().string());
          },
  };

  Expect(ApplyFileDrop(FileDropRequest{.action = FileDropAction::OpenProjectThenFile,
                                       .project_root = "/tmp/root",
                                       .file_path = "/tmp/root/a.txt"},
                       operations),
         "a project-then-file drop should report handled");
  Expect(calls.size() == 2 && calls[0] == "project:root" && calls[1] == "file:a.txt",
         "the project must be opened before the file, or the tab lands in the old project");
}

// If the project fails to open, opening the file anyway would put a tab in whatever
// project was open before — worse than ignoring the drop.
void TestApplyFileDropStopsWhenTheProjectFailsToOpen() {
  bool opened_file = false;
  const FileDropOperations operations{
      .open_project = [](const std::filesystem::path&) { return false; },
      .open_file = [&opened_file](const std::filesystem::path&) { opened_file = true; },
  };

  Expect(!ApplyFileDrop(FileDropRequest{.action = FileDropAction::OpenProjectThenFile,
                                        .project_root = "/tmp/root",
                                        .file_path = "/tmp/root/a.txt"},
                        operations),
         "a failed project open should report unhandled");
  Expect(!opened_file, "no file should be opened after the project open failed");
}

void TestApplyFileDropIgnoresAnEmptyRequest() {
  bool touched = false;
  const FileDropOperations operations{
      .open_project = [&touched](const std::filesystem::path&) { touched = true; return true; },
      .open_file = [&touched](const std::filesystem::path&) { touched = true; },
  };
  Expect(!ApplyFileDrop(FileDropRequest{}, operations), "a None request should report unhandled");
  Expect(!touched, "a None request should call nothing");
}

}  // namespace

void RegisterWorkspaceFileDropTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceFileDrop/DirectoryOpensAsProject", TestDroppedDirectoryOpensAsProject);
  AddTest(tests, "WorkspaceFileDrop/FileWithProjectOpensATab",
          TestDroppedFileWithProjectOpenOpensATab);
  AddTest(tests, "WorkspaceFileDrop/FileOutsideProjectStillOpensATab",
          TestDroppedFileOutsideTheProjectStillOpensATab);
  AddTest(tests, "WorkspaceFileDrop/FileWithNoProjectOpensItsParentFirst",
          TestDroppedFileWithNoProjectOpensItsParentFirst);
  AddTest(tests, "WorkspaceFileDrop/NonexistentPathIsIgnored",
          TestDroppedPathThatDoesNotExistIsIgnored);
  AddTest(tests, "WorkspaceFileDrop/DanglingSymlinkIsIgnored", TestDroppedDanglingSymlinkIsIgnored);
  AddTest(tests, "WorkspaceFileDrop/NonRegularFileIsIgnored", TestDroppedNonRegularFileIsIgnored);
  AddTest(tests, "WorkspaceFileDrop/ApplyDispatchesInOrder", TestApplyFileDropDispatchesInOrder);
  AddTest(tests, "WorkspaceFileDrop/ApplyStopsWhenProjectFails",
          TestApplyFileDropStopsWhenTheProjectFailsToOpen);
  AddTest(tests, "WorkspaceFileDrop/ApplyIgnoresEmptyRequest", TestApplyFileDropIgnoresAnEmptyRequest);
}

}  // namespace microide::tests
