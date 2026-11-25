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

void TestDebugValueTreeAggregateNodeBudget() {
  workspace::DebugValueTree tree;
  constexpr int kScopeRef = 100;
  tree.AddRoot("Locals", "", "", kScopeRef, true);
  tree.Rebuild();
  tree.ToggleRow(0);

  std::vector<workspace::dap_protocol::DapVariable> variables;
  const std::size_t requested = workspace::DebugValueTree::kMaxLoadedNodes + 500;
  variables.reserve(requested);
  for (std::size_t i = 0; i < requested; ++i) {
    workspace::dap_protocol::DapVariable variable;
    variable.name = "v";
    variable.value = "0";
    variables.push_back(std::move(variable));
  }
  tree.ApplyVariables(kScopeRef, variables, 0);

  Expect(tree.Truncated(), "exceeding kMaxLoadedNodes must set the truncated flag");
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
  tree.AddRoot("Locals", "", "", kScopeRef, true);
  tree.Rebuild();
  tree.ToggleRow(0);
  std::vector<workspace::dap_protocol::DapVariable> variables;
  for (int i = 0; i < 8; ++i) {
    workspace::dap_protocol::DapVariable variable;
    variable.name = "x" + std::to_string(i);
    variable.value = "0";
    variables.push_back(std::move(variable));
  }
  tree.ApplyVariables(kScopeRef, variables, 0);
  Expect(!tree.Truncated(), "an in-budget container must not be flagged truncated");
  Expect(tree.Rows().size() == 9, "root + 8 children, no terminal truncated row");
}

void TestDebugValueTreeBudgetCountsLiveNodesNotErasedSlots() {
  workspace::DebugValueTree tree;
  constexpr int kContainerRef = 42;
  constexpr std::size_t kPage = 200;
  tree.AddRoot("obj", "{...}", "T", kContainerRef, false,
               static_cast<int>(kPage), true);
  tree.Rebuild();
  tree.ToggleRow(0);

  std::vector<workspace::dap_protocol::DapVariable> variables;
  variables.reserve(kPage);
  for (std::size_t i = 0; i < kPage; ++i) {
    variables.push_back(workspace::dap_protocol::DapVariable{.name = "v", .value = "0"});
  }
  const std::size_t rounds = (workspace::DebugValueTree::kMaxLoadedNodes / kPage) + 20;
  for (std::size_t round = 0; round < rounds; ++round) {
    tree.ApplyVariables(kContainerRef, variables, 0);
  }
  Expect(!tree.Truncated(),
         "replacing a small container's page must not spend the node budget on tombstones");
  Expect(tree.Rows().size() == kPage + 1, "root + one live page after every replacement");
}

void TestDebugValueTreeStaleNodeIdDoesNotAliasAfterClear() {
  workspace::DebugValueTree tree;
  const std::uint32_t stale_root = tree.AddRoot("old", "1", "int", 0, false);
  tree.Rebuild();
  Expect(tree.Rows().size() == 1, "the first stop has one root row");

  tree.Clear();
  const std::uint32_t fresh_root = tree.AddRoot("new", "2", "int", 0, false);
  tree.Rebuild();
  Expect(fresh_root > stale_root, "node ids stay globally monotonic across Clear");

  tree.SetNodeValue(stale_root, "999", "int", 0);
  Expect(tree.Rows().size() == 1 && tree.Rows()[0].display_value == "2",
         "a stale node id must resolve to nothing, not to the new root's slot");

  tree.SetNodeValue(fresh_root + 5000, "999", "int", 0);
  Expect(tree.Rows()[0].display_value == "2", "an out-of-range node id must be inert");
}

void TestDebugValueTreeErasedNodeIdStaysDead() {
  workspace::DebugValueTree tree;
  constexpr int kContainerRef = 9;
  const std::uint32_t root_id = tree.AddRoot("obj", "{...}", "T", kContainerRef,
                                             false);
  tree.Rebuild();
  tree.ToggleRow(0);
  constexpr int kChildRef = 77;
  tree.ApplyVariables(
      kContainerRef,
      {workspace::dap_protocol::DapVariable{
           .name = "a", .value = "1", .variables_reference = kChildRef},
       workspace::dap_protocol::DapVariable{.name = "b", .value = "2"}},
      0);
  Expect(tree.Rows().size() == 3, "root + two children");
  const std::uint32_t erased_child = tree.Rows()[1].node_id;

  tree.ApplyVariables(kContainerRef,
                      {workspace::dap_protocol::DapVariable{.name = "c", .value = "3"}},
                      0);
  Expect(tree.Rows().size() == 2, "root + the one replacement child");
  Expect(tree.Rows()[1].node_id != erased_child, "the replacement child gets a fresh id");

  tree.SetNodeValue(erased_child, "999", "int", 0);
  Expect(tree.Rows()[1].display_value == "3", "an erased node id must not resolve");

  tree.ApplyVariables(kChildRef,
                      {workspace::dap_protocol::DapVariable{.name = "ghost", .value = "0"}},
                      0);
  Expect(tree.Rows().size() == 2, "a response for an erased node's reference is dropped");

  tree.SetNodeValue(root_id, "{updated}", "T", kContainerRef);
  Expect(tree.Rows()[0].display_value == "{updated}",
         "erasing children must leave the surviving nodes addressable");
}

