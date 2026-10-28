#pragma once

#include <chrono>
#include <optional>
#include <unordered_map>
#include <utility>

namespace microide::workspace {

// The request-id -> response-callback table of a stdio request/response client
// (the LSP client and the DAP client): a request is added when it is sent, taken
// by the response that answers it, removed by a failed send, and swept -- every
// entry when the peer is gone, only the expired ones on the periodic timeout pass
// -- so a peer that never answers cannot strand a request. Exactly one of those
// claims an entry, which is what keeps a callback from firing twice.
//
// Not synchronized: the owning client's mutex guards it, because the client's
// other protocol state is updated under the same lock.
template <typename Callback>
class PendingRequestTable {
 public:
  using Clock = std::chrono::steady_clock;
  struct Entry {
    Callback callback;
    Clock::time_point deadline;
    Clock::time_point sent_at;
  };

  void Add(int id, Callback callback, std::chrono::milliseconds timeout) {
    const Clock::time_point now = Clock::now();
    entries_[id] = Entry{.callback = std::move(callback), .deadline = now + timeout, .sent_at = now};
  }

  // The entry a response answers, now claimed; nullopt when a sweep or a failed
  // send claimed it first (or the id was never ours).
  std::optional<Entry> Take(int id) {
    const auto it = entries_.find(id);
    if (it == entries_.end()) {
      return std::nullopt;
    }
    Entry entry = std::move(it->second);
    entries_.erase(it);
    return entry;
  }

  // A failed send's cleanup. True iff the entry was still unclaimed: when the EOF
  // sweep already failed it, the sender must not report the failure again.
  bool Remove(int id) { return entries_.erase(id) != 0; }

  // Claim the entries a sweep fails -- all of them, or only those past their
  // deadline at `now` -- handing each to `fail(id, entry)`. Returns how many.
  template <typename Fail>
  std::size_t Sweep(bool only_expired, Clock::time_point now, Fail&& fail) {
    std::size_t claimed = 0;
    for (auto it = entries_.begin(); it != entries_.end();) {
      if (only_expired && now < it->second.deadline) {
        ++it;
        continue;
      }
      const int id = it->first;
      Entry entry = std::move(it->second);
      it = entries_.erase(it);
      fail(id, std::move(entry));
      ++claimed;
    }
    return claimed;
  }

  bool empty() const { return entries_.empty(); }
  void clear() { entries_.clear(); }

 private:
  std::unordered_map<int, Entry> entries_;
};

}  // namespace microide::workspace
