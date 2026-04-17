#include "TestSupport.h"

#include "editor/DiagnosticsStore.h"

#include <filesystem>
#include <utility>
#include <vector>

namespace microide::tests {
namespace {

using microide::editor::Diagnostic;
using microide::editor::DiagnosticSeverity;
using microide::editor::DiagnosticsStore;
using microide::editor::SelectionRange;
using microide::editor::TextPosition;

Diagnostic MakeDiagnostic(std::size_t start_line,
                          std::size_t start_column,
                          std::size_t end_line,
                          std::size_t end_column,
                          DiagnosticSeverity severity,
                          std::string message) {
  return Diagnostic{
      .range = SelectionRange{
          .start = TextPosition{start_line, start_column},
          .end = TextPosition{end_line, end_column},
      },
      .severity = severity,
      .message = std::move(message),
  };
}

void TestDiagnosticsStoreMergesOwnersPerFile() {
  DiagnosticsStore store;
  const std::filesystem::path path = "/tmp/project/main.cpp";

  Expect(store.ReplaceForOwnerFile(
             "eslint", path,
             {MakeDiagnostic(0, 4, 0, 9, DiagnosticSeverity::Warning, "unused value")}),
         "first owner diagnostics should publish");
  Expect(store.ReplaceForOwnerFile(
             "spellcheck", path,
             {MakeDiagnostic(1, 0, 1, 5, DiagnosticSeverity::Hint, "typo")}),
         "second owner diagnostics should publish");

  const auto* merged = store.FindByPath(path);
  Expect(merged != nullptr && merged->size() == 2,
         "merged diagnostics should expose entries from both owners");
  Expect((*merged)[0].owner == "eslint" && (*merged)[0].message == "unused value",
         "merged diagnostics should keep the first owner's diagnostic metadata");
  Expect((*merged)[1].owner == "spellcheck" && (*merged)[1].message == "typo",
         "merged diagnostics should keep the second owner's diagnostic metadata");

  const auto eslint_snapshot = store.SnapshotForOwner("eslint");
  Expect(eslint_snapshot.size() == 1 && eslint_snapshot.front().path == path,
         "owner snapshots should preserve per-owner file diagnostics");
}

void TestDiagnosticsStoreClearsOwnersIndependently() {
  DiagnosticsStore store;
  const std::filesystem::path path = "/tmp/project/main.cpp";

  Expect(store.ReplaceForOwnerFile(
             "eslint", path,
             {MakeDiagnostic(0, 0, 0, 1, DiagnosticSeverity::Error, "broken")}),
         "initial owner diagnostics should publish");
  Expect(store.ReplaceForOwnerFile(
             "clang-tidy", path,
             {MakeDiagnostic(0, 2, 0, 4, DiagnosticSeverity::Info, "note")}),
         "second owner diagnostics should publish");
  Expect(store.ClearOwnerFile("eslint", path),
         "clearing one owner's file diagnostics should report a change");

  const auto* remaining = store.FindByPath(path);
  Expect(remaining != nullptr && remaining->size() == 1 &&
             remaining->front().owner == "clang-tidy",
         "clearing one owner's file diagnostics should preserve other owners");

  Expect(store.ClearOwner("clang-tidy"),
         "clearing the remaining owner should report a change");
  Expect(store.FindByPath(path) == nullptr,
         "clearing the last owner should remove the merged file snapshot");
}

void TestDiagnosticsStoreSnapshotAllSortsAcrossFiles() {
  DiagnosticsStore store;
  const std::filesystem::path alpha = "/tmp/project/alpha.cpp";
  const std::filesystem::path beta = "/tmp/project/src/beta.cpp";

  Expect(store.ReplaceForOwnerFile(
             "lint", beta,
             {MakeDiagnostic(3, 2, 3, 6, DiagnosticSeverity::Warning, "late warning")}),
         "beta diagnostics should publish");
  Expect(store.ReplaceForOwnerFile(
             "lint", alpha,
             {MakeDiagnostic(1, 0, 1, 4, DiagnosticSeverity::Error, "early error"),
              MakeDiagnostic(1, 8, 1, 9, DiagnosticSeverity::Hint, "late hint")}),
         "alpha diagnostics should publish");

  const auto snapshot = store.SnapshotAll();
  Expect(snapshot.size() == 3, "snapshot-all should flatten merged diagnostics from every file");
  Expect(snapshot[0].path == alpha && snapshot[0].message == "early error",
         "snapshot-all should sort by path and line");
  Expect(snapshot[1].path == alpha && snapshot[1].message == "late hint",
         "snapshot-all should preserve later diagnostics from the same file");
  Expect(snapshot[2].path == beta && snapshot[2].message == "late warning",
         "snapshot-all should include diagnostics from later-sorted files");
}

void TestDiagnosticsStoreRetargetsPathPrefixes() {
  DiagnosticsStore store;
  const std::filesystem::path old_dir = "/tmp/project/src";
  const std::filesystem::path new_dir = "/tmp/project/renamed-src";
  const std::filesystem::path old_main = old_dir / "main.cpp";
  const std::filesystem::path old_nested = old_dir / "nested" / "util.cpp";
  const std::filesystem::path untouched = "/tmp/project/README.md";

  Expect(store.ReplaceForOwnerFile(
             "eslint", old_main,
             {MakeDiagnostic(0, 0, 0, 4, DiagnosticSeverity::Warning, "rename me")}),
         "retarget fixture should publish a top-level diagnostic");
  Expect(store.ReplaceForOwnerFile(
             "clang-tidy", old_nested,
             {MakeDiagnostic(1, 2, 1, 6, DiagnosticSeverity::Error, "rename nested")}),
         "retarget fixture should publish a nested diagnostic");
  Expect(store.ReplaceForOwnerFile(
             "eslint", untouched,
             {MakeDiagnostic(0, 0, 0, 1, DiagnosticSeverity::Info, "keep")}),
         "retarget fixture should publish an unrelated diagnostic");

  Expect(store.RetargetPathPrefix(old_dir, new_dir),
         "retargeting a diagnostics subtree should report a change");
  Expect(store.FindByPath(old_main) == nullptr && store.FindByPath(old_nested) == nullptr,
         "retarget should remove the old merged file keys");

  const auto* moved_main = store.FindByPath(new_dir / "main.cpp");
  const auto* moved_nested = store.FindByPath(new_dir / "nested" / "util.cpp");
  Expect(moved_main != nullptr && moved_main->size() == 1 &&
             moved_main->front().path == (new_dir / "main.cpp") &&
             moved_main->front().message == "rename me",
         "retarget should move top-level diagnostics to the renamed path");
  Expect(moved_nested != nullptr && moved_nested->size() == 1 &&
             moved_nested->front().path == (new_dir / "nested" / "util.cpp") &&
             moved_nested->front().message == "rename nested",
         "retarget should move nested diagnostics to the renamed subtree");

  const auto* untouched_snapshot = store.FindByPath(untouched);
  Expect(untouched_snapshot != nullptr && untouched_snapshot->size() == 1 &&
             untouched_snapshot->front().message == "keep",
         "retarget should preserve diagnostics outside the renamed subtree");
}

void TestDiagnosticsStoreClearsPathPrefixes() {
  DiagnosticsStore store;
  const std::filesystem::path doomed_dir = "/tmp/project/generated";
  const std::filesystem::path doomed_file = doomed_dir / "report.txt";
  const std::filesystem::path doomed_nested = doomed_dir / "nested" / "results.txt";
  const std::filesystem::path survivor = "/tmp/project/src/main.cpp";

  Expect(store.ReplaceForOwnerFile(
             "lint", doomed_file,
             {MakeDiagnostic(0, 0, 0, 4, DiagnosticSeverity::Warning, "delete file")}),
         "clear-prefix fixture should publish one doomed file");
  Expect(store.ReplaceForOwnerFile(
             "lint", doomed_nested,
             {MakeDiagnostic(1, 0, 1, 5, DiagnosticSeverity::Error, "delete nested")}),
         "clear-prefix fixture should publish one doomed nested file");
  Expect(store.ReplaceForOwnerFile(
             "lint", survivor,
             {MakeDiagnostic(2, 1, 2, 3, DiagnosticSeverity::Hint, "keep survivor")}),
         "clear-prefix fixture should publish a survivor file");

  Expect(store.ClearPathPrefix(doomed_dir),
         "clearing a diagnostics subtree should report a change");
  Expect(store.FindByPath(doomed_file) == nullptr && store.FindByPath(doomed_nested) == nullptr,
         "clear-prefix should remove diagnostics inside the deleted subtree");
  const auto* survivor_snapshot = store.FindByPath(survivor);
  Expect(survivor_snapshot != nullptr && survivor_snapshot->size() == 1 &&
             survivor_snapshot->front().message == "keep survivor",
         "clear-prefix should preserve diagnostics outside the deleted subtree");
}

}  // namespace

void RegisterDiagnosticsStoreTests(std::vector<TestCase>& tests) {
  AddTest(tests, "DiagnosticsStore/MergesOwnersPerFile", TestDiagnosticsStoreMergesOwnersPerFile);
  AddTest(tests, "DiagnosticsStore/ClearsOwnersIndependently",
          TestDiagnosticsStoreClearsOwnersIndependently);
  AddTest(tests, "DiagnosticsStore/SnapshotAllSortsAcrossFiles",
          TestDiagnosticsStoreSnapshotAllSortsAcrossFiles);
  AddTest(tests, "DiagnosticsStore/RetargetsPathPrefixes",
          TestDiagnosticsStoreRetargetsPathPrefixes);
  AddTest(tests, "DiagnosticsStore/ClearsPathPrefixes",
          TestDiagnosticsStoreClearsPathPrefixes);
}

}  // namespace microide::tests
