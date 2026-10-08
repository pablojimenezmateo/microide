#include "TestSupport.h"

#include "project/remote/RemoteConnection.h"
#include "project/remote/RemoteServerClient.h"
#include "project/remote/RemoteWorkspace.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace microide::tests {
namespace {

namespace remote = project::remote;

#if defined(__unix__) || defined(__APPLE__)

// A real `microide-server serve-stdio` with a hello naming `root`.
struct WorkspaceSession {
  std::shared_ptr<remote::RemoteServerClient> client =
      std::make_shared<remote::RemoteServerClient>();
  std::unique_ptr<remote::RemoteWorkspace> workspace;

  explicit WorkspaceSession(const std::filesystem::path& root) {
    std::string error;
    Expect(client->ConnectCommand({MICROIDE_SERVER_BINARY, "serve-stdio"},
                                  remote::HelloRequest{.release = "test", .root = root.string()},
                                  &error),
           "connects: " + error);
    workspace = std::make_unique<remote::RemoteWorkspace>(
        std::make_shared<remote::RemoteConnection>(client));
  }
};

void TestManifestStreamsTheHostTree() {
  TemporaryDirectory temp;
  const std::filesystem::path root = temp.path() / "host";
  InitializeGitRepo(root);
  WriteFile(root / ".gitignore", "build/\n");
  // Enough rows, with long enough paths, to need several bulk chunks.
  for (int i = 0; i < 1500; ++i) {
    WriteFile(root / "src" / ("module_" + std::to_string(i / 100)) /
                  ("a_rather_long_file_name_to_fill_chunks_" + std::to_string(i) + ".cpp"),
              "int v" + std::to_string(i) + ";\n");
  }
  WriteFile(root / "build/ignored.o", "x");
  CommitAll(root, "init", "remote workspace");
  WriteFile(root / "new.txt", "untracked\n");

  WorkspaceSession session(root);
  std::string error;
  const auto manifest = session.workspace->FetchManifestSync(&error);
  Expect(manifest.has_value(), "the manifest arrives: " + error);
  Expect(manifest->rows.size() == 1502 && manifest->git,
         "every tracked and untracked-not-ignored file, from git: " +
             std::to_string(manifest->rows.size()));
  const auto it = std::find_if(manifest->rows.begin(), manifest->rows.end(),
                               [](const remote::ManifestRow& row) { return row.path == "new.txt"; });
  Expect(it != manifest->rows.end() && it->hash == util::HashContent("untracked\n"),
         "rows carry the hash of the host's bytes");

  const auto again = session.workspace->FetchManifestSync(&error);
  Expect(again.has_value() && again->rows == manifest->rows && again->id > manifest->id,
         "a second manifest is equal and newer");
}

void TestManifestFailureIsAnErrorNotAnEmptyTree() {
  TemporaryDirectory temp;
  WorkspaceSession session(temp.path() / "does-not-exist");
  std::string error;
  Expect(!session.workspace->FetchManifestSync(&error).has_value() && !error.empty(),
         "a root the host cannot read fails the request: " + error);
  Expect(session.client->connected(), "and leaves the connection up");
}

void TestManifestNeedsAWorkspace() {
  auto client = std::make_shared<remote::RemoteServerClient>();
  std::string error;
  Expect(client->ConnectCommand({MICROIDE_SERVER_BINARY, "serve-stdio"},
                               remote::HelloRequest{.release = "test", .root = ""}, &error),
         "connects without a root: " + error);
  remote::RemoteWorkspace workspace(std::make_shared<remote::RemoteConnection>(client));
  Expect(!workspace.FetchManifestSync(&error).has_value() &&
             error.find("no workspace") != std::string::npos,
         "a terminal-only connection has no tree: " + error);
}

void TestObjectFetchServesVerifiedBytes() {
  TemporaryDirectory temp;
  const std::filesystem::path root = temp.path() / "host";
  const std::string big(300 * 1024 + 7, 'b');
  WriteFile(root / "small.txt", "small\n");
  WriteFile(root / "dir/big.bin", big);
  WorkspaceSession session(root);
  std::string error;
  const auto objects = session.workspace->FetchObjectsSync(
      {"small.txt", "dir/big.bin", "missing.txt", "../escape"}, remote::Lane::Interactive, &error);
  Expect(objects.has_value() && objects->size() == 4, "the fetch answers every object: " + error);
  Expect((*objects)[0].content == "small\n" && (*objects)[0].hash == util::HashContent("small\n"),
         "a small object arrives with its hash");
  Expect((*objects)[1].content == big && (*objects)[1].hash == util::HashContent(big),
         "a multi-chunk object is reassembled in order");
  Expect((*objects)[2].missing && !(*objects)[2].hash.has_value(), "a missing path says so");
  Expect(!(*objects)[3].error.empty() && (*objects)[3].content.empty(),
         "an unsafe path is an error, not a read");
  const auto bulk = session.workspace->FetchObjectsSync({"dir/big.bin"}, remote::Lane::Bulk, &error);
  Expect(bulk.has_value() && (*bulk)[0].content == big, "the bulk lane delivers the same bytes");
}

void TestWriteAndTreeOpsAreCompareAndSwap() {
  TemporaryDirectory temp;
  const std::filesystem::path root = temp.path() / "host";
  std::filesystem::create_directories(root);
  WorkspaceSession session(root);
  using Status = remote::RemoteWorkspace::WriteResult::Status;
  const std::string content(200 * 1024, 'w');  // several WriteData frames
  const auto created = session.workspace->WriteFileSync("src/new.cpp", content,
                                                        remote::Precondition::NotThere());
  Expect(created.status == Status::Ok && created.hash == util::HashContent(content) &&
             ReadFile(root / "src/new.cpp") == content,
         "a create lands on the host with its hash: " + created.error);
  const auto again = session.workspace->WriteFileSync("src/new.cpp", "other",
                                                      remote::Precondition::NotThere());
  Expect(again.status == Status::Conflict && again.hash == util::HashContent(content),
         "a second create conflicts, naming what is there");
  const auto update = session.workspace->WriteFileSync(
      "src/new.cpp", "v2", remote::Precondition::Of(util::HashContent(content)));
  Expect(update.status == Status::Ok && ReadFile(root / "src/new.cpp") == "v2",
         "an update on its base lands");
  const auto stale = session.workspace->WriteFileSync(
      "src/new.cpp", "v3", remote::Precondition::Of(util::HashContent(content)));
  Expect(stale.status == Status::Conflict && ReadFile(root / "src/new.cpp") == "v2",
         "an update on a stale base does not");
  const auto refused = session.workspace->WriteFileSync("../outside.txt", "x",
                                                        remote::Precondition::NotThere());
  Expect(refused.status == Status::Error && !std::filesystem::exists(temp.path() / "outside.txt"),
         "a write outside the root is refused");

  using TreeOp = remote::RemoteWorkspace::TreeOp;
  Expect(session.workspace
                 ->ApplyTreeOpSync(TreeOp::Rename, "src/new.cpp", "src/moved.cpp",
                                   remote::Precondition::Of(util::HashContent("v2")))
                 .status == Status::Ok &&
             ReadFile(root / "src/moved.cpp") == "v2",
         "a rename on its base moves the file");
  Expect(session.workspace
                 ->ApplyTreeOpSync(TreeOp::MakeDirectory, "empty/dir", "",
                                   remote::Precondition::Anything())
                 .status == Status::Ok &&
             std::filesystem::is_directory(root / "empty/dir"),
         "mkdir creates the directory");
  Expect(session.workspace
                 ->ApplyTreeOpSync(TreeOp::Delete, "src/moved.cpp", "",
                                   remote::Precondition::Of(util::HashContent("v2")))
                 .status == Status::Ok &&
             !std::filesystem::exists(root / "src/moved.cpp"),
         "a delete on its base removes the file");
}

#endif

}  // namespace

void RegisterRemoteWorkspaceTests(std::vector<TestCase>& tests) {
#if defined(__unix__) || defined(__APPLE__)
  AddTest(tests, "RemoteWorkspace/ManifestStreamsTheHostTree", TestManifestStreamsTheHostTree);
  AddTest(tests, "RemoteWorkspace/ManifestFailureIsAnErrorNotAnEmptyTree",
          TestManifestFailureIsAnErrorNotAnEmptyTree);
  AddTest(tests, "RemoteWorkspace/ManifestNeedsAWorkspace", TestManifestNeedsAWorkspace);
  AddTest(tests, "RemoteWorkspace/ObjectFetchServesVerifiedBytes", TestObjectFetchServesVerifiedBytes);
  AddTest(tests, "RemoteWorkspace/WriteAndTreeOpsAreCompareAndSwap",
          TestWriteAndTreeOpsAreCompareAndSwap);
#else
  (void)tests;
#endif
}

}  // namespace microide::tests
