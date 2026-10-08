#include "TestSupport.h"

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
  std::unique_ptr<remote::RemoteServerClient> client =
      std::make_unique<remote::RemoteServerClient>();
  std::unique_ptr<remote::RemoteWorkspace> workspace;

  explicit WorkspaceSession(const std::filesystem::path& root) {
    std::string error;
    Expect(client->ConnectCommand({MICROIDE_SERVER_BINARY, "serve-stdio"},
                                  remote::HelloRequest{.release = "test", .root = root.string()},
                                  &error),
           "connects: " + error);
    workspace = std::make_unique<remote::RemoteWorkspace>(*client);
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
  remote::RemoteServerClient client;
  std::string error;
  Expect(client.ConnectCommand({MICROIDE_SERVER_BINARY, "serve-stdio"},
                               remote::HelloRequest{.release = "test", .root = ""}, &error),
         "connects without a root: " + error);
  remote::RemoteWorkspace workspace(client);
  Expect(!workspace.FetchManifestSync(&error).has_value() &&
             error.find("no workspace") != std::string::npos,
         "a terminal-only connection has no tree: " + error);
}

#endif

}  // namespace

void RegisterRemoteWorkspaceTests(std::vector<TestCase>& tests) {
#if defined(__unix__) || defined(__APPLE__)
  AddTest(tests, "RemoteWorkspace/ManifestStreamsTheHostTree", TestManifestStreamsTheHostTree);
  AddTest(tests, "RemoteWorkspace/ManifestFailureIsAnErrorNotAnEmptyTree",
          TestManifestFailureIsAnErrorNotAnEmptyTree);
  AddTest(tests, "RemoteWorkspace/ManifestNeedsAWorkspace", TestManifestNeedsAWorkspace);
#else
  (void)tests;
#endif
}

}  // namespace microide::tests
