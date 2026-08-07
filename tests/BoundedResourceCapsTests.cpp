// Regression coverage for the TD-2026-07-17A focus-pass-2 "bounded resources"
// cluster: helpers that gained per-item caps + truncation flags so hostile or
// accidentally huge inputs cannot grow host memory / per-frame work without bound.

#include "TestSupport.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "plugin/PluginLifecycleLoadInterop.h"
#include "workspace/debug/DapProtocol.h"
#include "workspace/debug/DebugValueTree.h"
#include "workspace/SettingFlags.h"
#include "workspace/actions/WorkspaceActionServices.h"
#include "workspace/WorkspaceOutputChannels.h"
#include "workspace/WorkspaceTextSearch.h"

namespace microide::tests {
namespace {

using workspace::kMaxExcludeGlobsRules;
using workspace::ParseExcludeGlobs;

// TD-2026-07-17A-040: DebugValueTree bounds each child fetch (kChildPageSize) but had
// no aggregate loaded-node ceiling, so a huge (or hostile/garbage) container paged in
// over repeated "show more" clicks could grow the tree without bound. A single
// enormous page must now stop attaching at kMaxLoadedNodes and surface a terminal
// truncated row.
void TestDebugValueTreeAggregateNodeBudget() {
  workspace::DebugValueTree tree;
  constexpr int kScopeRef = 100;
  tree.AddRoot("Locals", "", "", kScopeRef, /*is_scope=*/true);
  tree.Rebuild();
  tree.ToggleRow(0);  // expand the scope so its children flatten into rows

  std::vector<workspace::dap_protocol::DapVariable> variables;
  const std::size_t requested = workspace::DebugValueTree::kMaxLoadedNodes + 500;
  variables.reserve(requested);
  for (std::size_t i = 0; i < requested; ++i) {
    workspace::dap_protocol::DapVariable variable;
    variable.name = "v";  // uniqueness is irrelevant to the node-count budget
    variable.value = "0";
    variables.push_back(std::move(variable));
  }
  tree.ApplyVariables(kScopeRef, variables, /*start=*/0);

  Expect(tree.Truncated(), "exceeding kMaxLoadedNodes must set the truncated flag");
  // root + attached children (< requested) + one terminal truncated row.
  Expect(tree.Rows().size() <= workspace::DebugValueTree::kMaxLoadedNodes + 2,
         "the flattened row list must stay within the aggregate node budget");
  Expect(tree.Rows().size() < requested,
         "the tree must have dropped children rather than attaching them all");
  const workspace::DebugVariableRowView& last = tree.Rows().back();
  Expect(last.is_placeholder && last.kind == workspace::DebugValueKind::Error,
         "a terminal, non-selectable truncated row must mark the dropped values");
}

void TestDebugValueTreeSmallContainerIsNotTruncated() {
  workspace::DebugValueTree tree;
  constexpr int kScopeRef = 7;
  tree.AddRoot("Locals", "", "", kScopeRef, /*is_scope=*/true);
  tree.Rebuild();
  tree.ToggleRow(0);
  std::vector<workspace::dap_protocol::DapVariable> variables;
  for (int i = 0; i < 8; ++i) {
    workspace::dap_protocol::DapVariable variable;
    variable.name = "x" + std::to_string(i);
    variable.value = "0";
    variables.push_back(std::move(variable));
  }
  tree.ApplyVariables(kScopeRef, variables, /*start=*/0);
  Expect(!tree.Truncated(), "an in-budget container must not be flagged truncated");
  Expect(tree.Rows().size() == 9, "root + 8 children, no terminal truncated row");
}

// TD-2026-08-07-162: nodes_ is a vector indexed by node id, so an erase leaves a
// tombstone. The aggregate budget must therefore count LIVE nodes, not slots —
// counting slots would make a tree that repeatedly replaces one small container's
// first page report itself truncated while holding a handful of values. The slot
// array is still capped (kMaxNodeSlots), which is what this walks up to.
void TestDebugValueTreeBudgetCountsLiveNodesNotErasedSlots() {
  workspace::DebugValueTree tree;
  constexpr int kContainerRef = 42;
  constexpr std::size_t kPage = 200;
  // total_known so the container reads as fully loaded after one page — otherwise
  // a full-page response appends a "show more…" row and the row count shifts.
  tree.AddRoot("obj", "{...}", "T", kContainerRef, /*is_scope=*/false,
               /*total_count=*/static_cast<int>(kPage), /*total_known=*/true);
  tree.Rebuild();
  tree.ToggleRow(0);  // expand so children flatten into rows

  std::vector<workspace::dap_protocol::DapVariable> variables;
  variables.reserve(kPage);
  for (std::size_t i = 0; i < kPage; ++i) {
    variables.push_back(workspace::dap_protocol::DapVariable{.name = "v", .value = "0"});
  }
  // Replace the first page enough times to burn well past kMaxLoadedNodes in
  // slots while never holding more than kPage + 1 live nodes.
  const std::size_t rounds = (workspace::DebugValueTree::kMaxLoadedNodes / kPage) + 20;
  for (std::size_t round = 0; round < rounds; ++round) {
    tree.ApplyVariables(kContainerRef, variables, /*start=*/0);
  }
  Expect(!tree.Truncated(),
         "replacing a small container's page must not spend the node budget on tombstones");
  Expect(tree.Rows().size() == kPage + 1, "root + one live page after every replacement");
}

// The other half of the same change: a node id from a previous stop must not
// resolve onto a freshly created node. Ids stay monotonic and `nodes_` is
// re-based on Clear, so a late setVariable/evaluate reply lands on nothing.
void TestDebugValueTreeStaleNodeIdDoesNotAliasAfterClear() {
  workspace::DebugValueTree tree;
  const std::uint32_t stale_root = tree.AddRoot("old", "1", "int", 0, /*is_scope=*/false);
  tree.Rebuild();
  Expect(tree.Rows().size() == 1, "the first stop has one root row");

  tree.Clear();
  const std::uint32_t fresh_root = tree.AddRoot("new", "2", "int", 0, /*is_scope=*/false);
  tree.Rebuild();
  Expect(fresh_root > stale_root, "node ids stay globally monotonic across Clear");

  // A reply issued against the previous stop.
  tree.SetNodeValue(stale_root, "999", "int", 0);
  Expect(tree.Rows().size() == 1 && tree.Rows()[0].display_value == "2",
         "a stale node id must resolve to nothing, not to the new root's slot");

  // And an id past the end of the live range is equally inert.
  tree.SetNodeValue(fresh_root + 5000, "999", "int", 0);
  Expect(tree.Rows()[0].display_value == "2", "an out-of-range node id must be inert");
}

// An erased node's id must stay dead for the life of the tree, and erasing it
// must not disturb the ids of the nodes that survive.
void TestDebugValueTreeErasedNodeIdStaysDead() {
  workspace::DebugValueTree tree;
  constexpr int kContainerRef = 9;
  const std::uint32_t root_id = tree.AddRoot("obj", "{...}", "T", kContainerRef,
                                             /*is_scope=*/false);
  tree.Rebuild();
  tree.ToggleRow(0);
  constexpr int kChildRef = 77;
  tree.ApplyVariables(
      kContainerRef,
      {workspace::dap_protocol::DapVariable{
           .name = "a", .value = "1", .variables_reference = kChildRef},
       workspace::dap_protocol::DapVariable{.name = "b", .value = "2"}},
      /*start=*/0);
  Expect(tree.Rows().size() == 3, "root + two children");
  const std::uint32_t erased_child = tree.Rows()[1].node_id;

  // A fresh first page replaces (and erases) the previous children.
  tree.ApplyVariables(kContainerRef,
                      {workspace::dap_protocol::DapVariable{.name = "c", .value = "3"}},
                      /*start=*/0);
  Expect(tree.Rows().size() == 2, "root + the one replacement child");
  Expect(tree.Rows()[1].node_id != erased_child, "the replacement child gets a fresh id");

  tree.SetNodeValue(erased_child, "999", "int", 0);
  Expect(tree.Rows()[1].display_value == "3", "an erased node id must not resolve");

  // The erased child's container reference is unmapped too, so a late `variables`
  // response for it attaches nothing rather than grafting onto a dead node.
  tree.ApplyVariables(kChildRef,
                      {workspace::dap_protocol::DapVariable{.name = "ghost", .value = "0"}},
                      /*start=*/0);
  Expect(tree.Rows().size() == 2, "a response for an erased node's reference is dropped");

  // The surviving root is untouched by the erase.
  tree.SetNodeValue(root_id, "{updated}", "T", kContainerRef);
  Expect(tree.Rows()[0].display_value == "{updated}",
         "erasing children must leave the surviving nodes addressable");
}

// TD-2026-07-17A-106: `project.files_exclude` had no parsed-rule count or byte
// budget, so a persisted/pasted setting could create an unbounded rule vector that
// is then copied into DirectoryTree/FileIndex/the native watcher and scanned per
// traversal predicate.
void TestExcludeGlobsRuleCountIsCapped() {
  // Far more comma-separated rules than the cap; each is a distinct short glob.
  std::string text;
  const std::size_t requested = kMaxExcludeGlobsRules + 500;
  for (std::size_t i = 0; i < requested; ++i) {
    if (i != 0) {
      text.push_back(',');
    }
    text += "rule";
    text += std::to_string(i);
  }
  bool truncated = false;
  const std::vector<std::string> globs = ParseExcludeGlobs(text, &truncated);
  Expect(globs.size() == kMaxExcludeGlobsRules,
         "exclude globs must be capped at kMaxExcludeGlobsRules");
  Expect(truncated, "dropping over-cap exclude rules must set the truncated flag");
}

void TestExcludeGlobsByteBudgetIsCapped() {
  // A single enormous glob line (comment-free) far beyond the byte budget: the raw
  // text is only scanned up to the byte cap, so the whole thing cannot be retained.
  std::string text(4u * 1024 * 1024, 'a');  // 4 MiB single token, no separators
  bool truncated = false;
  const std::vector<std::string> globs = ParseExcludeGlobs(text, &truncated);
  Expect(truncated, "scanning past the byte budget must set the truncated flag");
  // The single surviving glob is bounded by the byte cap, never the full 4 MiB.
  std::size_t total = 0;
  for (const std::string& glob : globs) {
    total += glob.size();
  }
  Expect(total <= workspace::kMaxExcludeGlobsBytes,
         "retained exclude text must never exceed the byte budget");
}

// TD-2026-07-17A-121: capabilities.process.allow read each entry via the C-string
// lua_tostring — no per-item/aggregate byte cap and truncation at embedded NUL — so a
// manifest with a few enormous allowlist strings could inflate host RSS during load.
// The acceptance predicate now mirrors the shared string-array byte budgets.
void TestProcessAllowlistEntryByteBudget() {
  namespace ll = plugin::lifecycle_load_interop;
  // A normal executable name is accepted.
  Expect(ll::ProcessAllowlistEntryAccepted("eslint", 0),
         "a normal allowlist entry must be accepted");
  // An entry exactly at the per-item cap is accepted; one byte over is rejected.
  const std::string at_cap(ll::kMaxProcessAllowItemBytes, 'a');
  const std::string over_cap(ll::kMaxProcessAllowItemBytes + 1, 'a');
  Expect(ll::ProcessAllowlistEntryAccepted(at_cap, 0),
         "an entry at the per-item byte cap must be accepted");
  Expect(!ll::ProcessAllowlistEntryAccepted(over_cap, 0),
         "an entry over the per-item byte cap must be rejected");
  // Embedded NUL is rejected (would be silently truncated by the C-string path).
  Expect(!ll::ProcessAllowlistEntryAccepted(std::string_view("tru\0e", 5), 0),
         "an entry with an embedded NUL must be rejected, not truncated");
  // Aggregate budget: an otherwise-fine entry that would overflow the remaining
  // aggregate budget is rejected.
  Expect(!ll::ProcessAllowlistEntryAccepted(
             "x", ll::kMaxProcessAllowAggregateBytes),
         "an entry that would exceed the aggregate byte budget must be rejected");
  Expect(ll::ProcessAllowlistEntryAccepted(
             "x", ll::kMaxProcessAllowAggregateBytes - 1),
         "an entry that exactly fits the remaining aggregate budget is accepted");
}

// TD-2026-07-17A-119: editor copy/cut materialized unbounded selected text and duplicated
// it into both clipboards. The shared clamp bounds the export on a UTF-8 boundary with a
// marker; cut refuses over-budget rather than deleting data it could not capture.
void TestClipboardExportClampBoundedAndUtf8Safe() {
  // Under budget: passthrough, no marker, not truncated.
  const workspace::ClipboardExportResult small =
      workspace::ClampClipboardExport("hello", 64);
  Expect(small.text == "hello" && !small.truncated,
         "an in-budget export is returned verbatim");

  // Over budget with multi-byte 'é' (0xC3 0xA9): the cut must land on a codepoint
  // boundary — budget 5 sits mid-'é' at byte 4, so it backs off to byte 4.
  std::string wide;
  for (int i = 0; i < 8; ++i) {
    wide += "\xC3\xA9";  // 'é' — 16 bytes total
  }
  const workspace::ClipboardExportResult clamped =
      workspace::ClampClipboardExport(wide, 5);
  Expect(clamped.truncated, "an over-budget export must be flagged truncated");
  // 4 bytes of payload (two whole 'é') then the marker; never a split codepoint.
  Expect(clamped.text.rfind("\xC3\xA9\xC3\xA9", 0) == 0,
         "truncation backs off to a UTF-8 codepoint boundary");
  Expect(clamped.text.find("truncated") != std::string::npos,
         "a truncated export carries a marker");
  Expect(clamped.text.size() < wide.size() + 32,
         "the clamped payload stays near the budget, not the full input");
}

// TD-2026-07-17A-029: the stored buffer-search match set had no cap, so a one-character
// query in a large minified buffer could allocate millions of ranges and make each query
// update scale with match count. The retained set is now capped with a truncated flag;
// navigation is unaffected (it re-scans independently).
void TestBufferSearchMatchesAreCapped() {
  // One long line of 'a' with more single-char matches than the cap.
  std::vector<std::string> lines;
  lines.emplace_back(workspace::kMaxBufferSearchMatches + 250, 'a');
  bool truncated = false;
  const auto matches = workspace::FindLiteralSearchMatches(lines, "a", {}, &truncated);
  Expect(matches.size() == workspace::kMaxBufferSearchMatches,
         "the retained buffer-search match set must be capped");
  Expect(truncated, "hitting the match cap must set the truncated flag");
}

void TestBufferSearchSmallResultIsNotTruncated() {
  std::vector<std::string> lines = {"alpha beta alpha", "gamma alpha"};
  bool truncated = true;
  const auto matches = workspace::FindLiteralSearchMatches(lines, "alpha", {}, &truncated);
  Expect(matches.size() == 3, "all in-budget matches are returned");
  Expect(!truncated, "an in-budget result must not be flagged truncated");
}

// TD-2026-07-17A-116: each output channel is per-channel byte/entry capped, but the
// channel *set* was unbounded — repeated failing debug launches keep a unique
// debug.console.<id> channel and could accumulate many 16 MiB consoles. The live channel
// count is now capped with LRU eviction that preserves the most recently active channels.
void TestOutputChannelCountIsCappedWithLru() {
  workspace::WorkspaceOutputChannels channels;
  const std::size_t cap = workspace::WorkspaceOutputChannels::kMaxOutputChannels;
  const std::size_t total = cap + 10;
  for (std::size_t i = 0; i < total; ++i) {
    const std::string id = "debug.console." + std::to_string(i);
    channels.AppendLine(id, id, "output line");
  }
  Expect(channels.Channels().size() == cap,
         "the live output-channel set must be capped");
  Expect(channels.EvictedChannelCount() == total - cap,
         "the over-cap channels must have been evicted");
  // The most recently created channels survive; the oldest are evicted (LRU).
  Expect(channels.Entries("debug.console." + std::to_string(total - 1)) != nullptr,
         "the most recently touched channel must survive eviction");
  Expect(channels.Entries("debug.console.0") == nullptr,
         "the least recently touched channel must be evicted");
}

// TD-2026-07-17A-070: an output channel's retained-byte budget must include the
// parsed context-snippet entries' owned prefix/code text (separate heap copies of
// the line), not just the raw entry line — otherwise a stream of compiler-style
// context snippets exceeds the intended memory cap by the duplicated code text.
void TestOutputChannelBudgetCountsParsedSnippetBytes() {
  workspace::WorkspaceOutputChannels channels;
  const std::string id = "build";
  const std::string ref = "src/main.cpp:10:5";           // parsed as a ReferencePath
  const std::string code(1000, 'x');
  const std::string snippet = "   10 | " + code;         // parsed as a ContextSnippet
  channels.AppendLine(id, id, ref);
  channels.AppendLine(id, id, snippet);

  const auto* entries = channels.Entries(id);
  Expect(entries != nullptr && entries->size() == 2, "both lines are retained");

  // The snippet's parsed entry owns prefix ("   10 | ", 8 bytes) + code (1000
  // bytes) on top of the raw snippet line, so the budget must exceed the raw bytes.
  const std::size_t raw = ref.size() + snippet.size();
  const std::size_t retained = channels.RetainedBytes(id);
  Expect(retained == raw + 8 + code.size(),
         "retained bytes include the parsed snippet's duplicated prefix+code");
  Expect(retained > raw, "parsed-entry owned bytes are charged to the channel budget");
}

void TestExcludeGlobsNormalInputIsNotFlaggedTruncated() {
  bool truncated = true;
  const std::vector<std::string> globs =
      ParseExcludeGlobs("build/\n# comment\nnode_modules/, dist/", &truncated);
  Expect(globs.size() == 3, "normal exclude parsing keeps all non-comment entries");
  Expect(!truncated, "in-budget input must not be flagged truncated");
  Expect(globs[0] == "build/" && globs[1] == "node_modules/" && globs[2] == "dist/",
         "exclude parsing preserves entries and trims whitespace");
}

}  // namespace

void RegisterBoundedResourceCapsTests(std::vector<TestCase>& tests) {
  AddTest(tests, "BoundedResourceCaps/ExcludeGlobsRuleCountIsCapped",
          TestExcludeGlobsRuleCountIsCapped);
  AddTest(tests, "BoundedResourceCaps/ExcludeGlobsByteBudgetIsCapped",
          TestExcludeGlobsByteBudgetIsCapped);
  AddTest(tests, "BoundedResourceCaps/ExcludeGlobsNormalInputIsNotFlaggedTruncated",
          TestExcludeGlobsNormalInputIsNotFlaggedTruncated);
  AddTest(tests, "BoundedResourceCaps/OutputChannelBudgetCountsParsedSnippetBytes",
          TestOutputChannelBudgetCountsParsedSnippetBytes);
  AddTest(tests, "BoundedResourceCaps/DebugValueTreeAggregateNodeBudget",
          TestDebugValueTreeAggregateNodeBudget);
  AddTest(tests, "BoundedResourceCaps/DebugValueTreeSmallContainerIsNotTruncated",
          TestDebugValueTreeSmallContainerIsNotTruncated);
  AddTest(tests, "BoundedResourceCaps/DebugValueTreeBudgetCountsLiveNodesNotErasedSlots",
          TestDebugValueTreeBudgetCountsLiveNodesNotErasedSlots);
  AddTest(tests, "BoundedResourceCaps/DebugValueTreeStaleNodeIdDoesNotAliasAfterClear",
          TestDebugValueTreeStaleNodeIdDoesNotAliasAfterClear);
  AddTest(tests, "BoundedResourceCaps/DebugValueTreeErasedNodeIdStaysDead",
          TestDebugValueTreeErasedNodeIdStaysDead);
  AddTest(tests, "BoundedResourceCaps/ProcessAllowlistEntryByteBudget",
          TestProcessAllowlistEntryByteBudget);
  AddTest(tests, "BoundedResourceCaps/ClipboardExportClampBoundedAndUtf8Safe",
          TestClipboardExportClampBoundedAndUtf8Safe);
  AddTest(tests, "BoundedResourceCaps/BufferSearchMatchesAreCapped",
          TestBufferSearchMatchesAreCapped);
  AddTest(tests, "BoundedResourceCaps/BufferSearchSmallResultIsNotTruncated",
          TestBufferSearchSmallResultIsNotTruncated);
  AddTest(tests, "BoundedResourceCaps/OutputChannelCountIsCappedWithLru",
          TestOutputChannelCountIsCappedWithLru);
}

}  // namespace microide::tests
