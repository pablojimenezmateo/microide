#include "TestSupport.h"

#include "project/remote/MirrorStore.h"
#include "project/remote/MirrorSyncEngine.h"
#include "project/remote/RemoteServerClient.h"
#include "project/remote/RemoteWorkspace.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace microide::tests {
namespace {

namespace remote = project::remote;
using ContentState = remote::MirrorSyncEngine::ContentState;

#if defined(__unix__) || defined(__APPLE__)

// A host tree served by a real `microide-server serve-stdio`, and a mirror of it in
// a different directory.
struct MirrorSession {
  TemporaryDirectory temp;
  std::filesystem::path host = temp.path() / "host";
  std::filesystem::path mirror_dir = temp.path() / "mirror";
  std::unique_ptr<remote::RemoteServerClient> client;
  std::unique_ptr<remote::RemoteWorkspace> workspace;
  std::unique_ptr<remote::MirrorStore> store;
  std::unique_ptr<remote::MirrorSyncEngine> engine;
  std::vector<std::string> materialized;

  MirrorSession() { std::filesystem::create_directories(host); }

  void Connect(remote::MirrorSyncEngine::Options options = {}) {
    engine.reset();
    client = std::make_unique<remote::RemoteServerClient>();
    std::string error;
    Expect(client->ConnectCommand({MICROIDE_SERVER_BINARY, "serve-stdio"},
                                  remote::HelloRequest{.release = "test", .root = host.string()},
                                  &error),
           "connects: " + error);
    workspace = std::make_unique<remote::RemoteWorkspace>(*client);
    store = std::make_unique<remote::MirrorStore>(mirror_dir);
    Expect(store->Open(&error), "the mirror opens: " + error);
    engine = std::make_unique<remote::MirrorSyncEngine>(
        *workspace, *store, options,
        remote::MirrorSyncEngine::Callbacks{
            .changed = {},
            .materialized =
                [this](const std::vector<std::string>& paths) {
                  materialized.insert(materialized.end(), paths.begin(), paths.end());
                },
        });
  }

  void Sync() {
    engine->RequestSync();
    engine->Flush();
  }

