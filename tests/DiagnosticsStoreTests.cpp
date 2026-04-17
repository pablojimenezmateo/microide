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

}  // namespace

void RegisterDiagnosticsStoreTests(std::vector<TestCase>& tests) {
  AddTest(tests, "DiagnosticsStore/MergesOwnersPerFile", TestDiagnosticsStoreMergesOwnersPerFile);
  AddTest(tests, "DiagnosticsStore/ClearsOwnersIndependently",
          TestDiagnosticsStoreClearsOwnersIndependently);
  AddTest(tests, "DiagnosticsStore/SnapshotAllSortsAcrossFiles",
          TestDiagnosticsStoreSnapshotAllSortsAcrossFiles);
}

}  // namespace microide::tests
