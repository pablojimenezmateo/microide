#include "TestSupport.h"

#include "project/remote/MirrorStore.h"

#include <filesystem>
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
    file.remote.path = "src/a.c";
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
    link.remote.path = "lnk";
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
  AddTest(tests, "MirrorStore/CheckLocal", TestMirrorStoreCheckLocal);
  AddTest(tests, "MirrorStore/DefaultDirectory", TestMirrorStoreDefaultDirectory);
}

}  // namespace microide::tests
