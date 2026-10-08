#pragma once

#include <memory>
#include <mutex>
#include <vector>

namespace microide::project::remote {

class RemoteServerClient;
class RemoteTerminalChannel;

// The CURRENT connection to one host (dev-docs/design/remote-projects.md § 6.6).
// A dropped link is replaced by a new RemoteServerClient on reconnect; everything
// that talks to the host — the project's launcher, its host terminals — holds this
// instead of a client, so it carries on over the new one. Replace() also reattaches
// every live host terminal, warm, from its own resume point.
//
// Threading: every method is safe from any thread.
class RemoteConnection {
 public:
  explicit RemoteConnection(std::shared_ptr<RemoteServerClient> client = nullptr)
      : client_(std::move(client)) {}

  std::shared_ptr<RemoteServerClient> client() const {
    std::lock_guard lock(mutex_);
    return client_;
  }

  // Swap in a new client (a reconnect) and reattach every live terminal over it.
  void Replace(std::shared_ptr<RemoteServerClient> client);

  // A terminal opened over this connection, to reattach on Replace.
  void Track(const std::shared_ptr<RemoteTerminalChannel>& channel);

 private:
  mutable std::mutex mutex_;
  std::shared_ptr<RemoteServerClient> client_;
  std::vector<std::weak_ptr<RemoteTerminalChannel>> terminals_;
};

}  // namespace microide::project::remote
