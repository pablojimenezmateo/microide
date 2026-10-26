#include "TestSupport.h"

#include "platform/DirectoryWalk.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace microide::tests {
namespace {

using microide::platform::PathType;
using microide::platform::TreeWalkVisitor;
using microide::platform::WalkAction;
using microide::platform::WalkDirectoryTree;
using microide::platform::WalkEntry;
using microide::platform::WalkOptions;
using microide::platform::WalkStatus;

struct Seen {
  std::string path;
  PathType type;
  bool is_symlink;
  int depth;
};

// Records every visit and leave, and descends everywhere unless told otherwise.
class RecordingVisitor final : public TreeWalkVisitor {
 public:
  WalkAction Visit(const WalkEntry& entry) override {
    Expect(entry.path.data()[entry.path.size()] == '\0', "an entry's path is NUL-terminated");
    Expect(entry.path.ends_with(entry.name) && entry.path[entry.path.size() - entry.name.size() - 1] == '/',
           "an entry's name is its path's last component");
    seen.push_back(Seen{std::string(entry.path), entry.type, entry.is_symlink, entry.depth});
    if (entry.name == prune_name) {
      return WalkAction::Prune;
    }
    if (entry.name == stop_name) {
      return WalkAction::Stop;
    }
    return WalkAction::Continue;
  }
  void Leave(const WalkEntry& directory) override { left.emplace_back(directory.path); }

