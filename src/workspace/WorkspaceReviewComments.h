#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace microide::workspace {

// Review comment state.
enum class ReviewCommentState {
  Pending,
  Active,
  Resolved,
};

// Review comment: inline code review comment in diff view.
struct ReviewComment {
  std::string id;
  std::string uri;      // diff or virtual document URI
  int line = 0;         // line in diff
  std::string author;   // user name
  std::string body;     // comment text
  ReviewCommentState state = ReviewCommentState::Pending;
};

// Review thread: a discussion thread on a code location.
struct ReviewThread {
  std::string id;
  std::string uri;
  int line = 0;
  std::vector<ReviewComment> comments;
  ReviewCommentState state = ReviewCommentState::Pending;
};

// Review comments registry: manages code review comments and threads.
class ReviewCommentsRegistry {
  struct UriIndex;  // forward declaration; defined in the private section below

 public:
  ReviewCommentsRegistry();
  ~ReviewCommentsRegistry();

  // Add a comment.
  void AddComment(const ReviewComment& comment);

  // Add a thread.
  void AddThread(const ReviewThread& thread);

  // Get comments for a document line.
  std::vector<ReviewComment> GetComments(const std::string& uri, int line) const;

  // Get threads for a document.
  std::vector<ReviewThread> GetThreads(const std::string& uri) const;

  // Check whether a document line has review markers.
  bool HasComments(const std::string& uri, int line) const;
  bool HasThreads(const std::string& uri, int line) const;

  // Resolve a document's marker index once (a single URI hash), then test
  // individual lines without re-hashing the URI per row. The handle is false
  // when the document has no markers. Used by the per-row review-marker render
  // pass to avoid two full-string-hash lookups per visible line.
  class DocumentMarkers {
   public:
    // Constructed only via MarkersForUri: the parameter type is the registry's
    // private UriIndex, so external code cannot name it to call this directly.
    explicit DocumentMarkers(const UriIndex* index) : index_(index) {}
    bool HasMarkerAtLine(int line) const;
    explicit operator bool() const { return index_ != nullptr; }

   private:
    const UriIndex* index_ = nullptr;
  };
  DocumentMarkers MarkersForUri(const std::string& uri) const;

  // True when no comments or threads exist at all. Lets hot render paths skip
  // per-frame work (e.g. materializing a viewport's URI) in the common case.
  bool Empty() const { return comments_.empty() && threads_.empty(); }

  // Update comment state.
  void UpdateCommentState(const std::string& comment_id, ReviewCommentState state);

  // Update thread state.
  void UpdateThreadState(const std::string& thread_id, ReviewCommentState state);

  // Remove comment.
  void RemoveComment(const std::string& comment_id);

  // Remove thread.
  void RemoveThread(const std::string& thread_id);

  // Clear all.
  void Clear();

 private:
  struct UriIndex {
    std::vector<std::size_t> thread_indices;
    std::unordered_map<int, std::vector<std::size_t>> comment_indices_by_line;
    std::unordered_map<int, std::vector<std::size_t>> thread_indices_by_line;
  };

  // Non-inserting: returns nullptr for a URI with no markers WITHOUT growing the
  // cache (a render pass visiting many marker-free files must not accumulate empty
  // per-file records). Rebuilds the whole per-URI grouping in one pass when the
  // cache is dirty, so the first lookup after a mutation is O(comments+threads)
  // once for all URIs instead of a full scan per first-seen URI.
  const UriIndex* IndexForUri(const std::string& uri) const;
  void RebuildIndices() const;
  void InvalidateAll();

  std::vector<ReviewComment> comments_;
  std::vector<ReviewThread> threads_;
  // Only URIs that actually carry markers appear here; rebuilt wholesale when
  // indices_dirty_ is set (any comment/thread mutation).
  mutable std::unordered_map<std::string, UriIndex> indices_by_uri_;
  mutable bool indices_dirty_ = true;
};

}  // namespace microide::workspace
