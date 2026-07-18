#include "TestSupport.h"

#include "editor/DiagnosticsStore.h"

#include <cstdint>
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

void TestDiagnosticsStoreTracksSeverityCountsAndRevision() {
  DiagnosticsStore store;
  const std::filesystem::path path = "/tmp/project/main.cpp";

  Expect(store.revision() == 0, "revision should start at zero");
  Expect(store.ErrorCount() == 0 && store.WarningCount() == 0 && store.InfoCount() == 0 &&
             store.HintCount() == 0,
         "severity counters should start at zero");

  Expect(store.ReplaceForOwnerFile(
             "lint", path,
             {MakeDiagnostic(0, 0, 0, 1, DiagnosticSeverity::Error, "error"),
              MakeDiagnostic(1, 0, 1, 1, DiagnosticSeverity::Warning, "warning"),
              MakeDiagnostic(2, 0, 2, 1, DiagnosticSeverity::Info, "info"),
              MakeDiagnostic(3, 0, 3, 1, DiagnosticSeverity::Hint, "hint")}),
         "first diagnostics publish should change store state");
  const std::uint64_t first_revision = store.revision();
  Expect(first_revision > 0, "revision should advance after first publish");
  Expect(store.ErrorCount() == 1 && store.WarningCount() == 1 && store.InfoCount() == 1 &&
             store.HintCount() == 1,
         "severity counters should track merged diagnostics");

  Expect(!store.ReplaceForOwnerFile(
             "lint", path,
             {MakeDiagnostic(0, 0, 0, 1, DiagnosticSeverity::Error, "error"),
              MakeDiagnostic(1, 0, 1, 1, DiagnosticSeverity::Warning, "warning"),
              MakeDiagnostic(2, 0, 2, 1, DiagnosticSeverity::Info, "info"),
              MakeDiagnostic(3, 0, 3, 1, DiagnosticSeverity::Hint, "hint")}),
         "publishing identical diagnostics should be a no-op");
  Expect(store.revision() == first_revision,
         "revision should not advance for no-op updates");

  Expect(store.ClearOwnerFile("lint", path),
         "clearing owner diagnostics should change store state");
  Expect(store.ErrorCount() == 0 && store.WarningCount() == 0 && store.InfoCount() == 0 &&
             store.HintCount() == 0,
         "severity counters should return to zero after clear");
  const std::uint64_t second_revision = store.revision();
  Expect(second_revision > first_revision, "revision should advance on clear");

  store.Clear();
  Expect(store.revision() == second_revision,
         "clearing an already-empty store should be a no-op");
}

void TestDiagnosticsSeverityFilter() {
  using microide::editor::DiagnosticSeverity;
  using microide::editor::FilterDiagnosticsAtLeastSeverity;
  using microide::editor::ParseDiagnosticSeverity;
  using microide::editor::PublishedDiagnostic;

  Expect(ParseDiagnosticSeverity("error") == DiagnosticSeverity::Error, "parse error");
  Expect(ParseDiagnosticSeverity("warning") == DiagnosticSeverity::Warning, "parse warning");
  Expect(ParseDiagnosticSeverity("hint") == DiagnosticSeverity::Hint, "parse hint");
  Expect(ParseDiagnosticSeverity("nonsense") == DiagnosticSeverity::Hint, "unknown -> hint");

  std::vector<PublishedDiagnostic> all;
  for (DiagnosticSeverity sev : {DiagnosticSeverity::Error, DiagnosticSeverity::Warning,
                                 DiagnosticSeverity::Info, DiagnosticSeverity::Hint}) {
    PublishedDiagnostic d;
    d.severity = sev;
    all.push_back(d);
  }
  std::vector<PublishedDiagnostic> scratch;

  // Hint (show-all) returns the SAME buffer (no copy).
  const auto shown_all = FilterDiagnosticsAtLeastSeverity(all, DiagnosticSeverity::Hint, scratch);
  Expect(shown_all.data() == all.data() && shown_all.size() == 4,
         "min=hint returns the input span unchanged");

  // min=warning keeps Error + Warning only.
  const auto warn = FilterDiagnosticsAtLeastSeverity(all, DiagnosticSeverity::Warning, scratch);
  Expect(warn.size() == 2, "min=warning keeps error+warning");
  Expect(warn[0].severity == DiagnosticSeverity::Error &&
             warn[1].severity == DiagnosticSeverity::Warning,
         "min=warning suppresses info+hint");

  // min=error keeps only errors.
  const auto err = FilterDiagnosticsAtLeastSeverity(all, DiagnosticSeverity::Error, scratch);
  Expect(err.size() == 1 && err[0].severity == DiagnosticSeverity::Error,
         "min=error keeps only errors");
}

