#include "TestSupport.h"

#include "workspace/WorkspaceReviewComments.h"
#include "workspace/WorkspaceVirtualDocument.h"

#include <string>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::ReviewComment;
using microide::workspace::ReviewCommentState;
using microide::workspace::ReviewCommentsRegistry;
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

// A render pass visits many marker-free files. Looking those up (via MarkersForUri /
// HasComments) must resolve correctly AND stay non-inserting: the earlier design grew
// a persistent empty per-URI record for every marker-free file and rescanned all
// comments/threads on each first-seen URI. Here we verify markers resolve on the file
// that has them and are absent everywhere else, before and after a mutation.
void TestReviewCommentsRegistryMarkerFreeLookupsAreCorrect() {
  ReviewCommentsRegistry registry;
  registry.AddComment(ReviewComment{
      .id = "c1",
      .uri = "virtual://review/has_markers",
      .line = 10,
      .author = "bob",
      .body = "look here",
      .state = ReviewCommentState::Pending,
  });

  // Probe many distinct marker-free URIs (the render-pass shape).
  for (int i = 0; i < 500; ++i) {
    const std::string uri = "virtual://review/plain_" + std::to_string(i);
    Expect(!registry.HasComments(uri, 0), "a marker-free URI must report no comments");
    Expect(!static_cast<bool>(registry.MarkersForUri(uri)),
           "MarkersForUri on a marker-free URI is a falsey handle");
  }

  // The file that has a marker still resolves after all those misses.
  Expect(registry.HasComments("virtual://review/has_markers", 10),
         "the marker on its file survives unrelated marker-free lookups");
  Expect(static_cast<bool>(registry.MarkersForUri("virtual://review/has_markers")),
         "MarkersForUri returns a live handle for a file with markers");

  // A mutation (new marker on a previously marker-free file) rebuilds the index so the
  // new marker resolves and the old one is untouched.
  registry.AddThread(microide::workspace::ReviewThread{
      .id = "t1",
      .uri = "virtual://review/plain_3",
      .line = 2,
      .comments = {},
      .state = ReviewCommentState::Pending,
  });
  Expect(registry.HasThreads("virtual://review/plain_3", 2),
         "a newly added marker resolves after the mutation");
  Expect(registry.HasComments("virtual://review/has_markers", 10),
         "the pre-existing marker still resolves after the mutation");
  Expect(!registry.HasThreads("virtual://review/plain_4", 2),
         "a still-marker-free URI reports nothing");
}

}  // namespace

void RegisterPhase4Tests(std::vector<TestCase>& tests) {
  tests.emplace_back("Phase4.VirtualDocumentRegistryUpdatesAndCallbacks",
                     &TestVirtualDocumentRegistryUpdatesAndCallbacks);
  tests.emplace_back("Phase4.ReviewCommentsRegistryTracksThreadsAndState",
                     &TestReviewCommentsRegistryTracksThreadsAndState);
  tests.emplace_back("Phase4.ReviewCommentsRegistryMarkerFreeLookupsAreCorrect",
                     &TestReviewCommentsRegistryMarkerFreeLookupsAreCorrect);
}

}  // namespace microide::tests
