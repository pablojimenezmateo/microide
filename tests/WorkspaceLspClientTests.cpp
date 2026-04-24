#include "TestSupport.h"

#include "workspace/WorkspaceLspClient.h"

#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::LspClient;

void TestWorkspaceLspClientShutdownDoesNotRaceInitialization() {
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif

  for (int iteration = 0; iteration < 200; ++iteration) {
    LspClient client;
    const bool started =
        client.Start({"/bin/sh", "-c", "sleep 0.01"}, "file:///tmp", "sh");
    Expect(started, "lsp lifecycle stress fixture should start");
    client.Shutdown();
  }
}

}  // namespace

void RegisterWorkspaceLspClientTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceLspClient/ShutdownDoesNotRaceInitialization",
          TestWorkspaceLspClientShutdownDoesNotRaceInitialization);
}

}  // namespace microide::tests