// TD-2026-07-17A-056: SelectContextDiagnostics selects diagnostics overlapping a
// code-action request range, capped so a densely-annotated line cannot materialize
// a huge context payload before the async request is queued.
void TestSelectContextDiagnosticsOverlapAndCap() {
  using microide::editor::PublishedDiagnostic;
  using microide::editor::SelectContextDiagnostics;

  const auto make = [](std::size_t sl, std::size_t sc, std::size_t el, std::size_t ec,
                       std::string m) {
    PublishedDiagnostic d;
    d.range = SelectionRange{TextPosition{sl, sc}, TextPosition{el, ec}};
    d.message = std::move(m);
    return d;
  };

  std::vector<PublishedDiagnostic> in;
  in.push_back(make(0, 0, 0, 5, "a"));  // line 0 cols [0,5)
  in.push_back(make(2, 0, 2, 3, "b"));  // line 2 — disjoint from a line-0 cursor
  in.push_back(make(0, 3, 0, 8, "c"));  // line 0 cols [3,8)

  // Empty selection (cursor) at line 0 col 4 overlaps "a" and "c" but not "b".
  const SelectionRange cursor{TextPosition{0, 4}, TextPosition{0, 4}};
  bool truncated = true;
  const auto sel = SelectContextDiagnostics(in, cursor, 100, &truncated);
  Expect(sel.size() == 2 && !truncated, "only overlapping diagnostics are selected, uncapped");
  Expect(sel[0].message == "a" && sel[1].message == "c",
         "overlapping diagnostics keep stored order");

  // 50 diagnostics all overlapping the cursor, capped at 8.
  std::vector<PublishedDiagnostic> many;
  for (int i = 0; i < 50; ++i) {
    many.push_back(make(0, 0, 0, 10, "d" + std::to_string(i)));
  }
  bool cap_truncated = false;
  const auto capped = SelectContextDiagnostics(many, cursor, 8, &cap_truncated);
  Expect(capped.size() == 8 && cap_truncated,
         "the context-diagnostic set is capped with the truncated flag set");
}

// A hostile/buggy language server can publish an unbounded number of
// diagnostics for one file; the per-file list is scanned per visible row per
// frame, so the store caps how many it retains to keep redraw bounded.
void TestDiagnosticsStoreCapsPerFileCount() {
  DiagnosticsStore store;
  const std::filesystem::path path = "/tmp/project/huge.cpp";

  std::vector<Diagnostic> flood;
  flood.reserve(25000);
  for (std::size_t i = 0; i < 25000; ++i) {
    flood.push_back(MakeDiagnostic(i, 0, i, 1, DiagnosticSeverity::Warning, "x"));
  }
  Expect(store.ReplaceForOwnerFile("flood", path, std::move(flood)),
         "a large diagnostic batch should still publish");

  const auto* merged = store.FindByPath(path);
  Expect(merged != nullptr, "the file should have diagnostics after a flood");
  Expect(merged->size() <= 10000,
         "stored diagnostics per file must be capped so per-row scans stay bounded");
}

