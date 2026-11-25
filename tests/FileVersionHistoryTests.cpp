#include "TestSupport.h"

#include "project/remote/FileVersionHistory.h"
#include "util/ContentHash.h"

#include <string>
#include <vector>

namespace microide::tests {
namespace {

using microide::project::remote::FileVersionHistory;
using microide::util::ContentHash;
using microide::util::HashContent;

void TestFileVersionHistoryListsNewestFirstWithinItsBounds() {
  FileVersionHistory history;
  Expect(history.VersionsOf("a.txt").empty() && history.empty(), "nothing recorded, nothing listed");
  history.Record("a.txt", HashContent("v1"), 100);
  history.Record("a.txt", HashContent("v2"), 200);
  history.Record("b.txt", HashContent("b1"), 150);
  std::vector<FileVersionHistory::Version> a = history.VersionsOf("a.txt");
  Expect(a.size() == 2 && a[0].hash == HashContent("v2") && a[0].replaced_at == 200 &&
             a[1].hash == HashContent("v1"),
         "a path's versions, newest first");
  Expect(history.VersionsOf("b.txt").size() == 1 && history.path_count() == 2, "each path has its own");

  history.Record("a.txt", HashContent("v2"), 300);
  a = history.VersionsOf("a.txt");
  Expect(a.size() == 2 && a[0].replaced_at == 300, "the same content twice in a row is listed once");

  for (int i = 0; i < 40; ++i) {
    history.Record("busy.txt", HashContent("r" + std::to_string(i)), 1000 + static_cast<std::uint64_t>(i));
  }
  const std::vector<FileVersionHistory::Version> busy = history.VersionsOf("busy.txt");
  Expect(busy.size() == FileVersionHistory::kMaxVersionsPerPath && busy[0].hash == HashContent("r39"),
         "a file rewritten many times keeps its newest versions");

  history.Record("", HashContent("x"), 1);
  history.Record(std::string(FileVersionHistory::kMaxPathBytes + 1, 'p'), HashContent("x"), 1);
  Expect(history.path_count() == 3, "an empty or absurd path is not recorded");

  FileVersionHistory full;
  for (std::size_t i = 0; i < FileVersionHistory::kMaxPaths; ++i) {
    full.Record("f" + std::to_string(i), HashContent("c"), 10 + i);
  }
  full.Record("newcomer", HashContent("c"), 999999);
  Expect(full.path_count() == FileVersionHistory::kMaxPaths && full.VersionsOf("f0").empty() &&
             full.VersionsOf("f1").size() == 1 && full.VersionsOf("newcomer").size() == 1,
         "the cap evicts the stalest path");
}

void TestFileVersionHistorySurvivesItsFileFormAndDistrustsIt() {
  FileVersionHistory history;
  history.Record("src/a.cpp", HashContent("v1"), 100);
  history.Record("src/a.cpp", HashContent("v2"), 200);
  history.Record("b.txt", HashContent("b1"), 150);
  const std::string encoded = history.Encode();
  const FileVersionHistory decoded = FileVersionHistory::Decode(encoded);
  Expect(decoded.VersionsOf("src/a.cpp") == history.VersionsOf("src/a.cpp") &&
             decoded.VersionsOf("b.txt") == history.VersionsOf("b.txt") && decoded.path_count() == 2,
         "what was encoded is what decodes");

  Expect(FileVersionHistory::Decode("").empty() && FileVersionHistory::Decode("\x07garbage").empty(),
         "an empty file or another format's is no history");
  for (std::size_t cut = 0; cut < encoded.size(); ++cut) {
    const FileVersionHistory partial = FileVersionHistory::Decode(encoded.substr(0, cut));
    Expect(partial.path_count() <= 2, "a truncated file decodes to at most what it held");
    for (const char* path : {"src/a.cpp", "b.txt"}) {
      for (const FileVersionHistory::Version& version : partial.VersionsOf(path)) {
        Expect(version.hash == HashContent("v1") || version.hash == HashContent("v2") ||
                   version.hash == HashContent("b1"),
               "and never a version it did not hold");
      }
    }
  }
  std::string forged = encoded.substr(0, 1);
  forged += "\xff\xff\xff\xff\xff\xff\xff\xff\x7f";
  Expect(FileVersionHistory::Decode(forged).empty(), "a forged path count yields nothing");
}

}

void RegisterFileVersionHistoryTests(std::vector<TestCase>& tests) {
  AddTest(tests, "FileVersionHistory/ListsNewestFirstWithinItsBounds",
          TestFileVersionHistoryListsNewestFirstWithinItsBounds);
  AddTest(tests, "FileVersionHistory/SurvivesItsFileFormAndDistrustsIt",
          TestFileVersionHistorySurvivesItsFileFormAndDistrustsIt);
}

}
