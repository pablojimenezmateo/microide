#include "workspace/WorkspaceReviewComments.h"

#include <algorithm>

namespace microide::workspace {

ReviewCommentsRegistry::ReviewCommentsRegistry() = default;
ReviewCommentsRegistry::~ReviewCommentsRegistry() = default;

void ReviewCommentsRegistry::AddComment(const ReviewComment& comment) {
  comments_.push_back(comment);
}

void ReviewCommentsRegistry::AddThread(const ReviewThread& thread) { threads_.push_back(thread); }

std::vector<ReviewComment> ReviewCommentsRegistry::GetComments(const std::string& uri,
                                                               int line) const {
  std::vector<ReviewComment> result;
  for (const auto& comment : comments_) {
    if (comment.uri == uri && comment.line == line) {
      result.push_back(comment);
    }
  }
  return result;
}

std::vector<ReviewThread> ReviewCommentsRegistry::GetThreads(const std::string& uri) const {
  std::vector<ReviewThread> result;
  for (const auto& thread : threads_) {
    if (thread.uri == uri) {
      result.push_back(thread);
    }
  }
  return result;
}

void ReviewCommentsRegistry::UpdateCommentState(const std::string& comment_id,
                                                ReviewCommentState state) {
  for (auto& comment : comments_) {
    if (comment.id == comment_id) {
      comment.state = state;
      return;
    }
  }
}

void ReviewCommentsRegistry::UpdateThreadState(const std::string& thread_id,
                                               ReviewCommentState state) {
  for (auto& thread : threads_) {
    if (thread.id == thread_id) {
      thread.state = state;
      return;
    }
  }
}

void ReviewCommentsRegistry::RemoveComment(const std::string& comment_id) {
  comments_.erase(std::remove_if(comments_.begin(), comments_.end(),
                                 [&](const ReviewComment& c) { return c.id == comment_id; }),
                  comments_.end());
}

void ReviewCommentsRegistry::RemoveThread(const std::string& thread_id) {
  threads_.erase(std::remove_if(threads_.begin(), threads_.end(),
                                [&](const ReviewThread& t) { return t.id == thread_id; }),
                 threads_.end());
}

void ReviewCommentsRegistry::Clear() {
  comments_.clear();
  threads_.clear();
}

}  // namespace microide::workspace
