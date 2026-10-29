#include "TestSupport.h"
#include "support/MirrorSession.h"

#include "project/FileWriteGate.h"
#include "project/remote/MirrorSyncEngine.h"
#include "project/remote/MirrorWriteGate.h"
#include "project/remote/RemoteConnection.h"
#include "project/remote/RemoteWorkspace.h"

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// The full matrix of a local tree operation against a remote one, on the same
// file or its directory: edit, create, delete, rename, move, both sides, in both
// orderings a user meets -- the local operation made while connected against a
// base the host had already moved on from, and made offline then replayed on
// reconnect through the journal. Every cell must end with no error, no conflict
// left, the two trees equal, and no bytes either side wrote lost without a
// conflict having asked the user. The cells whose answer is fixed (a conflict
// must be raised; a conflict must not be) say so.
//
// Written after a host that archived a folder (a rename of its directory) left
// every locally edited file under it a conflict Take Remote could not settle, a
// save the host never received of a file it deleted stayed "unsent" with no row
// forever, and a folder deleted here never went from the host at all.
namespace microide::tests {
namespace {

namespace remote = project::remote;
using Resolution = remote::MirrorSyncEngine::Resolution;
using TreeOp = project::FileWriteGate::TreeOp;

enum class LocalOp { None, Edit, Create, Delete, Rename, MoveDir, DeleteDir };
enum class RemoteOp { None, Edit, Create, Delete, Rename, RenameElsewhere, MoveDir, MoveDirElsewhere, DeleteDir };
enum class Ordering { Live, Offline };

constexpr RemoteOp kRemoteOps[] = {RemoteOp::None,    RemoteOp::Edit,            RemoteOp::Create,
                                   RemoteOp::Delete,  RemoteOp::Rename,          RemoteOp::RenameElsewhere,
                                   RemoteOp::MoveDir, RemoteOp::MoveDirElsewhere, RemoteOp::DeleteDir};

const char* Name(LocalOp op) {
  switch (op) {
    case LocalOp::None: return "nothing";
    case LocalOp::Edit: return "edit dir/a.txt";
    case LocalOp::Create: return "create dir/n.txt";
    case LocalOp::Delete: return "delete dir/a.txt";
    case LocalOp::Rename: return "rename dir/a.txt to dir/r.txt";
    case LocalOp::MoveDir: return "move dir to moved";
    case LocalOp::DeleteDir: return "delete dir";
  }
  return "?";
}

const char* Name(RemoteOp op) {
  switch (op) {
    case RemoteOp::None: return "nothing";
    case RemoteOp::Edit: return "edit dir/a.txt";
    case RemoteOp::Create: return "create dir/n.txt";
    case RemoteOp::Delete: return "delete dir/a.txt";
    case RemoteOp::Rename: return "rename dir/a.txt to dir/r.txt";
    case RemoteOp::RenameElsewhere: return "rename dir/a.txt to dir/h.txt";
    case RemoteOp::MoveDir: return "move dir to moved";
    case RemoteOp::MoveDirElsewhere: return "move dir to hmoved";
    case RemoteOp::DeleteDir: return "delete dir";
  }
  return "?";
}

void ApplyRemote(RemoteOp op, const std::filesystem::path& host) {
  switch (op) {
    case RemoteOp::None: break;
    case RemoteOp::Edit: WriteFile(host / "dir/a.txt", "remote\n"); break;
    case RemoteOp::Create: WriteFile(host / "dir/n.txt", "remote\n"); break;
    case RemoteOp::Delete: std::filesystem::remove(host / "dir/a.txt"); break;
    case RemoteOp::Rename: std::filesystem::rename(host / "dir/a.txt", host / "dir/r.txt"); break;
    case RemoteOp::RenameElsewhere: std::filesystem::rename(host / "dir/a.txt", host / "dir/h.txt"); break;
    case RemoteOp::MoveDir: std::filesystem::rename(host / "dir", host / "moved"); break;
    case RemoteOp::MoveDirElsewhere: std::filesystem::rename(host / "dir", host / "hmoved"); break;
    case RemoteOp::DeleteDir: std::filesystem::remove_all(host / "dir"); break;
  }
}

void ApplyLocal(LocalOp op, MirrorSession& session, remote::MirrorWriteGate& gate, const std::string& label) {
  const auto ops = [&](std::vector<TreeOp> list) {
    const auto applied = gate.ApplyTreeOps(list);
    Expect(applied.ok, label + ": the local operation applies: " + applied.error_message);
    gate.DisposeStaged(applied.staged_directories);  // as the shell does, off its thread
  };
  switch (op) {
    case LocalOp::None:
      break;
    case LocalOp::Edit: {
      const auto written = gate.WriteText(session.Tree("dir/a.txt"), "local\n");
      Expect(written.ok, label + ": the local save lands");
      break;
    }
    case LocalOp::Create: {
      const auto written = gate.WriteText(session.Tree("dir/n.txt"), "local\n");
      Expect(written.ok, label + ": the local create lands");
      break;
    }
    case LocalOp::Delete:
      ops({TreeOp{.kind = TreeOp::Kind::Delete, .path = session.Tree("dir/a.txt")}});
      break;
    case LocalOp::Rename:
      ops({TreeOp{.kind = TreeOp::Kind::Rename, .path = session.Tree("dir/a.txt"), .new_path = session.Tree("dir/r.txt")}});
      break;
    case LocalOp::MoveDir:
      ops({TreeOp{.kind = TreeOp::Kind::Rename, .path = session.Tree("dir"), .new_path = session.Tree("moved")}});
      break;
    case LocalOp::DeleteDir:
      ops({TreeOp{.kind = TreeOp::Kind::Delete, .path = session.Tree("dir"), .recursive = true}});
      break;
  }
}

using Snapshot = std::map<std::string, std::string>;

Snapshot Files(const std::filesystem::path& root) {
  Snapshot files;
  std::error_code error;
  for (auto it = std::filesystem::recursive_directory_iterator(root, error);
       !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
    if (it->is_regular_file(error)) {
      files[it->path().lexically_relative(root).generic_string()] = ReadFile(it->path());
    }
  }
  return files;
}

std::string Describe(const Snapshot& files) {
  std::string text = "{";
  for (const auto& [path, content] : files) {
    text += " " + path + "=" + content.substr(0, content.find('\n'));
  }
  return text + " }";
}

bool Holds(const Snapshot& files, std::string_view bytes) {
  for (const auto& [path, content] : files) {
    if (content == bytes) {
      return true;
    }
  }
  return false;
}

// The cells whose answer is fixed. Everything else may or may not conflict, as
// long as the invariants hold.
enum class Verdict { Either, MustConflict, MustNotConflict };

Verdict Expected(LocalOp local, RemoteOp host) {
  using L = LocalOp;
  using R = RemoteOp;
  if (local == L::None || host == R::None) {
    return Verdict::MustNotConflict;  // one side alone is never a conflict
  }
  switch (local) {
    case L::Edit:
      return host == R::Create ? Verdict::MustNotConflict : Verdict::MustConflict;
    case L::Create:
      return host == R::Create ? Verdict::MustConflict : Verdict::MustNotConflict;
    case L::Delete:
      // The host's newer bytes are never put back silently, and a delete both
      // sides made is settled.
      return host == R::Edit ? Verdict::MustConflict
             : host == R::Delete || host == R::Create || host == R::DeleteDir ? Verdict::MustNotConflict
                                                                              : Verdict::Either;
    case L::Rename:
      // A rename the host refused (the file edited, deleted or moved there) parks
      // the renamed copy; both sides renaming to the same name agree.
      return host == R::Create || host == R::Rename ? Verdict::MustNotConflict : Verdict::MustConflict;
    case L::MoveDir:
      return host == R::MoveDirElsewhere || host == R::DeleteDir ? Verdict::MustConflict
                                                                 : Verdict::MustNotConflict;
    case L::DeleteDir:
      return host == R::Edit ? Verdict::MustConflict
             : host == R::Create || host == R::Delete || host == R::DeleteDir ? Verdict::MustNotConflict
                                                                              : Verdict::Either;
    case L::None:
      break;
  }
  return Verdict::Either;
}

// One cell. Returns whether a conflict was raised, so the caller can run the
// other resolution on the same cell.
bool RunCell(LocalOp local, RemoteOp host, Ordering ordering, Resolution resolution) {
  const std::string label = std::string(ordering == Ordering::Live ? "live" : "offline") + ": local " +
                            Name(local) + " vs remote " + Name(host) + ", " +
                            (resolution == Resolution::KeepLocal ? "Keep Local" : "Take Remote");
  MirrorSession session;
  WriteFile(session.host / "dir/a.txt", "base\n");
  WriteFile(session.host / "dir/b.txt", "base\n");
  session.Connect();
  session.Sync();
  Expect(session.engine->status().error.empty(), label + ": the first sync: " + session.engine->status().error);
  if (ordering == Ordering::Live) {
    // The host moved on; the local operation is checked against the base.
    ApplyRemote(host, session.host);
    remote::MirrorWriteGate gate(*session.store, *session.engine);
    ApplyLocal(local, session, gate, label);
    session.engine->Flush();
  } else {
    // Made offline, journaled; the host moves on; replayed on reconnect.
    session.engine.reset();
    {
      remote::RemoteWorkspace offline(std::make_shared<remote::RemoteConnection>(nullptr));
      remote::MirrorSyncEngine engine(offline, *session.store);
      remote::MirrorWriteGate gate(*session.store, engine);
      ApplyLocal(local, session, gate, label);
      engine.Flush();
    }
    ApplyRemote(host, session.host);
    session.Connect();
  }
  session.Sync();
  auto status = session.engine->status();
  Expect(status.error.empty(), label + ": no error after the sync: " + status.error);
  const std::vector<std::string> conflicts = session.engine->Conflicts();
  const bool conflicted = !conflicts.empty();
  const auto list = [&] {
    std::string all;
    for (const std::string& path : conflicts) all += " " + path;
    return all;
  };
  switch (Expected(local, host)) {
    case Verdict::MustConflict:
      Expect(conflicted, label + ": must be a conflict (host " + Describe(Files(session.host)) + ", mirror " +
                             Describe(Files(session.mirror_dir / "tree")) + ")");
      break;
    case Verdict::MustNotConflict:
      Expect(!conflicted, label + ": must not be a conflict, is:" + list());
      break;
    case Verdict::Either:
      break;
  }
  // The conflicts' own report: a side it names as gone is gone.
  for (const remote::MirrorSyncEngine::Conflict& conflict : session.engine->ConflictDetails()) {
    Expect(conflict.in_mirror == std::filesystem::exists(session.Tree(conflict.path)),
           label + ": " + conflict.path + " says in_mirror=" + (conflict.in_mirror ? "yes" : "no"));
    Expect(conflict.on_host || !std::filesystem::exists(session.host / conflict.path),
           label + ": " + conflict.path + " says the host has no copy, it has");
  }
  for (const std::string& path : conflicts) {
    session.engine->ResolveConflict(path, resolution);
  }
  session.engine->Flush();
  session.Sync();
  session.Sync();
  status = session.engine->status();
  Expect(status.error.empty(), label + ": no error after settling: " + status.error);
  Expect(session.engine->Conflicts().empty(), label + ": nothing left in conflict:" + list());
  Expect(status.dirty == 0, label + ": nothing left unsent: " + std::to_string(status.dirty));
  const Snapshot host_files = Files(session.host);
  const Snapshot mirror_files = Files(session.mirror_dir / "tree");
  Expect(host_files == mirror_files, label + ": the trees agree: host " + Describe(host_files) + " mirror " +
                                         Describe(mirror_files));
  // No silent loss: bytes a side wrote survive unless a conflict asked, and then
  // the chosen side's do.
  const bool local_wrote = local == LocalOp::Edit || local == LocalOp::Create;
  const bool remote_wrote = host == RemoteOp::Edit || host == RemoteOp::Create;
  if (local_wrote && (!conflicted || resolution == Resolution::KeepLocal)) {
    Expect(Holds(host_files, "local\n"), label + ": the local bytes survive: " + Describe(host_files));
  }
  if (remote_wrote && (!conflicted || resolution == Resolution::TakeRemote)) {
    Expect(Holds(host_files, "remote\n"), label + ": the remote bytes survive: " + Describe(host_files));
  }
  return conflicted;
}

void RunRow(LocalOp local, Ordering ordering) {
  for (const RemoteOp host : kRemoteOps) {
    if (RunCell(local, host, ordering, Resolution::KeepLocal)) {
      RunCell(local, host, ordering, Resolution::TakeRemote);
    }
  }
}

template <LocalOp kLocal, Ordering kOrdering>
void TestRow() {
  RunRow(kLocal, kOrdering);
}

}  // namespace

void RegisterMirrorSyncMatrixTests(std::vector<TestCase>& tests) {
  AddTest(tests, "MirrorSyncMatrix/Live/Nothing", TestRow<LocalOp::None, Ordering::Live>);
  AddTest(tests, "MirrorSyncMatrix/Live/Edit", TestRow<LocalOp::Edit, Ordering::Live>);
  AddTest(tests, "MirrorSyncMatrix/Live/Create", TestRow<LocalOp::Create, Ordering::Live>);
  AddTest(tests, "MirrorSyncMatrix/Live/Delete", TestRow<LocalOp::Delete, Ordering::Live>);
  AddTest(tests, "MirrorSyncMatrix/Live/Rename", TestRow<LocalOp::Rename, Ordering::Live>);
  AddTest(tests, "MirrorSyncMatrix/Live/MoveDir", TestRow<LocalOp::MoveDir, Ordering::Live>);
  AddTest(tests, "MirrorSyncMatrix/Live/DeleteDir", TestRow<LocalOp::DeleteDir, Ordering::Live>);
  AddTest(tests, "MirrorSyncMatrix/Offline/Nothing", TestRow<LocalOp::None, Ordering::Offline>);
  AddTest(tests, "MirrorSyncMatrix/Offline/Edit", TestRow<LocalOp::Edit, Ordering::Offline>);
  AddTest(tests, "MirrorSyncMatrix/Offline/Create", TestRow<LocalOp::Create, Ordering::Offline>);
  AddTest(tests, "MirrorSyncMatrix/Offline/Delete", TestRow<LocalOp::Delete, Ordering::Offline>);
  AddTest(tests, "MirrorSyncMatrix/Offline/Rename", TestRow<LocalOp::Rename, Ordering::Offline>);
  AddTest(tests, "MirrorSyncMatrix/Offline/MoveDir", TestRow<LocalOp::MoveDir, Ordering::Offline>);
  AddTest(tests, "MirrorSyncMatrix/Offline/DeleteDir", TestRow<LocalOp::DeleteDir, Ordering::Offline>);
}

}  // namespace microide::tests
