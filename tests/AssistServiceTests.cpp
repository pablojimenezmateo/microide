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
