#include "TestSupport.h"

#include <filesystem>

#include "editor/TextViewport.h"
#include "workspace/AssistService.h"

namespace microide::tests {

using microide::editor::TextViewport;
using microide::workspace::AssistService;

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
}

}  // namespace microide::tests
