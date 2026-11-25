#pragma once

#include <memory>
#include <mutex>
#include <vector>

namespace microide::project::remote {

class RemoteServerClient;

class Reattachable {
 public:
  virtual ~Reattachable() = default;
  virtual void Reattach(std::shared_ptr<RemoteServerClient> client) = 0;
};

class RemoteConnection {
 public:
  explicit RemoteConnection(std::shared_ptr<RemoteServerClient> client = nullptr)
      : client_(std::move(client)) {}

  std::shared_ptr<RemoteServerClient> client() const {
    std::lock_guard lock(mutex_);
    return client_;
  }

  void Replace(std::shared_ptr<RemoteServerClient> client);

  void Track(const std::shared_ptr<Reattachable>& item);

 private:
  mutable std::mutex mutex_;
  std::shared_ptr<RemoteServerClient> client_;
  std::vector<std::weak_ptr<Reattachable>> items_;
};

}
