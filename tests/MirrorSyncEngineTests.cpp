#include "TestSupport.h"

#include "project/remote/MirrorObjects.h"
#include "project/remote/MirrorStore.h"
#include "project/remote/MirrorSyncEngine.h"
#include "project/remote/MirrorWriteGate.h"
#include "project/remote/RemoteConnection.h"
#include "project/remote/RemoteServerClient.h"
#include "project/remote/RemoteWorkspace.h"
#include "util/ContentHash.h"
#include "util/Parse.h"
#include "util/PerformanceCounters.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <functional>
#include <thread>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <unistd.h>

namespace microide::tests {
namespace {

namespace remote = project::remote;
using ContentState = remote::MirrorSyncEngine::ContentState;

// A host tree served by a real `microide-server serve-stdio`, and a mirror of it in
// a different directory.
struct MirrorSession {
  TemporaryDirectory temp;
  std::filesystem::path host = temp.path() / "host";
  std::filesystem::path mirror_dir = temp.path() / "mirror";
  std::shared_ptr<remote::RemoteServerClient> client;
  std::shared_ptr<remote::RemoteConnection> connection;
  std::unique_ptr<remote::RemoteWorkspace> workspace;
  std::unique_ptr<remote::MirrorStore> store;
  std::unique_ptr<remote::MirrorSyncEngine> engine;
  std::vector<std::string> materialized;

  MirrorSession() { std::filesystem::create_directories(host); }

  // A new server process for the host tree: its manifest ids start again at 1.
  // With `pid_file`, the server's pid is written there (to stop it from a test).
  std::shared_ptr<remote::RemoteServerClient> StartServer(const std::filesystem::path& pid_file = {}) {
    auto started = remote::RemoteServerClient::Create();
    std::string error;
    const std::vector<std::string> argv =
        pid_file.empty() ? std::vector<std::string>{MICROIDE_SERVER_BINARY, "serve-stdio"}
                         : std::vector<std::string>{"sh", "-c",
                                                    "echo $$ > '" + pid_file.string() + "'; exec '" +
                                                        std::string(MICROIDE_SERVER_BINARY) + "' serve-stdio"};
    Expect(started->ConnectCommand(argv,
                                   remote::HelloRequest{.release = "test", .root = host.string()},
                                   &error),
           "connects: " + error);
    return started;
  }