  std::vector<Seen> seen;
  std::vector<std::string> left;
  std::string prune_name;
  std::string stop_name;
};

const Seen* Find(const std::vector<Seen>& seen, const std::filesystem::path& path) {
  const auto it = std::find_if(seen.begin(), seen.end(),
                               [&](const Seen& s) { return s.path == path.native(); });
  return it == seen.end() ? nullptr : &*it;
}

std::size_t IndexOf(const std::vector<Seen>& seen, const std::filesystem::path& path) {
  for (std::size_t i = 0; i < seen.size(); ++i) {
    if (seen[i].path == path.native()) {
      return i;
    }
  }
  return seen.size();
}

void TestWalkVisitsEveryEntryPreOrderWithTypesAndDepths() {
  TemporaryDirectory temp;
  const std::filesystem::path root = temp.path() / "tree";
  WriteFile(root / "a.txt", "a");
  WriteFile(root / "sub" / "b.txt", "b");
  WriteFile(root / "sub" / "deeper" / "c.txt", "c");
  std::filesystem::create_symlink(root / "sub", root / "link-to-dir");
  std::filesystem::create_symlink(root / "a.txt", root / "link-to-file");
  std::filesystem::create_symlink(root / "nowhere", root / "dangling");

  RecordingVisitor visitor;
  const WalkStatus status = WalkDirectoryTree(root, WalkOptions{}, visitor);
  Expect(!status.root_unreadable && !status.incomplete && !status.stopped,
         "a readable tree walks completely");

  // Every entry, compared with the standard iterator's view of the same tree.
  std::set<std::string> expected;
  for (auto it = std::filesystem::recursive_directory_iterator(root); it != decltype(it){}; ++it) {
    expected.insert(it->path().native());
  }
  std::set<std::string> actual;
  for (const Seen& s : visitor.seen) {
    actual.insert(s.path);
  }
  Expect(actual == expected, "the walk visits exactly what recursive_directory_iterator does");

  const Seen* sub = Find(visitor.seen, root / "sub");
  const Seen* deeper_file = Find(visitor.seen, root / "sub" / "deeper" / "c.txt");
  Expect(sub != nullptr && sub->type == PathType::Directory && sub->depth == 1 && !sub->is_symlink,
         "a directory at the top is a depth-1 Directory");
  Expect(deeper_file != nullptr && deeper_file->type == PathType::RegularFile &&
             deeper_file->depth == 3,
         "depth counts levels below the root");
  Expect(IndexOf(visitor.seen, root / "sub") < IndexOf(visitor.seen, root / "sub" / "b.txt"),
         "a directory is visited before what is in it");

  const Seen* link_dir = Find(visitor.seen, root / "link-to-dir");
  const Seen* link_file = Find(visitor.seen, root / "link-to-file");
  const Seen* dangling = Find(visitor.seen, root / "dangling");
  Expect(link_dir != nullptr && link_dir->is_symlink && link_dir->type == PathType::Directory,
         "a link to a directory is a Directory, and says it is a link");
  Expect(Find(visitor.seen, root / "link-to-dir" / "b.txt") == nullptr,
         "a directory link is not followed by default");
  Expect(link_file != nullptr && link_file->is_symlink && link_file->type == PathType::RegularFile,
         "a link to a file is a RegularFile");
  Expect(dangling != nullptr && dangling->is_symlink && dangling->type == PathType::Missing,
         "a dangling link is Missing");

  std::vector<std::string> left = visitor.left;
  std::sort(left.begin(), left.end());
  Expect(left == std::vector<std::string>{(root / "sub").native(), (root / "sub" / "deeper").native()},
         "Leave fires once for every directory the walk entered, and for no other");
}

void TestWalkPrunesStopsAndFollowsWhenAsked() {
  TemporaryDirectory temp;
  const std::filesystem::path root = temp.path() / "tree";
  WriteFile(root / "keep" / "x.txt", "x");
  WriteFile(root / "skip" / "y.txt", "y");
  std::filesystem::create_symlink(root / "keep", root / "linked");

  RecordingVisitor pruning;
  pruning.prune_name = "skip";
  (void) WalkDirectoryTree(root, WalkOptions{}, pruning);
  Expect(Find(pruning.seen, root / "skip") != nullptr &&
             Find(pruning.seen, root / "skip" / "y.txt") == nullptr,
         "a pruned directory is visited and not entered");
  Expect(std::find(pruning.left.begin(), pruning.left.end(), (root / "skip").native()) ==
             pruning.left.end(),
         "and a directory that was not entered is not left");

  RecordingVisitor following;
  (void) WalkDirectoryTree(root, WalkOptions{.follow_directory_symlinks = true}, following);
  Expect(Find(following.seen, root / "linked" / "x.txt") != nullptr,
         "with following on, a directory link is entered");

  RecordingVisitor stopping;
  stopping.stop_name = "x.txt";
  const WalkStatus stopped = WalkDirectoryTree(root, WalkOptions{}, stopping);
  Expect(stopped.stopped && stopping.seen.back().path == (root / "keep" / "x.txt").native(),
         "Stop ends the walk at the entry that asked");

  // A lambda visitor through the adapter.
  std::size_t files = 0;
  (void) WalkDirectoryTree(root, WalkOptions{}, [&files](const WalkEntry& entry) {
    files += entry.type == PathType::RegularFile ? 1 : 0;
    return WalkAction::Continue;
  });
  Expect(files == 2, "the lambda form walks the same tree");
}

void TestWalkReportsWhatItCouldNotRead() {
  TemporaryDirectory temp;
  const std::filesystem::path root = temp.path() / "tree";
  WriteFile(root / "a" / "b" / "c" / "d.txt", "d");
  RecordingVisitor shallow;
  const WalkStatus depth_limited =
      WalkDirectoryTree(root, WalkOptions{.max_depth = 2}, shallow);
  Expect(depth_limited.truncated_by_depth && Find(shallow.seen, root / "a" / "b") != nullptr &&
             Find(shallow.seen, root / "a" / "b" / "c") == nullptr,
         "a directory past max_depth is visited and not entered, and the walk says so");

  RecordingVisitor missing;
  const WalkStatus no_root = WalkDirectoryTree(root / "absent", WalkOptions{}, missing);
  Expect(no_root.root_unreadable && missing.seen.empty(), "a missing root is unreadable");

  if (::geteuid() != 0) {  // root reads any directory
    WriteFile(root / "locked" / "secret.txt", "s");
    ::chmod((root / "locked").c_str(), 0);
    RecordingVisitor locked;
    const WalkStatus limited = WalkDirectoryTree(root, WalkOptions{}, locked);
    ::chmod((root / "locked").c_str(), 0755);
    Expect(limited.permission_limited && !limited.incomplete &&
               Find(locked.seen, root / "a" / "b" / "c" / "d.txt") != nullptr,
           "an unreadable directory is reported and skipped; the rest still walks");
  }
}

}  // namespace

void RegisterDirectoryWalkTests(std::vector<TestCase>& tests) {
  AddTest(tests, "DirectoryWalk/VisitsEveryEntryPreOrderWithTypesAndDepths",
          TestWalkVisitsEveryEntryPreOrderWithTypesAndDepths);
  AddTest(tests, "DirectoryWalk/PrunesStopsAndFollowsWhenAsked", TestWalkPrunesStopsAndFollowsWhenAsked);
  AddTest(tests, "DirectoryWalk/ReportsWhatItCouldNotRead", TestWalkReportsWhatItCouldNotRead);
}

}  // namespace microide::tests
