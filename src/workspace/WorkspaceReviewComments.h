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
    bool dirty = true;
    std::vector<std::size_t> thread_indices;
    std::unordered_map<int, std::vector<std::size_t>> comment_indices_by_line;
    std::unordered_map<int, std::vector<std::size_t>> thread_indices_by_line;
  };

  const UriIndex* IndexForUri(const std::string& uri) const;
  void InvalidateUri(const std::string& uri);
  void InvalidateAll();

  std::vector<ReviewComment> comments_;
  std::vector<ReviewThread> threads_;
  mutable std::unordered_map<std::string, UriIndex> indices_by_uri_;
};

}  // namespace microide::workspace
