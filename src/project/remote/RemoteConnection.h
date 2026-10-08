#pragma once

#include <memory>
#include <mutex>
#include <vector>

namespace microide::project::remote {

class RemoteServerClient;

// Something bound to a host connection that carries on over the next one: a host
// terminal (warm reattach from its resume point), a kept host process (proc/attach
// from the bytes it received). A null client means the connection is gone for
// good (Disconnect): end, rather than wait for a link that is not coming.
class Reattachable {
 public:
  virtual ~Reattachable() = default;
  virtual void Reattach(std::shared_ptr<RemoteServerClient> client) = 0;
};

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

  // Swap in a new client (a reconnect), or null (gone for good), and hand it to
  // everything tracked.
  void Replace(std::shared_ptr<RemoteServerClient> client);

  // A terminal or process opened over this connection, to carry over on Replace.
  void Track(const std::shared_ptr<Reattachable>& item);

 private:
  mutable std::mutex mutex_;
  std::shared_ptr<RemoteServerClient> client_;
  std::vector<std::weak_ptr<Reattachable>> items_;
};

}  // namespace microide::project::remote
