#include "workspace/state/SaveContinuationState.h"

#include <algorithm>
#include <utility>

namespace microide::workspace {

std::uint64_t SaveContinuationQueue::Add(SaveContinuation continuation) {
  if (continuation.waiting_tab_ids.empty()) {
    return 0;
  }
  continuation.id = next_id_++;
  if (continuation.total_tab_count < continuation.waiting_tab_ids.size()) {
    continuation.total_tab_count = continuation.waiting_tab_ids.size();
  }
  const std::uint64_t id = continuation.id;
  continuations_.push_back(std::move(continuation));
  return id;
}

SaveContinuationQueue::Settled SaveContinuationQueue::Complete(std::uint64_t tab_id, bool saved) {
  Settled settled;
  for (auto it = continuations_.begin(); it != continuations_.end();) {
    auto& waiting = it->waiting_tab_ids;
    const auto match = std::find(waiting.begin(), waiting.end(), tab_id);
    if (match == waiting.end()) {
      ++it;
      continue;
    }
    if (!saved) {
      settled.cancelled.push_back(std::move(*it));
      it = continuations_.erase(it);
      continue;
    }
    waiting.erase(match);
    if (waiting.empty()) {
      settled.ready.push_back(std::move(*it));
      it = continuations_.erase(it);
      continue;
    }
    ++it;
  }
  return settled;
}

std::optional<SaveContinuation> SaveContinuationQueue::Remove(std::uint64_t id) {
  const auto match = std::find_if(continuations_.begin(), continuations_.end(),
                                  [id](const SaveContinuation& c) { return c.id == id; });
  if (match == continuations_.end()) {
    return std::nullopt;
  }
  SaveContinuation removed = std::move(*match);
  continuations_.erase(match);
  return removed;
}

const SaveContinuation* SaveContinuationQueue::Find(std::uint64_t id) const {
  const auto match = std::find_if(continuations_.begin(), continuations_.end(),
                                  [id](const SaveContinuation& c) { return c.id == id; });
  return match == continuations_.end() ? nullptr : &*match;
}

bool SaveContinuationQueue::IsWaitingOn(std::uint64_t tab_id) const {
  return std::any_of(continuations_.begin(), continuations_.end(),
                     [tab_id](const SaveContinuation& c) {
                       return std::find(c.waiting_tab_ids.begin(), c.waiting_tab_ids.end(),
                                        tab_id) != c.waiting_tab_ids.end();
                     });
}

}  // namespace microide::workspace
