#pragma once

#include <cstddef>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace microide::server {

// The bytes of objects this server recently served, by hash: the bases a client's
// next fetch of the same path can be a delta against (remote-projects.md § 6.2).
// The client holds the version it was last sent; the host has only the current
// file, so without this there is nothing to diff against. Bounded, least recently
// used out; a miss costs a whole-file transfer, never correctness.
class RecentObjects {
 public:
  explicit RecentObjects(std::size_t capacity_bytes) : capacity_(capacity_bytes) {}

  void Put(std::string hash, std::string bytes) {
    std::lock_guard lock(mutex_);
    if (bytes.size() > capacity_ / 4) {
      return;  // one object may not crowd out the rest
    }
    if (const auto it = index_.find(hash); it != index_.end()) {
      order_.splice(order_.begin(), order_, it->second);
      return;
    }
    size_ += bytes.size();
    order_.emplace_front(hash, std::move(bytes));
    index_.emplace(std::move(hash), order_.begin());
    while (size_ > capacity_ && !order_.empty()) {
      size_ -= order_.back().second.size();
      index_.erase(order_.back().first);
      order_.pop_back();
    }
  }

  std::optional<std::string> Get(std::string_view hash) {
    std::lock_guard lock(mutex_);
    const auto it = index_.find(std::string(hash));
    if (it == index_.end()) {
      return std::nullopt;
    }
    order_.splice(order_.begin(), order_, it->second);
    return it->second->second;
  }

 private:
  std::mutex mutex_;
  std::size_t capacity_;
  std::size_t size_ = 0;
  std::list<std::pair<std::string, std::string>> order_;  // most recent first
  std::unordered_map<std::string, std::list<std::pair<std::string, std::string>>::iterator> index_;
};

}  // namespace microide::server