// Truncation must be observable so the Problems sidebar/status can signal that a
// file's diagnostic list is incomplete.
void TestDiagnosticsStoreFlagsTruncatedFiles() {
  constexpr std::size_t kMax = 10000;
  DiagnosticsStore store;
  const std::filesystem::path capped = "/tmp/project/capped.cpp";
  const std::filesystem::path small = "/tmp/project/small.cpp";

  std::vector<Diagnostic> flood;
  flood.reserve(kMax + 1);
  for (std::size_t i = 0; i < kMax + 1; ++i) {
    flood.push_back(MakeDiagnostic(i, 0, i, 1, DiagnosticSeverity::Warning, "x"));
  }
  Expect(store.ReplaceForOwnerFile("flood", capped, std::move(flood)),
         "publishing kMax + 1 diagnostics should succeed");

  const auto* merged = store.FindByPath(capped);
  Expect(merged != nullptr, "the capped file should have diagnostics");
  Expect(merged->size() == kMax, "stored count must be capped to the maximum");
  Expect(store.IsPathTruncated(capped), "the capped file must be flagged truncated");
  Expect(store.HasTruncatedFile(), "the store must report at least one truncated file");

  // A file whose list fits under the cap is not flagged.
  std::vector<Diagnostic> few;
  few.push_back(MakeDiagnostic(0, 0, 0, 1, DiagnosticSeverity::Error, "e"));
  Expect(store.ReplaceForOwnerFile("lint", small, std::move(few)),
         "a small diagnostic batch should publish");
  Expect(!store.IsPathTruncated(small), "an uncapped file must not be flagged truncated");

  // Replacing the capped file with a small batch clears its truncated flag and
  // the store-wide signal.
  std::vector<Diagnostic> replacement;
  replacement.push_back(MakeDiagnostic(0, 0, 0, 1, DiagnosticSeverity::Warning, "w"));
  Expect(store.ReplaceForOwnerFile("flood", capped, std::move(replacement)),
         "shrinking the capped file should republish");
  Expect(!store.IsPathTruncated(capped),
         "clearing the flood must clear the truncated flag");
  Expect(!store.HasTruncatedFile(),
         "with no capped file remaining the store must report no truncation");
}

// TD-2026-07-17A-064: each owner is capped at 10000, but the merged multi-owner view
// had no aggregate cap, so several LSP/plugin owners could multiply the per-visible-row
// diagnostic scan/underline cost for one file. Publishing three fully-capped owners for
// the same file must produce a merged list bounded by the aggregate cap, flagged
// truncated.
void TestDiagnosticsStoreCapsMergedMultiOwnerCount() {
  DiagnosticsStore store;
  const std::filesystem::path path = "/tmp/project/multi.cpp";

  const auto publish_owner = [&](const char* owner) {
    std::vector<Diagnostic> flood;
    flood.reserve(10000);
    for (std::size_t i = 0; i < 10000; ++i) {
      flood.push_back(MakeDiagnostic(i, 0, i, 1, DiagnosticSeverity::Warning, "x"));
    }
    Expect(store.ReplaceForOwnerFile(owner, path, std::move(flood)),
           "each owner's capped diagnostic batch should publish");
  };
  publish_owner("lsp-a");
  publish_owner("lsp-b");
  publish_owner("plugin-c");  // 3 * 10000 = 30000 merged before the aggregate cap

  const auto* merged = store.FindByPath(path);
  Expect(merged != nullptr, "the merged file view should exist");
  Expect(merged->size() <= 20000,
         "the merged multi-owner list must be bounded by the aggregate per-file cap");
  Expect(store.IsPathTruncated(path),
         "an aggregate-capped merged view must be flagged truncated");
}

}  // namespace

void RegisterDiagnosticsStoreTests(std::vector<TestCase>& tests) {
  AddTest(tests, "DiagnosticsStore/CapsMergedMultiOwnerCount",
          TestDiagnosticsStoreCapsMergedMultiOwnerCount);
  AddTest(tests, "DiagnosticsStore/CapsPerFileCount", TestDiagnosticsStoreCapsPerFileCount);
  AddTest(tests, "DiagnosticsStore/FlagsTruncatedFiles",
          TestDiagnosticsStoreFlagsTruncatedFiles);
  AddTest(tests, "DiagnosticsStore/SeverityFilter", TestDiagnosticsSeverityFilter);
  AddTest(tests, "DiagnosticsStore/SelectContextDiagnosticsOverlapAndCap",
          TestSelectContextDiagnosticsOverlapAndCap);
  AddTest(tests, "DiagnosticsStore/MergesOwnersPerFile", TestDiagnosticsStoreMergesOwnersPerFile);
  AddTest(tests, "DiagnosticsStore/ClearsOwnersIndependently",
          TestDiagnosticsStoreClearsOwnersIndependently);
  AddTest(tests, "DiagnosticsStore/SnapshotAllSortsAcrossFiles",
          TestDiagnosticsStoreSnapshotAllSortsAcrossFiles);
  AddTest(tests, "DiagnosticsStore/RetargetsPathPrefixes",
          TestDiagnosticsStoreRetargetsPathPrefixes);
  AddTest(tests, "DiagnosticsStore/ClearsPathPrefixes",
          TestDiagnosticsStoreClearsPathPrefixes);
  AddTest(tests, "DiagnosticsStore/TracksSeverityCountsAndRevision",
          TestDiagnosticsStoreTracksSeverityCountsAndRevision);
}

}  // namespace microide::tests
