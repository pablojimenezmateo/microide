#include "TestSupport.h"

#include "project/remote/MirrorStore.h"
#include "util/ByteCodec.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace microide::tests {
namespace {

using project::remote::ManifestEntryKind;
using project::remote::MirrorStore;

void TestMirrorStoreRoundTrips() {
  TemporaryDirectory temp;
  std::string error;
  {
    MirrorStore store(temp.path() / "m");
    Expect(store.Open(&error), "a new mirror opens empty: " + error);
    Expect(std::filesystem::is_directory(store.tree()), "tree/ exists");
    store.set_manifest_id(42);
    MirrorStore::Entry& file = store.entries()["src/a.c"];
    file.has_remote = true;
    file.remote.size = 3;
    file.remote.mode = 0755;
    file.remote.mtime_ns = -7;
    file.remote.hash = util::HashContent("abc");
    file.base = util::HashContent("abc");
    file.local_known = true;
    file.local_size = 3;
    file.local_mtime_ns = 1234567890123;
    file.push_pending = true;
    MirrorStore::Entry& link = store.entries()["lnk"];
    link.has_remote = true;
    link.remote.kind = ManifestEntryKind::Symlink;
    link.remote.link_target = "src/a.c";
    MirrorStore::Entry& local_only = store.entries()[".env"];
    local_only.local_only = true;
    local_only.conflict = true;
    Expect(store.Save(&error), "it saves: " + error);
  }
  MirrorStore reopened(temp.path() / "m");
  Expect(reopened.Open(&error), "it reopens: " + error);
  Expect(reopened.manifest_id() == 42 && reopened.entries().size() == 3, "every entry is back");
  const MirrorStore::Entry* file = reopened.Find("src/a.c");
  Expect(file != nullptr && file->push_pending && file->base == util::HashContent("abc") &&
             file->remote.mode == 0755 && file->remote.mtime_ns == -7 && file->local_known &&
             file->local_mtime_ns == 1234567890123,
         "a file entry survives intact, its pending push included");
  const MirrorStore::Entry* link = reopened.Find("lnk");
  Expect(link != nullptr && link->remote.kind == ManifestEntryKind::Symlink &&
             link->remote.link_target == "src/a.c" && !link->base.has_value(),
         "a link entry survives");
  const MirrorStore::Entry* env = reopened.Find(".env");
  Expect(env != nullptr && env->local_only && env->conflict && !env->has_remote,
         "a local-only entry survives");
}

// The state is written lazily, the journal at once: a reopened mirror takes the
// pending pushes and tree operations from the journal when it is newer.
void TestMirrorStoreJournalOverridesALaggingState() {
  TemporaryDirectory temp;
  std::string error;
  {
    MirrorStore store(temp.path() / "m");
    Expect(store.Open(&error), "opens");
    store.entries()["a.txt"].push_pending = true;
    store.entries()["b.txt"];
    Expect(store.Save(&error), "the full state (a.txt pending)");
    store.entries()["a.txt"].push_pending = false;  // acknowledged
    store.entries()["b.txt"].push_pending = true;   // a new save
    store.pending_tree_ops().push_back(
        MirrorStore::PendingTreeOp{MirrorStore::PendingTreeOp::Kind::Delete, "gone.txt", {}});
    Expect(store.SaveJournal(&error), "only the journal after that");
  }
  MirrorStore reopened(temp.path() / "m");
  Expect(reopened.Open(&error), "reopens: " + error);
  Expect(!reopened.Find("a.txt")->push_pending && reopened.Find("b.txt")->push_pending,
         "the journal's pending set wins over the older state");
  Expect(reopened.pending_tree_ops().size() == 1 && reopened.pending_tree_ops()[0].path == "gone.txt",
         "and its tree operations");
}

// A pull moves its entry's base in memory and the state is saved lazily: between
// the two a crash leaves the pulled bytes in tree/ unrecorded (read back as an edit
// made here). The journal, written at once, carries them (NotePulled) and Open
// adopts each whose file still holds them -- or whose path has a save journaled
// after the pull, made on top of them -- and leaves alone one written since.
void TestMirrorStoreJournalCarriesPullsTheStateLacks() {
  TemporaryDirectory temp;
  std::string error;
  const auto old = util::HashContent("one\n");
  const auto pulled = util::HashContent("two\n");
  {
    MirrorStore store(temp.path() / "m");
    Expect(store.Open(&error), "opens");
    for (const char* name : {"a.txt", "saved.txt", "edited.txt"}) {
      MirrorStore::Entry& entry = store.entries()[name];
      entry.has_remote = true;
      entry.remote.hash = old;
      entry.base = old;
      WriteFile(store.tree() / name, "one\n");
    }
    Expect(store.Save(&error), "the state, every base at the old version");
    // Then, before any save: three pulls land, one is saved over through the
    // gate, one is rewritten outside it.
    for (const char* name : {"a.txt", "saved.txt", "edited.txt"}) {
      WriteFile(store.tree() / name, "two\n");
      store.Find(name)->base = pulled;
      store.NotePulled(name, pulled);
    }
    WriteFile(store.tree() / "saved.txt", "user\n");
    store.Find("saved.txt")->push_pending = true;
    WriteFile(store.tree() / "edited.txt", "tool\n");
    Expect(store.WritePulledHint(), "only the hint (what a pull batch writes)");
    Expect(store.SaveJournal(&error), "and the journal (what the save made through the gate writes): " + error);
  }
  {
    MirrorStore reopened(temp.path() / "m");
    Expect(reopened.Open(&error), "reopens: " + error);
    Expect(reopened.Find("a.txt")->base == std::optional(pulled), "the pulled bytes are the base");
    Expect(reopened.Find("saved.txt")->base == std::optional(pulled) && reopened.Find("saved.txt")->push_pending,
           "a save journaled after the pull was made on top of it: its base is the pull");
    Expect(reopened.Find("edited.txt")->base == std::optional(old),
           "a file rewritten since holds neither: left for the reconcile");
    // Kept until a Save records them, in case a journal-only write comes first
    // (SaveJournal rewrites the hint); a Save then drops the hint.
    Expect(reopened.SaveJournal(&error), "the journal again");
    MirrorStore again(temp.path() / "m");
    Expect(again.Open(&error) && again.Find("a.txt")->base == std::optional(pulled), "still carried");
    Expect(again.Save(&error), "the state");
  }
  {
    MirrorStore reopened(temp.path() / "m");
    Expect(reopened.Open(&error) && reopened.Find("a.txt")->base == std::optional(pulled), "recorded by the state");
    Expect(!std::filesystem::exists(reopened.pulled_hint_path()), "and the hint is gone");
    // A hint that does not decode is no hint, never an error.
    WriteFile(reopened.pulled_hint_path(), "garbage");
    MirrorStore tolerant(temp.path() / "m");
    Expect(tolerant.Open(&error) && tolerant.Find("a.txt")->base == std::optional(pulled),
           "a corrupt hint is ignored: " + error);
  }
}

// meta/pulled is appended per pull batch, never rewritten: rewritten whole per
// batch (and per journal write) it cost O(paths pulled since the last save) each
// time -- quadratic over a first sync, and the largest source of the allocator
// slack TD-2026-10-28-418 measured. A later record for a path wins; a torn last
// record (a crash mid-append) is dropped and appends after it still read; a
// version-1 hint written before the format changed is still adopted.
void TestMirrorStorePulledHintIsAppendedAndSurvivesATornTail() {
  TemporaryDirectory temp;
  std::string error;
  const auto base = util::HashContent("base\n");
  const auto first = util::HashContent("first\n");
  const auto second = util::HashContent("second\n");
  const std::filesystem::path dir = temp.path() / "m";
  {
    MirrorStore store(dir);
    Expect(store.Open(&error), "opens");
    for (const char* name : {"a.txt", "b.txt"}) {
      MirrorStore::Entry& entry = store.entries()[name];
      entry.has_remote = true;
      entry.remote.hash = base;
      entry.base = base;
      WriteFile(store.tree() / name, "base\n");
    }
    Expect(store.Save(&error), "the state");
    // Two batches, each pulling a.txt.
    WriteFile(store.tree() / "a.txt", "first\n");
    store.NotePulled("a.txt", first);
    Expect(store.WritePulledHint(), "batch one");
    WriteFile(store.tree() / "a.txt", "second\n");
    store.NotePulled("a.txt", second);
    Expect(store.WritePulledHint(), "batch two");
    Expect(store.WritePulledHint(), "a write with nothing new adds nothing");
    // A version byte, then two records of (length 5, "a.txt", 32-byte hash).
    Expect(std::filesystem::file_size(store.pulled_hint_path()) == 1 + 2 * (1 + 5 + 32),
           "each batch appended its own record: " +
               std::to_string(std::filesystem::file_size(store.pulled_hint_path())));
  }
  {
    MirrorStore reopened(dir);
    Expect(reopened.Open(&error) && reopened.Find("a.txt")->base == std::optional(second),
           "the later record wins: " + error);
  }
  {
    std::ofstream torn(MirrorStore(dir).pulled_hint_path(), std::ios::binary | std::ios::app);
    torn << "\x05a.t";  // a record cut short
  }
  {
    MirrorStore reopened(dir);
    Expect(reopened.Open(&error) && reopened.Find("a.txt")->base == std::optional(second),
           "a torn last record is dropped, the rest kept");
    WriteFile(reopened.tree() / "b.txt", "first\n");
    reopened.NotePulled("b.txt", first);
    Expect(reopened.WritePulledHint(), "an append after the torn tail");
  }
  {
    MirrorStore reopened(dir);
    Expect(reopened.Open(&error) && reopened.Find("b.txt")->base == std::optional(first) &&
               reopened.Find("a.txt")->base == std::optional(second),
           "the hint was rewritten whole, so what came after the tear is read");
  }
  {
    // Version 1: a count, then the records.
    std::string v1;
    util::PutVarint(v1, 1);
    util::PutVarint(v1, 1);
    util::PutBytes(v1, "b.txt");
    v1.append(second.raw());
    MirrorStore probe(dir);
    WriteFile(probe.tree() / "b.txt", "second\n");
    WriteFile(probe.pulled_hint_path(), v1);
    MirrorStore reopened(dir);
    Expect(reopened.Open(&error) && reopened.Find("b.txt")->base == std::optional(second),
           "a version-1 hint is adopted");
  }
}

void TestMirrorStoreCorruptStateIsAnError() {
  TemporaryDirectory temp;
  std::string error;
  MirrorStore store(temp.path() / "m");
  Expect(store.Open(&error), "opens");
  WriteFile(store.state_path(), "not a record");
  std::filesystem::remove(store.state_path().string() + ".bak");
  MirrorStore reopened(temp.path() / "m");
  Expect(!reopened.Open(&error) && error.find("unreadable") != std::string::npos,
         "a corrupt state file is refused, never read as an empty mirror: " + error);
}

void TestMirrorStoreCheckLocal() {
  TemporaryDirectory temp;
  std::string error;
  MirrorStore store(temp.path() / "m");
  Expect(store.Open(&error), "opens");
  MirrorStore::Entry entry;
  entry.base = util::HashContent("base");
  Expect(store.CheckLocal("f.txt", entry) == MirrorStore::LocalState::Missing, "missing");
  WriteFile(store.tree() / "f.txt", "base");
  Expect(store.CheckLocal("f.txt", entry) == MirrorStore::LocalState::MatchesBase,
         "matching bytes match, found by hash");
  Expect(!entry.local_known,
         "a stat seconds old does not vouch for the bytes (a same-size rewrite in the same "
         "tick would keep it): the next check hashes again");
  // Backdated (what a pull does with the host's mtime), it does.
  std::filesystem::last_write_time(store.tree() / "f.txt",
                                   std::filesystem::file_time_type::clock::now() - std::chrono::hours(1));
  WriteFile(store.tree() / "g.txt", "base");
  MirrorStore::Entry old_entry;
  old_entry.base = util::HashContent("base");
  std::filesystem::last_write_time(store.tree() / "g.txt",
                                   std::filesystem::file_time_type::clock::now() - std::chrono::hours(1));
  Expect(store.CheckLocal("g.txt", old_entry) == MirrorStore::LocalState::MatchesBase &&
             old_entry.local_known,
         "an old mtime vouches once the bytes are confirmed");
  WriteFile(store.tree() / "f.txt", "edit");
  Expect(store.CheckLocal("f.txt", entry) == MirrorStore::LocalState::Differs, "an edit differs");
  MirrorStore::Entry no_base;
  Expect(store.CheckLocal("f.txt", no_base) == MirrorStore::LocalState::Differs,
         "a file with no base to match differs");
}

void TestMirrorStoreDefaultDirectory() {
  const auto a = MirrorStore::DefaultDirectory("build-box", "/home/u/src/app");
  const auto b = MirrorStore::DefaultDirectory("build-box", "/home/v/src/app");
  Expect(a != b && a.parent_path() == b.parent_path(), "two roots with one basename differ");
  Expect(a.filename().string().rfind("app-", 0) == 0, "the slug starts with the basename");
  const auto odd = MirrorStore::DefaultDirectory("../evil/host", "/x");
  Expect(odd.parent_path().filename() == ".._evil_host", "a host string is one safe component");
}

}  // namespace

void RegisterMirrorStoreTests(std::vector<TestCase>& tests) {
  AddTest(tests, "MirrorStore/RoundTrips", TestMirrorStoreRoundTrips);
  AddTest(tests, "MirrorStore/CorruptStateIsAnError", TestMirrorStoreCorruptStateIsAnError);
  AddTest(tests, "MirrorStore/JournalOverridesALaggingState", TestMirrorStoreJournalOverridesALaggingState);
  AddTest(tests, "MirrorStore/PulledHintIsAppendedAndSurvivesATornTail",
          TestMirrorStorePulledHintIsAppendedAndSurvivesATornTail);
  AddTest(tests, "MirrorStore/JournalCarriesPullsTheStateLacks", TestMirrorStoreJournalCarriesPullsTheStateLacks);
  AddTest(tests, "MirrorStore/CheckLocal", TestMirrorStoreCheckLocal);
  AddTest(tests, "MirrorStore/DefaultDirectory", TestMirrorStoreDefaultDirectory);
}

}  // namespace microide::tests
