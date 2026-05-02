#include "TestSupport.h"

#include "workspace/WorkspaceReviewComments.h"
#include "workspace/WorkspaceSecretStorage.h"
#include "workspace/WorkspaceVirtualDocument.h"

#include <string>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::ReviewComment;
using microide::workspace::ReviewCommentState;
using microide::workspace::ReviewCommentsRegistry;
using microide::workspace::SecretStorage;
using microide::workspace::VirtualDocumentRegistry;
using microide::workspace::VirtualDocumentSpec;

void TestSecretStoragePersistsAcrossInstances() {
  TemporaryDirectory temp_dir;
  ScopedEnvVar xdg_config_home("XDG_CONFIG_HOME", (temp_dir.path() / "config").string());

  {
    SecretStorage storage;
    Expect(storage.Store("github.token", "secret-value"),
           "secret storage should persist stored values");
    Expect(storage.Contains("github.token"),
           "secret storage should report persisted keys as present");
  }

  {
    SecretStorage reloaded;
    const auto secret = reloaded.Retrieve("github.token");
    Expect(secret.has_value() && *secret == "secret-value",
           "secret storage should reload persisted values in a new instance");
    Expect(reloaded.Delete("github.token"),
           "secret storage should delete persisted keys");
  }

  {
    SecretStorage reloaded;
    Expect(!reloaded.Contains("github.token"),
           "deleting a persisted key should survive a later reload");
  }
}

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

void TestReviewCommentsRegistryTracksThreadsAndState() {
  ReviewCommentsRegistry registry;
  registry.AddComment(ReviewComment{
      .id = "comment-1",
      .uri = "virtual://review/readme",
      .line = 4,
      .author = "alice",
      .body = "Needs a follow-up",
      .state = ReviewCommentState::Pending,
  });
  registry.AddThread(microide::workspace::ReviewThread{
      .id = "thread-1",
      .uri = "virtual://review/readme",
      .line = 4,
      .comments = {},
      .state = ReviewCommentState::Pending,
  });

  registry.UpdateCommentState("comment-1", ReviewCommentState::Resolved);
  registry.UpdateThreadState("thread-1", ReviewCommentState::Active);

  const auto comments = registry.GetComments("virtual://review/readme", 4);
  Expect(comments.size() == 1 && comments.front().state == ReviewCommentState::Resolved,
         "review comment registry should update stored comment state");
  const auto threads = registry.GetThreads("virtual://review/readme");
  Expect(threads.size() == 1 && threads.front().state == ReviewCommentState::Active,
         "review comment registry should update stored thread state");
  Expect(registry.HasComments("virtual://review/readme", 4),
         "review comment registry should index comments by URI and line");
  Expect(registry.HasThreads("virtual://review/readme", 4),
         "review comment registry should index threads by URI and line");
  Expect(!registry.HasComments("virtual://review/readme", 5),
         "review comment registry line index should not match unrelated lines");

  registry.RemoveComment("comment-1");
  Expect(!registry.HasComments("virtual://review/readme", 4),
         "removing a comment should invalidate the line index");
  registry.RemoveThread("thread-1");
  Expect(!registry.HasThreads("virtual://review/readme", 4),
         "removing a thread should invalidate the line index");
}

}  // namespace

void RegisterPhase4Tests(std::vector<TestCase>& tests) {
  tests.emplace_back("Phase4.SecretStoragePersistsAcrossInstances",
                     &TestSecretStoragePersistsAcrossInstances);
  tests.emplace_back("Phase4.VirtualDocumentRegistryUpdatesAndCallbacks",
                     &TestVirtualDocumentRegistryUpdatesAndCallbacks);
  tests.emplace_back("Phase4.ReviewCommentsRegistryTracksThreadsAndState",
                     &TestReviewCommentsRegistryTracksThreadsAndState);
}

}  // namespace microide::tests
