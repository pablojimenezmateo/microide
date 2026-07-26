#include "TestSupport.h"

#include "workspace/WorkspaceVirtualDocument.h"

#include <string>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::VirtualDocumentRegistry;
using microide::workspace::VirtualDocumentSpec;

void TestVirtualDocumentRegistryUpdatesAndCallbacks() {
  VirtualDocumentRegistry registry;
  std::string changed_uri;
  registry.SetOnChange([&](const std::string& uri) { changed_uri = uri; });
  registry.Register(VirtualDocumentSpec{
      .uri = "virtual://preview/readme",
      .language_id = "markdown",
      .content = "alpha\n",
      .editable = false,
      .plugin_id = "phase4-tests",
  });

  registry.UpdateContent("virtual://preview/readme", "beta\n");
  const auto* document = registry.GetDocument("virtual://preview/readme");
  Expect(document != nullptr && document->content == "beta\n",
         "virtual document registry should update stored content");
  Expect(changed_uri == "virtual://preview/readme",
         "virtual document registry should invoke the change callback with the updated URI");
}

}  // namespace

void RegisterPhase4Tests(std::vector<TestCase>& tests) {
  tests.emplace_back("Phase4.VirtualDocumentRegistryUpdatesAndCallbacks",
                     &TestVirtualDocumentRegistryUpdatesAndCallbacks);
}

}  // namespace microide::tests