  void Connect(remote::MirrorSyncEngine::Options options = {}) {
    engine.reset();
    client = StartServer();
    std::string error;
    connection = std::make_shared<remote::RemoteConnection>(client);
    workspace = std::make_unique<remote::RemoteWorkspace>(connection);
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

// A pull is not flushed per file: the tree is flushed once before the state that
// records it is saved (Durability::Deferred). Two fsyncs per pulled file -- the
// tree's and the object store's -- were two thirds of a 4,521-file first sync on
// a real host (TD-2026-10-14-340).
void TestMirrorFlushesAPullOncePerSaveNotPerFile() {
  MirrorSession session;
  for (int i = 0; i < 300; ++i) {
    WriteFile(session.host / ("f" + std::to_string(i) + ".txt"), "content " + std::to_string(i) + "\n");
  }
  session.Connect();
  util::ResetPerformanceCounters();
  session.Sync();
  Expect(session.engine->status().error.empty(), "synced: " + session.engine->status().error);
  Expect(ReadFile(session.Tree("f299.txt")) == "content 299\n", "the files are there");
  Expect(util::ReadPerformanceCounter(util::PerfCounterId::RemoteTreeFileFsyncs) == 0,
         "no pulled file was flushed on its own: " +
             std::to_string(util::ReadPerformanceCounter(util::PerfCounterId::RemoteTreeFileFsyncs)));
  const std::uint64_t flushes = util::ReadPerformanceCounter(util::PerfCounterId::RemoteTreeFlushes);
  Expect(flushes >= 1 && flushes <= 4,
         "the tree is flushed once per state save: " + std::to_string(flushes));
}

// § 7.9: what the host changed after the first sync is recorded, newest first, one
// row per path — and the mirror's own pushes coming back are not host changes.
void TestMirrorRecordsChangedOnHost() {
  using Kind = remote::MirrorSyncEngine::HostChange::Kind;
  MirrorSession session;
  WriteFile(session.host / "a.txt", "alpha\n");
  WriteFile(session.host / "doomed.txt", "bye\n");
  WriteFile(session.host / "mine.txt", "mine\n");
  session.Connect();
  session.Sync();
  Expect(session.engine->HostChanges().empty(), "the first sync is not a list of host changes");

  WriteFile(session.Tree("mine.txt"), "edited here\n");
  session.engine->NotifyLocalWrite("mine.txt");
  session.engine->Flush();
  session.Sync();
  Expect(session.engine->HostChanges().empty(), "a push the host echoes back is not a host change");

  WriteFile(session.host / "a.txt", "alpha v2\n");
  std::filesystem::remove(session.host / "doomed.txt");
  session.Sync();
  WriteFile(session.host / "new.txt", "n\n");
  session.Sync();
  const auto changes = session.engine->HostChanges();
  Expect(changes.size() == 3, "three host changes, got " + std::to_string(changes.size()));
  Expect(changes.size() == 3 && changes[0].path == "new.txt" && changes[0].kind == Kind::Added,
         "newest first: the added file");
  const auto find = [&](std::string_view path) -> const remote::MirrorSyncEngine::HostChange* {
    for (const auto& change : changes) {
      if (change.path == path) {
        return &change;
      }
    }
    return nullptr;
  };
  const auto* a = find("a.txt");
  Expect(a != nullptr && a->kind == Kind::Modified && a->previous == util::HashContent("alpha\n"),
         "a modification remembers the version the user last had");
  std::optional<std::filesystem::path> before;
  session.engine->FetchPreviousCopy("a.txt", [&](std::optional<std::filesystem::path> copy, std::string) {
    before = std::move(copy);
  });
  session.engine->Flush();
  Expect(before.has_value() && ReadFile(*before) == "alpha\n",
         "the version before the change comes back from the object store for a compare");
  const auto* doomed = find("doomed.txt");
  Expect(doomed != nullptr && doomed->kind == Kind::Deleted, "a host delete is listed");
  Expect(session.engine->status().host_changes >= 3, "the status counts them");

  std::filesystem::remove(session.host / "new.txt");
  session.Sync();
  const auto after = session.engine->HostChanges();
  Expect(after.size() == 2 && std::none_of(after.begin(), after.end(),
                                           [](const auto& change) { return change.path == "new.txt"; }),
         "added then deleted on the host is nothing the user ever had");
  session.engine->ClearHostChanges();
  Expect(session.engine->HostChanges().empty() && session.engine->status().host_changes == 0,
         "Clear empties the list");
}

// A pulled file keeps the host's mtime, so its recorded stat vouches for it at
// once (no rehash on the next reconcile) — and a same-size rewrite in the mirror
// still moves the stat and is caught.
void TestMirrorPulledFilesAreTrustedAtOnce() {
  MirrorSession session;
  WriteFile(session.host / "old.txt", "four\n");
  const auto an_hour_ago = std::filesystem::file_time_type::clock::now() - std::chrono::hours(1);
  std::filesystem::last_write_time(session.host / "old.txt", an_hour_ago);
  session.Connect();
  session.Sync();
  const remote::MirrorStore::Entry* entry = session.store->Find("old.txt");
  Expect(entry != nullptr && entry->local_known, "the pulled file's stat is recorded as trustworthy");
  Expect(std::filesystem::last_write_time(session.Tree("old.txt")) ==
             std::filesystem::last_write_time(session.host / "old.txt"),
         "it carries the host's mtime");
  WriteFile(session.Tree("old.txt"), "FOUR\n");  // same size, a tool's edit
  session.Sync();
  Expect(ReadFile(session.host / "old.txt") == "FOUR\n",
         "a same-size rewrite in the mirror is still seen, and pushed");
}

// A host file name that is not UTF-8 is still just bytes: it is listed, pulled
// and pushed like any other (paths ride JSON strings, which carry bytes verbatim
// between the two ends).
// A large file is pulled too — after the small ones, on its own — so the tree is
// complete; one over the per-file ceiling is not.
void TestMirrorPullsLargeFilesLast() {
  MirrorSession session;
  const std::string big(9 * 1024 * 1024 + 3, 'L');
  WriteFile(session.host / "big.bin", big);
  WriteFile(session.host / "huge.bin", std::string(2 * 1024 * 1024, 'H'));
  WriteFile(session.host / "small.txt", "s\n");
  remote::MirrorSyncEngine::Options options;
  options.max_file_bytes = 10 * 1024 * 1024;
  options.large_file_bytes = 1024 * 1024;
  session.Connect(options);
  session.Sync();
  Expect(ReadFile(session.Tree("small.txt")) == "s\n", "small files arrive");
  Expect(std::filesystem::exists(session.Tree("big.bin")) &&
             std::filesystem::file_size(session.Tree("big.bin")) == big.size(),
         "a large file arrives too");
  Expect(std::filesystem::exists(session.Tree("huge.bin")), "each large file, one after the other");
  options.max_file_bytes = 4 * 1024 * 1024;
  MirrorSession capped;
  WriteFile(capped.host / "big.bin", big);
  capped.Connect(options);
  capped.Sync();
  Expect(!std::filesystem::exists(capped.Tree("big.bin")) &&
             capped.engine->StateOf("big.bin") == ContentState::Absent,
         "a file over the ceiling stays absent, and says so");
}

// The workload's most common change — an agent rewriting one function in a big
// source file — arrives as a zstd delta against the tree's copy (§ 6.2), not as
// the file: the host diffs against the version this mirror holds, which for a
// committed file is its git index blob (the first sync's bytes are not kept on the
// host, TD-2026-10-28-417). A file edited here as well is never patched; that is a
// conflict, as before.
void TestMirrorPullsAnEditAsADelta() {
  MirrorSession session;
  std::string source;
  for (int i = 0; i < 4000; ++i) {
    source += "int function_" + std::to_string(i) + "(int x) { return x * " + std::to_string(i) + "; }\n";
  }
  InitializeGitRepo(session.host);
  WriteFile(session.host / "big.cpp", source);
  WriteFile(session.host / "small.txt", "v1\n");
  CommitAll(session.host, "init", "mirror delta");
  session.Connect();
  session.Sync();
  Expect(session.engine->status().pulled_as_delta == 0, "a first pull has nothing to diff against");

  std::string edited = source;
  const std::size_t at = edited.find("int function_2000(");
  edited.replace(at, edited.find('\n', at) - at, "int function_2000(int x) { return -x; }");
  WriteFile(session.host / "big.cpp", edited);
  WriteFile(session.host / "small.txt", "v2\n");
  session.Sync();
  Expect(ReadFile(session.Tree("big.cpp")) == edited && ReadFile(session.Tree("small.txt")) == "v2\n",
         "the mirror has the host's new bytes");
  // The 3-byte file's frame would outweigh the file, so it came whole.
  Expect(session.engine->status().pulled_as_delta == 1,
         "the big edit arrived as a delta, the tiny one whole: " +
             std::to_string(session.engine->status().pulled_as_delta));
  Expect(session.engine->StateOf("big.cpp") == ContentState::Current, "and the file is current");
}

// Bytes the mirror has held once never cross the link again (§ 6.2): a file an
// agent puts back, a branch switched back, comes out of meta/objects — checked
// against its hash like anything fetched. The sweep keeps what a path still
// needs and drops the rest, oldest first, down to the budget.
void TestMirrorRefillsKnownBytesFromItsObjectStore() {
  MirrorSession session;
  WriteFile(session.host / "a.txt", std::string(5000, 'A'));
  session.Connect();
  session.Sync();
  WriteFile(session.host / "a.txt", std::string(5000, 'B'));
  session.Sync();
  Expect(session.engine->status().pulled_from_store == 0, "new bytes come from the host");
  WriteFile(session.host / "a.txt", std::string(5000, 'A'));  // put back
  session.Sync();
  Expect(ReadFile(session.Tree("a.txt")) == std::string(5000, 'A'), "the mirror has the bytes back");
  Expect(session.engine->status().pulled_from_store == 1,
         "they came from the object store, not the link: " +
             std::to_string(session.engine->status().pulled_from_store));
  remote::MirrorObjects objects(session.mirror_dir / "meta" / "objects");
  const util::ContentHash a = util::HashContent(std::string(5000, 'A'));
  const util::ContentHash b = util::HashContent(std::string(5000, 'B'));
  Expect(objects.Has(a) && objects.Has(b), "both versions are stored under the default budget");
  const auto swept = objects.Sweep({a.Hex()}, 0);
  Expect(objects.Has(a) && !objects.Has(b) && swept.removed == 1,
         "a zero budget drops what nothing needs, never what a path does");
}

// A file past the old 64 MiB per-object ceiling — a dataset, a build artifact
// someone committed — is in the mirror too, with the defaults: it streams to disk
// as it arrives (never held in memory whole), is verified against its hash, and
// leaves no staging copy behind. It used to be absent, and invisible in the tree.
void TestMirrorPullsAFileOverSixtyFourMegabytes() {
  MirrorSession session;
  std::string big(64 * 1024 * 1024 + 4097, '\0');
  for (std::size_t i = 0; i < big.size(); i += 4093) {
    big[i] = static_cast<char>('a' + (i / 4093) % 26);  // not one repeated page
  }
  WriteFile(session.host / "data/huge.bin", big);
  WriteFile(session.host / "small.txt", "s\n");
  session.Connect();
  session.Sync();
  Expect(session.engine->status().error.empty(), "synced: " + session.engine->status().error);
  Expect(std::filesystem::exists(session.Tree("data/huge.bin")) &&
             util::HashFileContent(session.Tree("data/huge.bin")) == util::HashContent(big),
         "the whole file arrived, byte for byte");
  Expect(session.engine->StateOf("data/huge.bin") == ContentState::Current, "and it is current");
  std::error_code ignored;
  Expect(std::filesystem::is_empty(session.mirror_dir / "meta" / "incoming", ignored),
         "no staging copy is left behind");
}

// A tab opening a large file the backfill has not reached (large files go last,
// and this one is over what the backfill takes at all) streams it to disk at
// once instead of waiting for the whole tree.
void TestMirrorPrioritizedLargeFileStreamsAtOnce() {
  MirrorSession session;
  // Over the 64 MiB an in-memory interactive fetch may carry: before, Prioritize
  // asked for it in memory, was refused, and the tab waited for the backfill.
  std::string big(64 * 1024 * 1024 + 4097, '\0');
  for (std::size_t i = 0; i < big.size(); i += 4093) {
    big[i] = static_cast<char>('a' + (i / 4093) % 26);
  }
  WriteFile(session.host / "logs/big.log", big);
  WriteFile(session.host / "small.txt", "s\n");
  remote::MirrorSyncEngine::Options options;
  options.max_file_bytes = 1024 * 1024;  // the backfill leaves it
  session.Connect(options);
  session.Sync();
  Expect(!std::filesystem::exists(session.Tree("logs/big.log")) &&
             session.engine->StateOf("logs/big.log") == ContentState::Absent,
         "the backfill did not pull it");
  std::atomic<bool> landed{false};
  session.engine->Prioritize({"logs/big.log"}, [&] { landed = true; });
  session.engine->Flush();
  Expect(landed.load(), "the caller hears when it has landed (an editor opens it then)");
  Expect(util::HashFileContent(session.Tree("logs/big.log")) == util::HashContent(big) &&
             session.engine->StateOf("logs/big.log") == ContentState::Current,
         "opening it streams it now: " + session.engine->status().error);
  std::error_code ignored;
  Expect(std::filesystem::is_empty(session.mirror_dir / "meta" / "incoming", ignored),
         "no staging copy is left behind");
}

// A file the host cannot read has no hash, so the host leaves it out; the sync
// itself succeeds, everything else arrives, and it follows once it is readable.
void TestMirrorSkipsAnUnreadableHostFile() {
  MirrorSession session;
  WriteFile(session.host / "ok.txt", "ok\n");
  WriteFile(session.host / "secret.txt", "no\n");
  std::filesystem::permissions(session.host / "secret.txt", std::filesystem::perms::none);
  if (::access((session.host / "secret.txt").c_str(), R_OK) == 0) {
    return;  // running as root: nothing is unreadable
  }
  session.Connect();
  session.Sync();
  const auto status = session.engine->status();
  Expect(status.error.empty() && status.synced_once, "the sync succeeds: " + status.error);
  Expect(!std::filesystem::exists(session.Tree("secret.txt")), "the unreadable file is not mirrored");
  Expect(ReadFile(session.Tree("ok.txt")) == "ok\n", "everything else arrives");
  std::filesystem::permissions(session.host / "secret.txt", std::filesystem::perms::owner_all);
  session.Sync();
  Expect(session.engine->status().unreadable == 0 && ReadFile(session.Tree("secret.txt")) == "no\n",
         "made readable, the next sync pulls it");
}

void TestMirrorCarriesNonUtf8Names() {
  MirrorSession session;
  const std::string name("caf\xe9.txt");
  WriteFile(session.host / name, "latin-1 name\n");
  session.Connect();
  session.Sync();
  Expect(ReadFile(session.Tree(name)) == "latin-1 name\n", "pulled");
  WriteFile(session.Tree(name), "edited\n");
  session.engine->NotifyLocalWrite(name);
  session.engine->Flush();
  Expect(ReadFile(session.host / name) == "edited\n", "and pushed back");
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

// TD-2026-10-22-385: a watch row the host built before it acknowledged a push --
// the echo of the previous push -- reaches the client AFTER that acknowledgement
// over a slow link (the host sends deltas on the bulk lane, answers on the
// interactive one, and the serial queue runs the delta after the waiting push).
// Taking it pulled the old content back over the save; the next push expected it
// and conflicted, and every save after that stayed in the mirror. Delivered here
// through the engine's watch sink, as the host's watch would deliver it.
void TestMirrorIgnoresAWatchRowOlderThanItsPush() {
  MirrorSession session;
  WriteFile(session.host / "a.txt", "one\n");
  session.Connect();
  session.Sync();

  WriteFile(session.Tree("a.txt"), "two\n");
  session.engine->NotifyLocalWrite("a.txt");
  session.engine->Flush();
  Expect(ReadFile(session.host / "a.txt") == "two\n", "the save reaches the host");
  const std::uint64_t acknowledged = session.store->entries()["a.txt"].pushed_manifest;
  Expect(acknowledged > 0, "the host's answer says which manifest it had built by then");

  const auto row = [](std::string_view content) {
    return remote::ManifestRow{.path = "a.txt",
                               .kind = remote::ManifestEntryKind::File,
                               .size = content.size(),
                               .mode = 0644,
                               .mtime_ns = 0,
                               .hash = util::HashContent(content),
                               .link_target = {}};
  };
  // The echo of the FIRST content, from a manifest the host built before the push.
  session.engine->WatchSink()(remote::RemoteWorkspace::WatchDelta{
      .manifest_id = acknowledged, .deleted = {}, .rows = {row("one\n")}, .repository_moved = false});
  session.engine->Flush();
  Expect(ReadFile(session.Tree("a.txt")) == "two\n" &&
             session.engine->StateOf("a.txt") == ContentState::Current,
         "a row older than the push is not taken over the save");

  WriteFile(session.Tree("a.txt"), "three\n");
  session.engine->NotifyLocalWrite("a.txt");
  session.engine->Flush();
  Expect(ReadFile(session.host / "a.txt") == "three\n" &&
             session.engine->StateOf("a.txt") == ContentState::Current,
         "so the next save lands, with no conflict");

  // A row from a later manifest is the host's change, and is followed.
  WriteFile(session.host / "a.txt", "host four\n");
  session.engine->WatchSink()(remote::RemoteWorkspace::WatchDelta{
      .manifest_id = session.store->entries()["a.txt"].pushed_manifest + 1,
      .deleted = {},
      .rows = {row("host four\n")},
      .repository_moved = false});
  session.engine->Flush();
  Expect(ReadFile(session.Tree("a.txt")) == "host four\n",
         "a newer row is a host change and is pulled");
}

// The skip above compares manifest ids, and those are one server tree's sequence:
// a reconnect that reaches a new tree (the last connection to the old one went, or
// the server restarted) starts again at 1. A push acknowledged at id N before the
// link dropped then made every row for that path from the new tree's first N
// manifests read as "older than the push" -- including the full manifest the
// reconnect syncs from, which is skipped before it can re-plan the save journaled
// while offline. QA remote E ("journal flushed on reconnect") and F failed on every
// run once the QA host ran a server new enough to send the ids.
void TestMirrorReconnectToANewServerFlushesTheJournal() {
  MirrorSession session;
  WriteFile(session.host / "a.txt", "one\n");
  session.Connect();
  for (int i = 0; i < 6; ++i) {
    session.Sync();  // each full manifest is a new id
  }
  WriteFile(session.Tree("a.txt"), "two\n");
  session.engine->NotifyLocalWrite("a.txt");
  session.engine->Flush();
  Expect(ReadFile(session.host / "a.txt") == "two\n", "the save reaches the host");
  Expect(session.store->entries()["a.txt"].pushed_manifest > 3, "acknowledged well into the sequence");

  // The link drops; a save made meanwhile is journaled.
  session.connection->Replace(nullptr);
  WriteFile(session.Tree("a.txt"), "three\n");
  session.engine->NotifyLocalWrite("a.txt");
  session.engine->Flush();
  Expect(ReadFile(session.host / "a.txt") == "two\n" && session.engine->status().dirty == 1,
         "offline, the save waits in the journal");

  // Back, to a new server whose ids start again.
  session.client = session.StartServer();
  session.connection->Replace(session.client);
  session.Sync();
  Expect(ReadFile(session.host / "a.txt") == "three\n",
         "the reconnect's sync pushes the journaled save: '" + ReadFile(session.host / "a.txt") + "'");
  Expect(session.engine->StateOf("a.txt") == ContentState::Current && session.engine->status().dirty == 0,
         "and the file is current, nothing left unsent");
}

// A push the host applied but whose answer was lost with the link -- the client
// only learns "no answer" -- and a save after it. The next connection's push
// expects the old base and finds the host holding the lost push's bytes. That is
// the client's own write, not a host change: it becomes the base and the newer save
// lands over it. Taken for a conflict, it parked the file, and every later save of
// it stayed in the mirror (QA remote, slow link: D2's save landed unanswered, then
// E, F and J failed on LICENSE).
void TestMirrorAPushWhoseAnswerWasLostIsNotAConflict() {
  MirrorSession session;
  WriteFile(session.host / "a.txt", "one\n");
  session.Connect();
  session.Sync();
  // Swap in a server whose pid is known, so its answers can be withheld.
  const std::filesystem::path pid_file = session.temp.path() / "server.pid";
  session.client = session.StartServer(pid_file);
  session.connection->Replace(session.client);
  session.Sync();
  std::string pid_text = ReadFile(pid_file);
  while (!pid_text.empty() && (pid_text.back() == '\n' || pid_text.back() == ' ')) {
    pid_text.pop_back();
  }
  const std::optional<std::int64_t> pid = util::ParseInt64(pid_text);
  Expect(pid.has_value() && *pid > 0, "the server's pid is known");
  if (!pid.has_value() || *pid <= 0) {
    return;
  }
  ::kill(static_cast<pid_t>(*pid), SIGSTOP);

  // The push goes out; no answer can come.
  const std::size_t before = session.client->peer().pending_request_count();
  WriteFile(session.Tree("a.txt"), "two\n");
  session.engine->NotifyLocalWrite("a.txt");
  // pending_request_count counts a request once it is queued on the transport,
  // not when it is registered: counted from registration, the wait below once
  // saw the push before it was sent, the Fail beat the send, and the push was
  // (rightly) never-sent -- 1 run in ~15 on two cores (TD-2026-10-28-410).
  // It also leaves the link's own pings out: one going unanswered
  // to the stopped server here once passed for the push, which then found the
  // link failed, was never sent, and the host bytes the test wrote below really
  // were a host change (1 run in ~40 under load, CI's TSan job).
  Expect(WaitUntil([&] { return session.client->peer().pending_request_count() > before; },
                   std::chrono::seconds(10), std::chrono::milliseconds(1)),
         "the push is on the wire");
  // The link drops before the answer; the host had applied the write.
  session.client->peer().Fail("the link dropped mid-write");
  session.engine->Flush();
  ::kill(static_cast<pid_t>(*pid), SIGKILL);
  WriteFile(session.host / "a.txt", "two\n");
  Expect(session.engine->status().dirty == 1, "unanswered, the save stays in the journal");

  // Saved again while the link is down: never sent, so never a candidate.
  WriteFile(session.Tree("a.txt"), "three\n");
  session.engine->NotifyLocalWrite("a.txt");
  session.engine->Flush();

  session.client = session.StartServer();
  session.connection->Replace(session.client);
  session.Sync();
  {
    const auto status = session.engine->status();
    Expect(ReadFile(session.host / "a.txt") == "three\n",
           "the newer save lands over the lost push's bytes: '" + ReadFile(session.host / "a.txt") +
               "' (mirror '" + ReadFile(session.Tree("a.txt")) + "', state " +
               std::to_string(static_cast<int>(session.engine->StateOf("a.txt"))) + ", dirty " +
               std::to_string(status.dirty) + ", conflicts " + std::to_string(status.conflicts) + ", error '" +
               status.error + "')");
  }
  Expect(session.engine->StateOf("a.txt") == ContentState::Current && session.engine->status().conflicts == 0 &&
             session.engine->status().dirty == 0,
         "with no conflict and nothing left unsent");
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

// Nothing is settled automatically; the user's choice is: keep mine (the one
// unconditional write) or take the host's.
void TestMirrorResolvesConflictsTheUsersWay() {
  MirrorSession session;
  WriteFile(session.host / "mine.txt", "base\n");
  WriteFile(session.host / "theirs.txt", "base\n");
  session.Connect();
  session.Sync();
  for (const char* path : {"mine.txt", "theirs.txt"}) {
    WriteFile(session.host / path, "agent\n");
    WriteFile(session.Tree(path), "human\n");
    session.engine->NotifyLocalWrite(path);
  }
  session.engine->Flush();
  Expect(session.engine->Conflicts() == std::vector<std::string>{"mine.txt", "theirs.txt"},
         "both are conflicts");

  std::optional<std::filesystem::path> copy;
  std::string copy_error;
  session.engine->FetchHostCopy("theirs.txt", [&](std::optional<std::filesystem::path> path, std::string why) {
    copy = std::move(path);
    copy_error = std::move(why);
  });
  session.engine->Flush();
  Expect(copy.has_value() && ReadFile(*copy) == "agent\n" &&
             copy->string().find("/tree/") == std::string::npos,
         "Compare's host copy holds the host's bytes, outside the tree: " + copy_error);
  session.engine->ResolveConflict("mine.txt", remote::MirrorSyncEngine::Resolution::KeepMine);
  session.engine->ResolveConflict("theirs.txt", remote::MirrorSyncEngine::Resolution::TakeHost);
  session.engine->Flush();
  Expect(ReadFile(session.host / "mine.txt") == "human\n" &&
             ReadFile(session.Tree("mine.txt")) == "human\n",
         "keep mine overwrites the host's copy");
  Expect(ReadFile(session.host / "theirs.txt") == "agent\n" &&
             ReadFile(session.Tree("theirs.txt")) == "agent\n",
         "take the host's replaces the mirror's");
  Expect(session.engine->Conflicts().empty() &&
             session.engine->StateOf("mine.txt") == ContentState::Current &&
             session.engine->StateOf("theirs.txt") == ContentState::Current,
         "both are current again");
  // And they stay settled: the next sync finds nothing to do.
  session.Sync();
  Expect(session.engine->Conflicts().empty(), "a sync does not reopen them");
}

// A conflict merged three ways: the sides are the base both started from (kept in
// meta/objects), the host's current bytes and the mirror's file; the result is
// pushed only over the host version it was merged against.
void TestMirrorMergesAConflictThreeWays() {
  MirrorSession session;
  WriteFile(session.host / "merge.txt", "one\ntwo\nthree\n");
  session.Connect();
  session.Sync();
  WriteFile(session.host / "merge.txt", "ONE\ntwo\nthree\n");
  WriteFile(session.Tree("merge.txt"), "one\ntwo\nTHREE\n");
  session.engine->NotifyLocalWrite("merge.txt");
  session.engine->Flush();
  Expect(session.engine->Conflicts() == std::vector<std::string>{"merge.txt"}, "a conflict");

  const auto fetch_sides = [&] {
    std::optional<remote::MirrorSyncEngine::MergeSides> sides;
    std::string error;
    session.engine->FetchMergeSides("merge.txt", [&](std::optional<remote::MirrorSyncEngine::MergeSides> got,
                                                     std::string why) {
      sides = std::move(got);
      error = std::move(why);
    });
    session.engine->Flush();
    Expect(sides.has_value(), "the merge's sides are fetched: " + error);
    return sides.value_or(remote::MirrorSyncEngine::MergeSides{});
  };
  const remote::MirrorSyncEngine::MergeSides sides = fetch_sides();
  Expect(ReadFile(sides.base) == "one\ntwo\nthree\n" && ReadFile(sides.host) == "ONE\ntwo\nthree\n",
         "the base and the host's version");
  Expect(sides.host_hash == util::HashContent("ONE\ntwo\nthree\n"), "and which host version that is");
  Expect(sides.base.string().find("/tree/") == std::string::npos &&
             sides.host.string().find("/tree/") == std::string::npos,
         "both outside the tree");

  WriteFile(session.Tree("merge.txt"), "ONE\ntwo\nTHREE\n");  // the merge's result
  WriteFile(session.host / "merge.txt", "ONE!\ntwo\nthree\n");  // an agent, meanwhile
  session.engine->ResolveConflict("merge.txt", remote::MirrorSyncEngine::Resolution::KeepMine, sides.host_hash);
  session.engine->Flush();
  Expect(ReadFile(session.host / "merge.txt") == "ONE!\ntwo\nthree\n" &&
             session.engine->Conflicts() == std::vector<std::string>{"merge.txt"},
         "a host that moved on is not overwritten: still a conflict");
  Expect(session.engine->status().error.find("again") != std::string::npos,
         "and says why: " + session.engine->status().error);

  const remote::MirrorSyncEngine::MergeSides again = fetch_sides();
  session.engine->ResolveConflict("merge.txt", remote::MirrorSyncEngine::Resolution::KeepMine, again.host_hash);
  session.engine->Flush();
  Expect(ReadFile(session.host / "merge.txt") == "ONE\ntwo\nTHREE\n" && session.engine->Conflicts().empty() &&
             session.engine->StateOf("merge.txt") == ContentState::Current,
         "merged against the current version, the result reaches the host");
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

bool WaitFor(const std::function<bool()>& done, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (done()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return done();
}

void TestMirrorFollowsTheHostWatch() {
  MirrorSession session;
  WriteFile(session.host / "a.txt", "one\n");
  WriteFile(session.host / "gone.txt", "x\n");
  session.Connect();
  bool native = false;
  std::string error;
  Expect(session.workspace->SubscribeWatch(session.engine->WatchSink(), &native, &error),
         "subscribes: " + error);
  session.Sync();
  Expect(ReadFile(session.Tree("a.txt")) == "one\n", "the first sync lands");

  WriteFile(session.host / "a.txt", "two\n");
  WriteFile(session.host / "sub/new.txt", "new\n");
  std::filesystem::remove(session.host / "gone.txt");
  const bool followed = WaitFor(
      [&] {
        session.engine->Flush();
        return std::filesystem::exists(session.Tree("sub/new.txt")) &&
               ReadFile(session.Tree("a.txt")) == "two\n" &&
               !std::filesystem::exists(session.Tree("gone.txt"));
      },
      std::chrono::seconds(10));
  Expect(followed, std::string("an edit, a new file and a delete on the host reach the mirror with no ") +
                       "sync requested (" + (native ? "native" : "polling") + " watch)");

  // A local save echoed back by the watch changes nothing.
  WriteFile(session.Tree("a.txt"), "three\n");
  session.engine->NotifyLocalWrite("a.txt");
  session.engine->Flush();
  std::this_thread::sleep_for(std::chrono::milliseconds(600));
  session.engine->Flush();
  Expect(ReadFile(session.Tree("a.txt")) == "three\n" &&
             session.engine->StateOf("a.txt") == ContentState::Current,
         "our own push echoed by the watch is current, not a change");
}

// A mass delete is held in case the host root vanished (§ 6.3), but a branch
// switch deletes for git's reasons: when the host's HEAD moves with it
// (watch/moved), the mirror follows. Measured on a real host before this: a
// checkout 2,000 commits back left 2,960 files in the mirror that the host no
// longer had, behind a prompt with no Apply button.
void TestMirrorFollowsABranchSwitchThatDeletesMostFiles() {
  MirrorSession session;
  InitializeGitRepo(session.host);
  for (int i = 0; i < 150; ++i) {
    WriteFile(session.host / ("f" + std::to_string(i)), "x" + std::to_string(i));
  }
  CommitAll(session.host, "all", "branch switch");
  RequireGitCommandSuccess(session.host, {"checkout", "-q", "-b", "small"}, "branch");
  for (int i = 0; i < 140; ++i) {
    std::filesystem::remove(session.host / ("f" + std::to_string(i)));
  }
  RequireGitCommandSuccess(session.host, {"commit", "-qam", "small"}, "commit the deletes");
  RequireGitCommandSuccess(session.host, {"checkout", "-q", "-"}, "back to all");
  session.Connect();
  bool native = false;
  std::string error;
  Expect(session.workspace->SubscribeWatch(session.engine->WatchSink(), &native, &error),
         "subscribes: " + error);
  Expect(session.client->SubscribeGitStatus([](const std::string&) {}, &error), "git subscribed: " + error);
  session.Sync();
  Expect(std::filesystem::exists(session.Tree("f0")), "the first sync lands");

  RequireGitCommandSuccess(session.host, {"checkout", "-q", "small"}, "switch to the small branch");
  Expect(WaitFor(
             [&] {
               session.engine->Flush();
               return !std::filesystem::exists(session.Tree("f0")) &&
                      std::filesystem::exists(session.Tree("f149"));
             },
             std::chrono::seconds(10)),
         "the mirror follows the switch");
  Expect(session.engine->status().held_deletes == 0, "and nothing is held");

  RequireGitCommandSuccess(session.host, {"checkout", "-q", "-"}, "and back");
  Expect(WaitFor(
             [&] {
               session.engine->Flush();
               return std::filesystem::exists(session.Tree("f0")) && ReadFile(session.Tree("f0")) == "x0";
             },
             std::chrono::seconds(10)),
         "switching back brings the files back");
}

// The other side of the same rule: files removed with no HEAD move (an unmounted
// directory, an rm) are still held, and a held path the host lists again is not
// deleted when the hold is approved.
void TestMirrorHoldsAMassDeleteTheRepositoryDidNotExplain() {
  MirrorSession session;
  InitializeGitRepo(session.host);
  for (int i = 0; i < 150; ++i) {
    WriteFile(session.host / ("f" + std::to_string(i)), "x" + std::to_string(i));
  }
  CommitAll(session.host, "all", "held delete");
  session.Connect();
  bool native = false;
  std::string error;
  Expect(session.workspace->SubscribeWatch(session.engine->WatchSink(), &native, &error),
         "subscribes: " + error);
  Expect(session.client->SubscribeGitStatus([](const std::string&) {}, &error), "git subscribed: " + error);
  session.Sync();

  for (int i = 0; i < 140; ++i) {
    std::filesystem::remove(session.host / ("f" + std::to_string(i)));
  }
  Expect(WaitFor(
             [&] {
               session.engine->Flush();
               return session.engine->status().held_deletes == 140;
             },
             std::chrono::seconds(10)),
         "removed with no HEAD move, the delete is held: " +
             std::to_string(session.engine->status().held_deletes));
  // Past the host's repository sampler: nothing announces a move it did not see.
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  session.engine->Flush();
  Expect(session.engine->status().held_deletes == 140 && std::filesystem::exists(session.Tree("f0")),
         "and stays held");

  WriteFile(session.host / "f5", "back");
  Expect(WaitFor(
             [&] {
               session.engine->Flush();
               return session.engine->status().held_deletes == 139;
             },
             std::chrono::seconds(10)),
         "a held path the host lists again leaves the hold");
  session.engine->ApproveHeldDeletes();
  session.engine->Flush();
  Expect(!std::filesystem::exists(session.Tree("f0")), "approved, the rest apply");
  Expect(WaitFor(
             [&] {
               session.engine->Flush();
               return std::filesystem::exists(session.Tree("f5")) && ReadFile(session.Tree("f5")) == "back";
             },
             std::chrono::seconds(10)),
         "and the one that came back is kept, with the host's new bytes");
}

// TD-2026-10-27-402: a mass delete the watch delivers in two batches, each under
// the threshold alone, is held as one -- the first batch's deletes, already
// applied, are put back and held with the second's.
void TestMirrorHoldsAMassDeleteSplitAcrossBatches() {
  MirrorSession session;
  for (int i = 0; i < 150; ++i) {
    WriteFile(session.host / ("f" + std::to_string(i)), "x" + std::to_string(i));
  }
  session.Connect();
  bool native = false;
  std::string error;
  Expect(session.workspace->SubscribeWatch(session.engine->WatchSink(), &native, &error),
         "subscribes: " + error);
  session.Sync();
  for (int i = 0; i < 60; ++i) {
    std::filesystem::remove(session.host / ("f" + std::to_string(i)));
  }
  Expect(WaitFor(
             [&] {
               session.engine->Flush();
               return !std::filesystem::exists(session.Tree("f0")) &&
                      !std::filesystem::exists(session.Tree("f59"));
             },
             std::chrono::seconds(10)),
         "sixty deletes are under the threshold: applied");
  Expect(session.engine->status().held_deletes == 0, "and nothing is held");
  for (int i = 60; i < 120; ++i) {
    std::filesystem::remove(session.host / ("f" + std::to_string(i)));
  }
  Expect(WaitFor(
             [&] {
               session.engine->Flush();
               return session.engine->status().held_deletes == 120;
             },
             std::chrono::seconds(10)),
         "sixty more within the window make 120: all of them held: " +
             std::to_string(session.engine->status().held_deletes));
  Expect(ReadFile(session.Tree("f0")) == "x0" && ReadFile(session.Tree("f119")) == "x119",
         "the first batch's files are back, with their bytes, and the second's were never deleted");
  Expect(session.engine->StateOf("f0") == ContentState::Current, "as the host last listed them");
  session.engine->ApproveHeldDeletes();
  session.engine->Flush();
  Expect(!std::filesystem::exists(session.Tree("f0")) && !std::filesystem::exists(session.Tree("f119")) &&
             ReadFile(session.Tree("f120")) == "x120",
         "approved, all 120 apply and the rest stays");
  Expect(session.engine->status().held_deletes == 0, "and nothing is held after the approval");
}

// TD-2026-10-27-403: a pull the host answers "no such file" -- deleted while the
// backfill was on its way, with no delete delta to say so -- is the host's delete:
// the row stops counting as one to fetch.
void TestMirrorAPullOfAFileTheHostDeletedDropsTheRow() {
  MirrorSession session;
  WriteFile(session.host / "a.txt", "a\n");
  WriteFile(session.host / "big.txt", "more than ten bytes\n");
  remote::MirrorSyncEngine::Options options;
  options.max_file_bytes = 10;  // never backfilled: absent until a tab asks
  session.Connect(options);
  session.Sync();
  Expect(session.engine->status().absent == 1 && session.engine->StateOf("big.txt") == ContentState::Absent,
         "the large file is listed and not fetched");
  std::filesystem::remove(session.host / "big.txt");  // and no watch to report it
  session.engine->Prioritize({"big.txt"});
  session.engine->Flush();
  const auto status = session.engine->status();
  Expect(status.absent == 0, "the host's answer drops it from the files to fetch: " +
                                 std::to_string(status.absent));
  Expect(session.engine->StateOf("big.txt") == ContentState::Unknown, "it is not in the mirror at all");
  Expect(status.unreadable == 0, "and it is not an unreadable file");
}

// A rename and a delete made with no connection are journaled, survive the engine
// going away, and replay — in order, before the manifest is compared — on the next
// connection: the deleted file is not pulled back, the renamed one lands at its
// new path on the host.
void TestMirrorTreeOpsMadeOfflineReplay() {
  MirrorSession session;
  WriteFile(session.host / "old.txt", "moving\n");
  WriteFile(session.host / "doomed.txt", "bye\n");
  session.Connect();
  session.Sync();
  {
    // Offline: an engine whose connection has no client.
    remote::RemoteWorkspace offline(std::make_shared<remote::RemoteConnection>(nullptr));
    remote::MirrorSyncEngine engine(offline, *session.store);
    remote::MirrorWriteGate gate(*session.store, engine);
    session.engine.reset();
    using TreeOp = project::FileWriteGate::TreeOp;
    const std::vector<TreeOp> ops = {
        TreeOp{.kind = TreeOp::Kind::Rename, .path = session.Tree("old.txt"),
               .new_path = session.Tree("new.txt")},
        TreeOp{.kind = TreeOp::Kind::Delete, .path = session.Tree("doomed.txt")},
    };
    Expect(gate.ApplyTreeOps(ops).ok, "the ops apply locally while offline");
    engine.Flush();
    Expect(session.store->pending_tree_ops().size() == 2, "and wait in the journal");
  }
  session.Connect();  // a new engine over a reopened store
  Expect(session.store->pending_tree_ops().size() == 2, "the journal survived the restart");
  session.Sync();
  Expect(!std::filesystem::exists(session.host / "old.txt") &&
             ReadFile(session.host / "new.txt") == "moving\n",
         "the rename reached the host");
  Expect(!std::filesystem::exists(session.host / "doomed.txt"), "the delete reached the host");
  Expect(!std::filesystem::exists(session.Tree("doomed.txt")) &&
             !std::filesystem::exists(session.Tree("old.txt")),
         "and nothing was pulled back into the mirror");
  Expect(session.store->pending_tree_ops().empty(), "the journal is empty");
}

void TestMirrorWriteGateReachesTheHost() {
  MirrorSession session;
  WriteFile(session.host / "a.txt", "host\n");
  WriteFile(session.host / "dir/b.txt", "b\n");
  WriteFile(session.host / "c.txt", "c\n");
  session.Connect();
  session.Sync();
  remote::MirrorWriteGate gate(*session.store, *session.engine);

  const auto saved = gate.WriteText(session.Tree("a.txt"), "saved\n",
                                    project::FileWriteGate::Signature::Capture);
  Expect(saved.ok && saved.signature.exists, "the save lands locally at once, with its signature");
  Expect(ReadFile(session.Tree("a.txt")) == "saved\n", "in the mirror");
  session.engine->Flush();
  Expect(ReadFile(session.host / "a.txt") == "saved\n", "and then on the host");

  using TreeOp = project::FileWriteGate::TreeOp;
  const std::vector<TreeOp> ops = {
      TreeOp{.kind = TreeOp::Kind::Rename, .path = session.Tree("dir/b.txt"),
             .new_path = session.Tree("moved/b.txt")},
      TreeOp{.kind = TreeOp::Kind::Delete, .path = session.Tree("c.txt")},
      TreeOp{.kind = TreeOp::Kind::CreateFile, .path = session.Tree("empty.txt")},
      TreeOp{.kind = TreeOp::Kind::CreateDirectory, .path = session.Tree("newdir")},
  };
  const auto applied = gate.ApplyTreeOps(ops);
  Expect(applied.ok, "the tree ops apply locally: " + applied.error_message);
  session.engine->Flush();
  Expect(ReadFile(session.host / "moved/b.txt") == "b\n" && !std::filesystem::exists(session.host / "dir/b.txt"),
         "a rename is replayed on the host");
  Expect(!std::filesystem::exists(session.host / "c.txt"), "a delete is replayed");
  Expect(std::filesystem::exists(session.host / "empty.txt") &&
             std::filesystem::file_size(session.host / "empty.txt") == 0,
         "a created file is created");
  Expect(std::filesystem::is_directory(session.host / "newdir"), "a created directory is created");
  Expect(session.engine->StateOf("moved/b.txt") == ContentState::Current,
         "the renamed file keeps its base under its new name");

  // A path outside the mirror is a plain local write.
  const auto outside = gate.WriteText(session.temp.path() / "elsewhere.txt", "x");
  Expect(outside.ok && !std::filesystem::exists(session.host / "elsewhere.txt"),
         "a write outside tree/ stays local");
}

// § 7.3: the host's files the mirror has not fetched are published for the tree
// and quick-open to list, with the folders they imply; each leaves the list once it
// has landed.
void TestMirrorListsWhatItHasNotFetched() {
  MirrorSession session;
  WriteFile(session.host / "small.txt", "s\n");
  WriteFile(session.host / "logs/a.log", std::string(2 * 1024 * 1024, 'a'));
  WriteFile(session.host / "logs/deep/b.log", std::string(2 * 1024 * 1024, 'b'));
  remote::MirrorSyncEngine::Options options;
  options.max_file_bytes = 1024 * 1024;  // the backfill leaves both logs
  session.Connect(options);
  Expect(session.engine->PendingPaths() == nullptr, "nothing is pending before the manifest");
  session.Sync();
  const project::SharedPendingPaths pending = session.engine->PendingPaths();
  Expect(pending != nullptr &&
             pending->files() == std::vector<std::string>{"logs/a.log", "logs/deep/b.log"},
         "the files not fetched are listed");
  Expect(session.engine->status().pending_revision > 0, "and the status says the list changed");
  const auto root = pending->ChildrenOf("");
  const auto logs = pending->ChildrenOf("logs");
  Expect(root.size() == 1 && root[0].name == "logs" && root[0].is_directory, "the root holds a pending folder");
  Expect(logs.size() == 2 && logs[0].name == "a.log" && !logs[0].is_directory && logs[1].name == "deep" &&
             logs[1].is_directory,
         "which holds a file and a folder");
  session.engine->Prioritize({"logs/a.log", "logs/deep/b.log"});
  session.engine->Flush();
  Expect(session.engine->StateOf("logs/a.log") == ContentState::Current, "opened, it is fetched");
  Expect(session.engine->PendingPaths() == nullptr, "and nothing is pending any more");
}

}  // namespace

void RegisterMirrorSyncEngineTests(std::vector<TestCase>& tests) {
  AddTest(tests, "MirrorSyncEngine/InitialSyncAndHostChanges", TestMirrorInitialSyncAndHostChanges);
  AddTest(tests, "MirrorSyncEngine/PushesLocalWritesUnderCompareAndSwap",
          TestMirrorPushesLocalWritesUnderCompareAndSwap);
  AddTest(tests, "MirrorSyncEngine/IgnoresAWatchRowOlderThanItsPush",
          TestMirrorIgnoresAWatchRowOlderThanItsPush);
  AddTest(tests, "MirrorSyncEngine/ReconnectToANewServerFlushesTheJournal",
          TestMirrorReconnectToANewServerFlushesTheJournal);
  AddTest(tests, "MirrorSyncEngine/APushWhoseAnswerWasLostIsNotAConflict",
          TestMirrorAPushWhoseAnswerWasLostIsNotAConflict);
  AddTest(tests, "MirrorSyncEngine/ParksConflictsAndNeverOverwritesLocalEdits",
          TestMirrorParksConflictsAndNeverOverwritesLocalEdits);
  AddTest(tests, "MirrorSyncEngine/HoldsAMassDelete", TestMirrorHoldsAMassDelete);
  AddTest(tests, "MirrorSyncEngine/HoldsAMassDeleteSplitAcrossBatches",
          TestMirrorHoldsAMassDeleteSplitAcrossBatches);
  AddTest(tests, "MirrorSyncEngine/APullOfAFileTheHostDeletedDropsTheRow",
          TestMirrorAPullOfAFileTheHostDeletedDropsTheRow);
  AddTest(tests, "MirrorSyncEngine/FollowsABranchSwitchThatDeletesMostFiles",
          TestMirrorFollowsABranchSwitchThatDeletesMostFiles);
  AddTest(tests, "MirrorSyncEngine/HoldsAMassDeleteTheRepositoryDidNotExplain",
          TestMirrorHoldsAMassDeleteTheRepositoryDidNotExplain);
  AddTest(tests, "MirrorSyncEngine/RecordsChangedOnHost", TestMirrorRecordsChangedOnHost);
  AddTest(tests, "MirrorSyncEngine/ListsWhatItHasNotFetched", TestMirrorListsWhatItHasNotFetched);
  AddTest(tests, "MirrorSyncEngine/CarriesNonUtf8Names", TestMirrorCarriesNonUtf8Names);
  AddTest(tests, "MirrorSyncEngine/SkipsAnUnreadableHostFile", TestMirrorSkipsAnUnreadableHostFile);
  AddTest(tests, "MirrorSyncEngine/PullsLargeFilesLast", TestMirrorPullsLargeFilesLast);
  AddTest(tests, "MirrorSyncEngine/FlushesAPullOncePerSaveNotPerFile",
          TestMirrorFlushesAPullOncePerSaveNotPerFile);
  AddTest(tests, "MirrorSyncEngine/PrioritizedLargeFileStreamsAtOnce",
          TestMirrorPrioritizedLargeFileStreamsAtOnce);
  AddTest(tests, "MirrorSyncEngine/PullsAnEditAsADelta", TestMirrorPullsAnEditAsADelta);
  AddTest(tests, "MirrorSyncEngine/RefillsKnownBytesFromItsObjectStore",
          TestMirrorRefillsKnownBytesFromItsObjectStore);
  AddTest(tests, "MirrorSyncEngine/PullsAFileOverSixtyFourMegabytes",
          TestMirrorPullsAFileOverSixtyFourMegabytes);
  AddTest(tests, "MirrorSyncEngine/PulledFilesAreTrustedAtOnce", TestMirrorPulledFilesAreTrustedAtOnce);
  AddTest(tests, "MirrorSyncEngine/ResolvesConflictsTheUsersWay", TestMirrorResolvesConflictsTheUsersWay);
  AddTest(tests, "MirrorSyncEngine/MergesAConflictThreeWays", TestMirrorMergesAConflictThreeWays);
  AddTest(tests, "MirrorSyncEngine/JournalSurvivesARestart", TestMirrorJournalSurvivesARestart);
  AddTest(tests, "MirrorSyncEngine/WriteGateReachesTheHost", TestMirrorWriteGateReachesTheHost);
  AddTest(tests, "MirrorSyncEngine/TreeOpsMadeOfflineReplay", TestMirrorTreeOpsMadeOfflineReplay);
  AddTest(tests, "MirrorSyncEngine/FollowsTheHostWatch", TestMirrorFollowsTheHostWatch);
}

}  // namespace microide::tests
