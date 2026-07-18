#include "TestSupport.h"

#include <filesystem>
#include <string>
#include <vector>

#include "editor/TextViewport.h"
#include "workspace/AssistProviderMerge.h"
#include "workspace/AssistService.h"

namespace microide::tests {

using microide::editor::TextViewport;
using microide::workspace::AssistService;
namespace assist_merge = microide::workspace::assist_merge;

namespace {

std::vector<std::string> RankLabels(const std::vector<std::string>& primary,
                                    const std::vector<std::string>& secondary) {
  return assist_merge::RankedUnion(primary, secondary,
                                   [](const std::string& item) { return item; });
}

}  // namespace

// Regression coverage for the stale-async-result guard: a plugin/LSP completion
// or code-action response that arrives after the active editable buffer has been
// switched (or closed) must be dropped so it cannot clobber a newer session or
// land across a file/project switch. AssistService::ResultIsStale is the pure
// decision the async callbacks consult before mutating the overlay session.
void RegisterAssistServiceTests(std::vector<TestCase>& tests) {
  tests.push_back({"AssistService/ResultIsStaleDropsClosedBuffer", [] {
                     Expect(AssistService::ResultIsStale(nullptr, "/proj/a.cpp"),
                            "a null active viewport (buffer closed) must be treated as stale");
                   }});

  tests.push_back({"AssistService/ResultIsStaleDropsSwitchedPath", [] {
                     TextViewport other;
                     other.LoadContent("other\n", "/proj/b.cpp");
                     Expect(AssistService::ResultIsStale(&other, "/proj/a.cpp"),
                            "a response for a.cpp must be stale once b.cpp is the active buffer");
                   }});

  tests.push_back({"AssistService/ResultIsFreshForMatchingPath", [] {
                     TextViewport same;
                     same.LoadContent("same\n", "/proj/a.cpp");
                     Expect(!AssistService::ResultIsStale(&same, "/proj/a.cpp"),
                            "a response for the still-active buffer path must be applied");
                   }});

  // The concurrent provider merge: RankedUnion keeps the authoritative (primary)
  // source first, drops later duplicates, and preserves per-source order.
  tests.push_back({"AssistService/RankedUnionPrimaryFirstDedupes", [] {
                     const std::vector<std::string> merged =
                         RankLabels({"lsp_a", "shared"}, {"shared", "plugin_b"});
                     Expect(merged.size() == 3,
                            "the union of two sources should drop the duplicate key");
                     Expect(merged[0] == "lsp_a" && merged[1] == "shared" &&
                                merged[2] == "plugin_b",
                            "primary items rank first and win the shared-key tie");
                   }});

  tests.push_back({"AssistService/RankedUnionOneSourceEmpty", [] {
                     Expect(RankLabels({}, {"only_plugin"}) ==
                                std::vector<std::string>{"only_plugin"},
                            "an empty primary yields the secondary source verbatim");
                     Expect(RankLabels({"only_lsp"}, {}) ==
                                std::vector<std::string>{"only_lsp"},
                            "an empty secondary yields the primary source verbatim");
                   }});

  tests.push_back({"AssistService/RankedUnionBothEmpty", [] {
                     Expect(RankLabels({}, {}).empty(),
                            "the union of two empty sources is empty");
                   }});

  // Above the hash-set threshold (combined size >= 128) the de-dup switches from a
  // linear seen-scan to a hash set; the ranked-append semantics (primary first,
  // drop later duplicates, preserve per-source order) must be identical.
  tests.push_back({"AssistService/RankedUnionLargeListsDedupeIdentically", [] {
                     std::vector<std::string> primary;
                     std::vector<std::string> secondary;
                     for (int i = 0; i < 200; ++i) {
                       primary.push_back("p_" + std::to_string(i));
                     }
                     // Secondary repeats every primary key (all dropped) plus 200 fresh
                     // keys and internal duplicates (only the first survives).
                     for (int i = 0; i < 200; ++i) {
                       secondary.push_back("p_" + std::to_string(i));  // dup of primary
                     }
                     for (int i = 0; i < 200; ++i) {
                       secondary.push_back("s_" + std::to_string(i));
                       secondary.push_back("s_" + std::to_string(i));  // internal dup
                     }
                     const std::vector<std::string> merged = RankLabels(primary, secondary);
                     Expect(merged.size() == 400, "200 primary + 200 unique secondary keys survive");
                     for (int i = 0; i < 200; ++i) {
                       Expect(merged[static_cast<std::size_t>(i)] == "p_" + std::to_string(i),
                              "primary items keep their order and rank first");
                       Expect(merged[static_cast<std::size_t>(200 + i)] == "s_" + std::to_string(i),
                              "unique secondary items follow in first-seen order");
                     }
                   }});

  // TD-2026-07-17A-057: the code-action overlay materializes every action's inline
  // WorkspaceEdit under a SHARED aggregate byte/edit budget. A server returning many
  // large-but-capped fixes cannot make the overlay hold the sum of all edit
  // payloads — past the budget an action's inline fix is dropped (edits_truncated).
  tests.push_back({"AssistService/CodeActionEditsShareAggregateBudget", [] {
                     using microide::workspace::LspClient;
                     const auto make_action = [](std::string title, std::size_t text_bytes) {
                       LspClient::CodeAction action;
                       action.title = std::move(title);
                       action.has_edit = true;
                       LspClient::Range range{};
                       action.edit.changes["file:///tmp/a.cpp"] = {
                           {range, std::string(text_bytes, 'x')}};
                       return action;
                     };
                     // 16 MiB budget: two ~10 MiB edits — the first fits, the second
                     // overflows the aggregate and is dropped.
                     const std::size_t big = 10u * 1024 * 1024;
                     std::vector<LspClient::CodeAction> actions;
                     actions.push_back(make_action("first", big));
                     actions.push_back(make_action("second", big));

                     const auto items = AssistService::TransformLspCodeActions(actions);
                     Expect(items.size() == 2, "both action rows are still present (metadata kept)");
                     Expect(items[0].title == "first" && !items[0].edits.empty() &&
                                !items[0].edits_truncated,
                            "the first action's inline edit fits within the budget");
                     Expect(items[1].title == "second" && items[1].edits.empty() &&
                                items[1].edits_truncated,
                            "the second action's inline edit is dropped (over aggregate budget)");
                   }});

  // ChooseNavigation: single-result nav prefers the authoritative source, waits
  // for it while pending, and only consults the other once it resolves empty.
  using assist_merge::ChooseNavigation;
  using assist_merge::NavChoice;
  tests.push_back({"AssistService/NavWaitsForAuthoritativeLsp", [] {
                     // LSP authoritative + still pending: even a ready plugin hit
                     // must wait (a slower LSP answer wins for its language).
                     Expect(ChooseNavigation(/*lsp_authoritative=*/true, /*lsp_pending=*/true,
                                             /*lsp_has=*/false, /*plugin_pending=*/false,
                                             /*plugin_has=*/true) == NavChoice::Pending,
                            "navigation must wait for a pending authoritative LSP source");
                   }});

  tests.push_back({"AssistService/NavLspWinsWhenResolvedWithResult", [] {
                     Expect(ChooseNavigation(true, false, true, false, true) == NavChoice::UseLsp,
                            "a resolved authoritative LSP result wins the tie over the plugin");
                   }});

  tests.push_back({"AssistService/NavFallsToPluginWhenLspEmpty", [] {
                     Expect(ChooseNavigation(true, false, false, false, true) == NavChoice::UsePlugin,
                            "an empty authoritative LSP defers to a resolved plugin result");
                     // LSP empty but plugin still pending: wait for the plugin.
                     Expect(ChooseNavigation(true, false, false, true, false) == NavChoice::Pending,
                            "an empty LSP still waits on a pending plugin source");
                   }});

  tests.push_back({"AssistService/NavNoneWhenBothEmpty", [] {
                     Expect(ChooseNavigation(true, false, false, false, false) == NavChoice::None,
                            "both sources resolved empty yields no navigation target");
                   }});

  tests.push_back({"AssistService/NavPluginPreferredWithoutServer", [] {
                     // No server (plugin authoritative): use the plugin result and
                     // do not wait on the non-authoritative LSP source.
                     Expect(ChooseNavigation(/*lsp_authoritative=*/false, /*lsp_pending=*/true,
                                             /*lsp_has=*/false, /*plugin_pending=*/false,
                                             /*plugin_has=*/true) == NavChoice::UsePlugin,
                            "with no server the plugin result is used immediately");
                   }});
}

}  // namespace microide::tests