void TestExcludeGlobsRuleCountIsCapped() {
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
  std::string text(4u * 1024 * 1024, 'a');
  bool truncated = false;
  const std::vector<std::string> globs = ParseExcludeGlobs(text, &truncated);
  Expect(truncated, "scanning past the byte budget must set the truncated flag");
  std::size_t total = 0;
  for (const std::string& glob : globs) {
    total += glob.size();
  }
  Expect(total <= workspace::kMaxExcludeGlobsBytes,
         "retained exclude text must never exceed the byte budget");
}

void TestProcessAllowlistEntryByteBudget() {
  namespace ll = plugin::lifecycle_load_interop;
  Expect(ll::ProcessAllowlistEntryAccepted("eslint", 0),
         "a normal allowlist entry must be accepted");
  const std::string at_cap(ll::kMaxProcessAllowItemBytes, 'a');
  const std::string over_cap(ll::kMaxProcessAllowItemBytes + 1, 'a');
  Expect(ll::ProcessAllowlistEntryAccepted(at_cap, 0),
         "an entry at the per-item byte cap must be accepted");
  Expect(!ll::ProcessAllowlistEntryAccepted(over_cap, 0),
         "an entry over the per-item byte cap must be rejected");
  Expect(!ll::ProcessAllowlistEntryAccepted(std::string_view("tru\0e", 5), 0),
         "an entry with an embedded NUL must be rejected, not truncated");
  Expect(!ll::ProcessAllowlistEntryAccepted(
             "x", ll::kMaxProcessAllowAggregateBytes),
         "an entry that would exceed the aggregate byte budget must be rejected");
  Expect(ll::ProcessAllowlistEntryAccepted(
             "x", ll::kMaxProcessAllowAggregateBytes - 1),
         "an entry that exactly fits the remaining aggregate budget is accepted");
}

void TestClipboardExportClampBoundedAndUtf8Safe() {
  const workspace::ClipboardExportResult small =
      workspace::ClampClipboardExport("hello", 64);
  Expect(small.text == "hello" && !small.truncated,
         "an in-budget export is returned verbatim");

  std::string wide;
  for (int i = 0; i < 8; ++i) {
    wide += "\xC3\xA9";
  }
  const workspace::ClipboardExportResult clamped =
      workspace::ClampClipboardExport(wide, 5);
  Expect(clamped.truncated, "an over-budget export must be flagged truncated");
  Expect(clamped.text.rfind("\xC3\xA9\xC3\xA9", 0) == 0,
         "truncation backs off to a UTF-8 codepoint boundary");
  Expect(clamped.text.find("truncated") != std::string::npos,
         "a truncated export carries a marker");
  Expect(clamped.text.size() < wide.size() + 32,
         "the clamped payload stays near the budget, not the full input");
}

void TestBufferSearchMatchesAreCapped() {
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
  Expect(channels.Entries("debug.console." + std::to_string(total - 1)) != nullptr,
         "the most recently touched channel must survive eviction");
  Expect(channels.Entries("debug.console.0") == nullptr,
         "the least recently touched channel must be evicted");
}

void TestOutputChannelBudgetCountsParsedSnippetBytes() {
  workspace::WorkspaceOutputChannels channels;
  const std::string id = "build";
  const std::string ref = "src/main.cpp:10:5";
  const std::string code(1000, 'x');
  const std::string snippet = "   10 | " + code;
  channels.AppendLine(id, id, ref);
  channels.AppendLine(id, id, snippet);

  const auto* entries = channels.Entries(id);
  Expect(entries != nullptr && entries->size() == 2, "both lines are retained");

  const std::size_t raw = ref.size() + snippet.size();
  const std::size_t retained = channels.RetainedBytes(id);
  Expect(retained == raw + 8 + code.size(),
         "retained bytes include the parsed snippet's duplicated prefix+code");
  Expect(retained > raw, "parsed-entry owned bytes are charged to the channel budget");
}

void TestOutputEntrySerialsFollowTheirEntriesAndAreNeverReused() {
  workspace::WorkspaceOutputChannels channels;
  const std::string id = "log";
  for (int i = 0; i < 3; ++i) {
    channels.AppendLine(id, id, "line " + std::to_string(i));
  }
  channels.AppendLine("other", "other", "elsewhere");
  const std::uint64_t first = channels.FirstEntrySerial(id);
  channels.AppendLine(id, id, "line 3");
  Expect(channels.FirstEntrySerial(id) == first, "an append keeps the existing entries' serials");
  const std::size_t before = channels.Entries(id)->size();
  std::size_t appended = 0;
  while (channels.Entries(id)->size() >= before + appended) {
    channels.AppendLine(id, id, "x");
    ++appended;
  }
  const std::size_t dropped = before + appended - channels.Entries(id)->size();
  Expect(channels.FirstEntrySerial(id) == first + dropped, "a trim advances the base by the lines it dropped");
  const std::uint64_t last_issued = channels.FirstEntrySerial(id) + channels.Entries(id)->size() - 1;
  channels.Clear(id);
  channels.AppendLine(id, id, "after the clear");
  Expect(channels.FirstEntrySerial(id) > last_issued, "a cleared channel's new lines take serials never issued");
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

}

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
  AddTest(tests, "BoundedResourceCaps/OutputEntrySerialsFollowTheirEntriesAndAreNeverReused",
          TestOutputEntrySerialsFollowTheirEntriesAndAreNeverReused);
}

}
