#include "project/remote/RemoteConnection.h"

#include <algorithm>

namespace microide::project::remote {

void RemoteConnection::Replace(std::shared_ptr<RemoteServerClient> client) {
  std::vector<std::shared_ptr<Reattachable>> live;
  {
    std::lock_guard lock(mutex_);
    client_ = client;
    std::erase_if(items_, [](const std::weak_ptr<Reattachable>& weak) { return weak.expired(); });
    for (const auto& weak : items_) {
      if (auto item = weak.lock()) {
        live.push_back(std::move(item));
      }
    }
  }
  // Outside the lock: a reattach may block on a reply (a process's proc/attach).
  for (const auto& item : live) {
    item->Reattach(client);
  }
}

void RemoteConnection::Track(const std::shared_ptr<Reattachable>& item) {
  std::lock_guard lock(mutex_);
  std::erase_if(items_, [](const std::weak_ptr<Reattachable>& weak) { return weak.expired(); });
  items_.push_back(item);
}

}  // namespace microide::project::remote
