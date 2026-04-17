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

}  // namespace

void RegisterDiagnosticsStoreTests(std::vector<TestCase>& tests) {
  AddTest(tests, "DiagnosticsStore/MergesOwnersPerFile", TestDiagnosticsStoreMergesOwnersPerFile);
  AddTest(tests, "DiagnosticsStore/ClearsOwnersIndependently",
          TestDiagnosticsStoreClearsOwnersIndependently);
}

}  // namespace microide::tests
