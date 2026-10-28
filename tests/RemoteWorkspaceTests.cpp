#include "TestSupport.h"

#include "project/remote/RemoteConnection.h"
#include "project/remote/RemoteServerClient.h"
#include "project/remote/RemoteWorkspace.h"
#include "util/ContentHash.h"
#include "util/Zstd.h"

#include <algorithm>
#include <filesystem>
#include <future>
#include <optional>
#include <memory>
#include <string>
#include <vector>

namespace microide::tests {
namespace {

namespace remote = project::remote;

// A real `microide-server serve-stdio` with a hello naming `root`.
struct WorkspaceSession {
  std::shared_ptr<remote::RemoteServerClient> client =
      remote::RemoteServerClient::Create();
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
  auto client = remote::RemoteServerClient::Create();
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

// object/fetch with the version the client holds, waited on.
std::vector<remote::RemoteWorkspace::FetchedObject> FetchWithBases(
    remote::RemoteWorkspace& workspace, std::vector<std::string> paths,
    std::vector<std::optional<util::ContentHash>> bases) {
  std::promise<std::vector<remote::RemoteWorkspace::FetchedObject>> answered;
  workspace.FetchObjects(
      std::move(paths), remote::Lane::Bulk,
      [&](std::optional<std::vector<remote::RemoteWorkspace::FetchedObject>> objects, std::string error) {
        Expect(objects.has_value(), "the fetch answers: " + error);
        answered.set_value(objects.value_or(std::vector<remote::RemoteWorkspace::FetchedObject>{}));
      },
      0, std::move(bases));
  auto future = answered.get_future();
  Expect(future.wait_for(std::chrono::seconds(60)) == std::future_status::ready, "the fetch answers in time");
  return future.get();
}

// What the client ends up holding for an answer: the bytes, or the delta applied
// to the base it sent.
std::string Reconstruct(const remote::RemoteWorkspace::FetchedObject& object, const std::string& base) {
  if (!object.delta_base.has_value()) {
    return object.content;
  }
  if (object.content.empty()) {
    return base;  // unchanged
  }
  return util::ZstdApplyDelta(base, object.content, 64u << 20).value_or("<delta did not apply>");
}

std::string Lines(const std::string& tag, int count) {
  std::string text;
  for (int i = 0; i < count; ++i) {
    text += tag + " line " + std::to_string(i) + " with some text to make it a real source file\n";
  }
  return text;
}

// The host keeps delta bases for files that CHANGE, not for the first sync
// (TD-2026-10-28-417): the backfill used to pin every byte it served. A changed
// file's first delta comes from git's index when what the client holds is the
// committed blob, and later ones from the bytes this server last sent.
void TestAChangedFileIsADeltaWithoutKeepingTheFirstSync() {
  TemporaryDirectory temp;
  const std::filesystem::path root = temp.path() / "host";
  InitializeGitRepo(root);
  const std::string big = Lines("big", 1200);      // ~80 KiB: over the index-base minimum
  const std::string small = Lines("small", 10);    // under it
  const std::string dirty = Lines("dirty", 1200);  // differs from its index blob
  WriteFile(root / "big.txt", big);
  WriteFile(root / "small.txt", small);
  WriteFile(root / "dirty.txt", Lines("committed", 1200));
  CommitAll(root, "init", "delta bases");
  WriteFile(root / "dirty.txt", dirty);

  WorkspaceSession session(root);
  std::string error;
  const auto first = session.workspace->FetchObjectsSync({"big.txt", "small.txt", "dirty.txt"},
                                                         remote::Lane::Bulk, &error);
  Expect(first.has_value() && first->size() == 3 && (*first)[0].hash.has_value() &&
             (*first)[1].hash.has_value() && (*first)[2].hash.has_value(),
         "the first sync: " + error);

  const std::string big2 = "an agent's edit\n" + big;
  const std::string small2 = "an agent's edit\n" + small;
  const std::string dirty2 = "an agent's edit\n" + dirty;
  WriteFile(root / "big.txt", big2);
  WriteFile(root / "small.txt", small2);
  WriteFile(root / "dirty.txt", dirty2);
  auto changed = FetchWithBases(*session.workspace, {"big.txt", "small.txt", "dirty.txt"},
                                {(*first)[0].hash, (*first)[1].hash, (*first)[2].hash});
  Expect(changed.size() == 3, "every changed object answers");
  Expect(changed[0].delta_base == (*first)[0].hash && changed[0].content.size() < big2.size() / 8 &&
             Reconstruct(changed[0], big) == big2,
         "a committed file's first change is a delta against its index blob");
  Expect(!changed[1].delta_base.has_value() && changed[1].content == small2,
         "a small file is sent whole: the first sync's bytes were not kept");
  Expect(!changed[2].delta_base.has_value() && changed[2].content == dirty2,
         "a file that was dirty at the first sync has no base on the host: whole");

  const std::string big3 = big2 + "a second edit\n";
  const std::string dirty3 = dirty2 + "a second edit\n";
  WriteFile(root / "big.txt", big3);
  WriteFile(root / "dirty.txt", dirty3);
  const std::string small_hash = changed[1].hash->Hex();
  auto again = FetchWithBases(*session.workspace, {"big.txt", "dirty.txt", "small.txt"},
                              {changed[0].hash, changed[2].hash, changed[1].hash});
  Expect(again.size() == 3 && again[0].delta_base.has_value() && Reconstruct(again[0], big2) == big3,
         "the next change is a delta against what was last sent");
  Expect(again[1].delta_base.has_value() && Reconstruct(again[1], dirty2) == dirty3,
         "including for the file git could not answer for");
  Expect(again[2].delta_base.has_value() && again[2].content.empty() && again[2].hash->Hex() == small_hash,
         "an unchanged file is answered as unchanged, without its bytes");
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

}  // namespace

void RegisterRemoteWorkspaceTests(std::vector<TestCase>& tests) {
  AddTest(tests, "RemoteWorkspace/ManifestStreamsTheHostTree", TestManifestStreamsTheHostTree);
  AddTest(tests, "RemoteWorkspace/ManifestFailureIsAnErrorNotAnEmptyTree",
          TestManifestFailureIsAnErrorNotAnEmptyTree);
  AddTest(tests, "RemoteWorkspace/ManifestNeedsAWorkspace", TestManifestNeedsAWorkspace);
  AddTest(tests, "RemoteWorkspace/ObjectFetchServesVerifiedBytes", TestObjectFetchServesVerifiedBytes);
  AddTest(tests, "RemoteWorkspace/AChangedFileIsADeltaWithoutKeepingTheFirstSync",
          TestAChangedFileIsADeltaWithoutKeepingTheFirstSync);
  AddTest(tests, "RemoteWorkspace/WriteAndTreeOpsAreCompareAndSwap",
          TestWriteAndTreeOpsAreCompareAndSwap);
}

}  // namespace microide::tests