  std::filesystem::path Tree(std::string_view path) const {
    return mirror_dir / "tree" / std::string(path);
  }
};

void TestMirrorInitialSyncAndHostChanges() {
  MirrorSession session;
  WriteFile(session.host / "a.txt", "alpha\n");
  WriteFile(session.host / "src/b.cpp", "int b;\n");
  WriteFile(session.host / "doomed.txt", "bye\n");
  std::filesystem::create_symlink("a.txt", session.host / "link");
  session.Connect();
  session.Sync();
  const auto status = session.engine->status();
  Expect(status.error.empty() && status.synced_once && !status.syncing, "synced: " + status.error);
  Expect(ReadFile(session.Tree("a.txt")) == "alpha\n" && ReadFile(session.Tree("src/b.cpp")) == "int b;\n",
         "the mirror holds the host's bytes");
  Expect(std::filesystem::is_symlink(session.Tree("link")) &&
             std::filesystem::read_symlink(session.Tree("link")) == "a.txt",
         "an in-root link is a link in the mirror");
  Expect(session.engine->StateOf("a.txt") == ContentState::Current, "a pulled file is current");

  // The host moves on: an edit, a delete, a new file.
  WriteFile(session.host / "a.txt", "alpha v2\n");
  std::filesystem::remove(session.host / "doomed.txt");
  WriteFile(session.host / "new/c.txt", "c\n");
  session.Sync();
  Expect(ReadFile(session.Tree("a.txt")) == "alpha v2\n", "an edit on the host is pulled");
  Expect(!std::filesystem::exists(session.Tree("doomed.txt")), "a delete on the host is applied");
  Expect(ReadFile(session.Tree("new/c.txt")) == "c\n", "a new host file arrives");
  Expect(session.engine->StateOf("doomed.txt") == ContentState::Unknown, "and the deleted one is gone");
}

void TestMirrorPushesLocalWritesUnderCompareAndSwap() {
  MirrorSession session;
  WriteFile(session.host / "a.txt", "host\n");
  session.Connect();
  session.Sync();

  WriteFile(session.Tree("a.txt"), "local edit\n");
  session.engine->NotifyLocalWrite("a.txt");
  session.engine->Flush();
  Expect(ReadFile(session.host / "a.txt") == "local edit\n", "a local save reaches the host");
  Expect(session.engine->StateOf("a.txt") == ContentState::Current, "and the file is current");

  WriteFile(session.Tree("fresh.txt"), "made here\n");
  session.engine->NotifyLocalWrite("fresh.txt");
  session.engine->Flush();
  Expect(ReadFile(session.host / "fresh.txt") == "made here\n", "a file created locally is created");

  // The pushed write comes back as the host's row: not a change, not a pull.
  session.materialized.clear();
  session.Sync();
  Expect(session.materialized.empty(), "our own push echoed by the manifest pulls nothing");
  Expect(session.engine->StateOf("a.txt") == ContentState::Current, "still current");
}

void TestMirrorParksConflictsAndNeverOverwritesLocalEdits() {
  MirrorSession session;
  WriteFile(session.host / "a.txt", "base\n");
  session.Connect();
  session.Sync();

  // Both sides move from the base.
  WriteFile(session.host / "a.txt", "agent\n");
  WriteFile(session.Tree("a.txt"), "human\n");
  session.engine->NotifyLocalWrite("a.txt");
  session.engine->Flush();
  Expect(ReadFile(session.host / "a.txt") == "agent\n", "the push is refused: the host keeps its bytes");
  Expect(ReadFile(session.Tree("a.txt")) == "human\n", "the mirror keeps the user's bytes");
  Expect(session.engine->StateOf("a.txt") == ContentState::Conflict, "the path is a conflict");
  session.Sync();
  Expect(ReadFile(session.Tree("a.txt")) == "human\n", "a later sync does not pull over it");
  Expect(session.engine->status().conflicts == 1, "and still reports it");
}

void TestMirrorHoldsAMassDelete() {
  MirrorSession session;
  for (int i = 0; i < 150; ++i) {
    WriteFile(session.host / ("f" + std::to_string(i)), "x");
  }
  session.Connect();
  session.Sync();
  for (int i = 0; i < 140; ++i) {
    std::filesystem::remove(session.host / ("f" + std::to_string(i)));
  }
  session.Sync();
  Expect(session.engine->status().held_deletes == 140, "a delete of most of the mirror is held");
  Expect(std::filesystem::exists(session.Tree("f0")), "and nothing was deleted");
  session.engine->ApproveHeldDeletes();
  session.engine->Flush();
  Expect(!std::filesystem::exists(session.Tree("f0")) && std::filesystem::exists(session.Tree("f149")),
         "approved, it applies");
}

void TestMirrorJournalSurvivesARestart() {
  MirrorSession session;
  WriteFile(session.host / "a.txt", "v1\n");
  session.Connect();
  session.Sync();
  // A save that never reached the host: written and journaled, then the link died.
  WriteFile(session.Tree("a.txt"), "offline edit\n");
  session.store->entries()["a.txt"].push_pending = true;
  std::string error;
  Expect(session.store->Save(&error), "journal saved");
  session.Connect();  // a new connection, a reopened store
  Expect(session.engine->StateOf("a.txt") == ContentState::Dirty, "the reopened mirror knows it");
  session.Sync();
  Expect(ReadFile(session.host / "a.txt") == "offline edit\n", "the next sync replays it");
  Expect(session.engine->StateOf("a.txt") == ContentState::Current, "and it is current");

  // An edit made in tree/ by something other than the editor is ours too.
  WriteFile(session.Tree("a.txt"), "a tool wrote this\n");
  session.Sync();
  Expect(ReadFile(session.host / "a.txt") == "a tool wrote this\n", "an outside local edit is pushed");
}

#endif

}  // namespace

void RegisterMirrorSyncEngineTests(std::vector<TestCase>& tests) {
#if defined(__unix__) || defined(__APPLE__)
  AddTest(tests, "MirrorSyncEngine/InitialSyncAndHostChanges", TestMirrorInitialSyncAndHostChanges);
  AddTest(tests, "MirrorSyncEngine/PushesLocalWritesUnderCompareAndSwap",
          TestMirrorPushesLocalWritesUnderCompareAndSwap);
  AddTest(tests, "MirrorSyncEngine/ParksConflictsAndNeverOverwritesLocalEdits",
          TestMirrorParksConflictsAndNeverOverwritesLocalEdits);
  AddTest(tests, "MirrorSyncEngine/HoldsAMassDelete", TestMirrorHoldsAMassDelete);
  AddTest(tests, "MirrorSyncEngine/JournalSurvivesARestart", TestMirrorJournalSurvivesARestart);
#else
  (void)tests;
#endif
}

}  // namespace microide::tests
