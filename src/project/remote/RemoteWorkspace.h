#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "project/remote/RemoteManifest.h"

namespace microide::project::remote {

class RemoteServerClient;

// The client's end of the workspace half of the protocol (dev-docs/design/
// remote-projects.md § 6.2-6.4): the tree a connection's hello named as its root.
// A thin, stateless facade over a RemoteServerClient; the mirror engine owns what
// it does with the answers.
class RemoteWorkspace {
 public:
  // Ceiling on decoded rows, far over the server's remote.max_manifest_files: a
  // server streaming more is broken or hostile, and the connection is failed.
  static constexpr std::size_t kMaxManifestRows = 1'000'000;

  struct Manifest {
    std::uint64_t id = 0;
    std::vector<ManifestRow> rows;  // sorted by path, unique (the server's order)
    bool git = false;
  };
  using ManifestDone = std::function<void(std::optional<Manifest> manifest, std::string error)>;

  explicit RemoteWorkspace(RemoteServerClient& client) : client_(client) {}

  // tree/manifest. `done` runs once on the connection's I/O thread. Returns the
  // request id (cancellable through the peer), 0 when it could not be sent — then
  // `done` has already run with the error.
  std::uint64_t FetchManifest(ManifestDone done);
  // Blocking form, for a worker thread or a test; never the I/O thread.
  std::optional<Manifest> FetchManifestSync(std::string* error,
                                            std::chrono::milliseconds timeout = std::chrono::seconds(120));

 private:
  RemoteServerClient& client_;
};

}  // namespace microide::project::remote
