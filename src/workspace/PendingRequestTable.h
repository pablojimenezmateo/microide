#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <unordered_map>
#include <utility>

namespace microide::workspace {

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

  std::optional<Entry> Take(int id) {
    const auto it = entries_.find(id);
    if (it == entries_.end()) {
      return std::nullopt;
    }
    Entry entry = std::move(it->second);
    entries_.erase(it);
    return entry;
  }

  bool Remove(int id) { return entries_.erase(id) != 0; }

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
  std::size_t size() const { return entries_.size(); }
  void clear() { entries_.clear(); }

 private:
  std::unordered_map<int, Entry> entries_;
};

}
