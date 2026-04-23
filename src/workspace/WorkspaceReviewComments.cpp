#include "workspace/WorkspaceReviewComments.h"

#include <algorithm>

namespace microide::workspace {

ReviewCommentsRegistry::ReviewCommentsRegistry() = default;
ReviewCommentsRegistry::~ReviewCommentsRegistry() = default;

void ReviewCommentsRegistry::AddComment(const ReviewComment& comment) {
  InvalidateUri(comment.uri);
  comments_.push_back(comment);
}

void ReviewCommentsRegistry::AddThread(const ReviewThread& thread) {
  InvalidateUri(thread.uri);
  threads_.push_back(thread);
}

std::vector<ReviewComment> ReviewCommentsRegistry::GetComments(const std::string& uri,
                                                               int line) const {
  std::vector<ReviewComment> result;
  const UriIndex* index = IndexForUri(uri);
  if (index == nullptr) {
    return result;
  }
  const auto line_it = index->comment_indices_by_line.find(line);
  if (line_it == index->comment_indices_by_line.end()) {
    return result;
  }
  result.reserve(line_it->second.size());
  for (const std::size_t comment_index : line_it->second) {
    result.push_back(comments_[comment_index]);
  }
  return result;
}

std::vector<ReviewThread> ReviewCommentsRegistry::GetThreads(const std::string& uri) const {
  std::vector<ReviewThread> result;
  const UriIndex* index = IndexForUri(uri);
  if (index == nullptr) {
    return result;
  }
  result.reserve(index->thread_indices.size());
  for (const std::size_t thread_index : index->thread_indices) {
    result.push_back(threads_[thread_index]);
  }
  return result;
}

bool ReviewCommentsRegistry::HasComments(const std::string& uri, int line) const {
  const UriIndex* index = IndexForUri(uri);
  return index != nullptr && index->comment_indices_by_line.find(line) !=
                                 index->comment_indices_by_line.end();
}

bool ReviewCommentsRegistry::HasThreads(const std::string& uri, int line) const {
  const UriIndex* index = IndexForUri(uri);
  return index != nullptr &&
         index->thread_indices_by_line.find(line) != index->thread_indices_by_line.end();
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
  InvalidateAll();
  comments_.erase(std::remove_if(comments_.begin(), comments_.end(),
                                 [&](const ReviewComment& c) { return c.id == comment_id; }),
                  comments_.end());
}

void ReviewCommentsRegistry::RemoveThread(const std::string& thread_id) {
  InvalidateAll();
  threads_.erase(std::remove_if(threads_.begin(), threads_.end(),
                                [&](const ReviewThread& t) { return t.id == thread_id; }),
                 threads_.end());
}

void ReviewCommentsRegistry::Clear() {
  comments_.clear();
  threads_.clear();
  indices_by_uri_.clear();
}

const ReviewCommentsRegistry::UriIndex* ReviewCommentsRegistry::IndexForUri(
    const std::string& uri) const {
  UriIndex& index = indices_by_uri_[uri];
  if (!index.dirty) {
    return index.thread_indices.empty() && index.comment_indices_by_line.empty() ? nullptr
                                                                                : &index;
  }

  index.thread_indices.clear();
  index.comment_indices_by_line.clear();
  index.thread_indices_by_line.clear();

  for (std::size_t i = 0; i < comments_.size(); ++i) {
    const ReviewComment& comment = comments_[i];
    if (comment.uri == uri) {
      index.comment_indices_by_line[comment.line].push_back(i);
    }
  }
  for (std::size_t i = 0; i < threads_.size(); ++i) {
    const ReviewThread& thread = threads_[i];
    if (thread.uri == uri) {
      index.thread_indices.push_back(i);
      index.thread_indices_by_line[thread.line].push_back(i);
    }
  }

  index.dirty = false;
  return index.thread_indices.empty() && index.comment_indices_by_line.empty() ? nullptr
                                                                              : &index;
}

void ReviewCommentsRegistry::InvalidateUri(const std::string& uri) {
  auto it = indices_by_uri_.find(uri);
  if (it != indices_by_uri_.end()) {
    it->second.dirty = true;
  }
}

void ReviewCommentsRegistry::InvalidateAll() {
  for (auto& [uri, index] : indices_by_uri_) {
    (void)uri;
    index.dirty = true;
  }
}

}  // namespace microide::workspace
