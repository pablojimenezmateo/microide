#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "project/remote/RemoteFrame.h"
#include "project/remote/RemoteManifest.h"
#include "project/remote/RemoteProtocol.h"

namespace microide::project::remote {

class RemoteConnection;
class RemoteServerClient;

// The client's end of the workspace half of the protocol (dev-docs/design/
// remote-projects.md § 6.2-6.4): the tree a connection's hello named as its root.
// A thin, stateless facade over the host's CURRENT connection (a reconnect
// replaces the client underneath, as for the launcher); the mirror engine owns
// what it does with the answers. Every call fails fast with "not connected" while
// there is no client.
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

  explicit RemoteWorkspace(std::shared_ptr<RemoteConnection> connection)
      : connection_(std::move(connection)) {}

  // tree/manifest. `done` runs once on the connection's I/O thread. Returns the
  // request id (cancellable through the peer), 0 when it could not be sent — then
  // `done` has already run with the error.
  std::uint64_t FetchManifest(ManifestDone done);
  // Blocking form, for a worker thread or a test; never the I/O thread.
  std::optional<Manifest> FetchManifestSync(std::string* error,
                                            std::chrono::milliseconds timeout = std::chrono::seconds(120));

  // watch/subscribe: from the next manifest on, every change to the host's content
  // set arrives through `on_delta` (I/O thread), against what this connection was
  // last sent. Subscribe BEFORE fetching the manifest. False with *error when the
  // server refused; *native says whether the host notifies or polls.
  struct WatchDelta {
    std::uint64_t manifest_id = 0;
    std::vector<std::string> deleted;
    std::vector<ManifestRow> rows;
  };
  // git/subscribe on the current connection: `changed` runs on its I/O thread
  // after every pushed git status (RemoteServerClient::SubscribeGitStatus).
  bool SubscribeGitStatus(std::function<void()> changed, std::string* error);
  bool SubscribeWatch(std::function<void(WatchDelta delta)> on_delta, bool* native,
                      std::string* error);

  // object/fetch: the current bytes at each path, and the hash of exactly those
  // bytes (verified here, not taken on trust). `lane` is Interactive for a file the
  // user is waiting on, Bulk for backfill.
  struct FetchedObject {
    std::string path;
    std::optional<util::ContentHash> hash;  // set when the read succeeded
    bool missing = false;                   // nothing at the path on the host
    std::string error;
    std::string content;
  };
  using FetchDone =
      std::function<void(std::optional<std::vector<FetchedObject>> objects, std::string error)>;
  std::uint64_t FetchObjects(std::vector<std::string> paths, Lane lane, FetchDone done,
                             std::uint64_t max_bytes = 0);
  // file/read: one ABSOLUTE host path outside the content set, read-only (a system
  // header a language server names). `done` gets one FetchedObject, as above.
  std::uint64_t ReadHostFile(std::string host_path, FetchDone done);
  std::optional<std::vector<FetchedObject>> FetchObjectsSync(
      std::vector<std::string> paths, Lane lane, std::string* error,
      std::chrono::milliseconds timeout = std::chrono::seconds(120));

  // file/write and fs/op: compare-and-swap against `expect` (§ 6.3).
  struct WriteResult {
    enum class Status {
      Ok,        // hash: what the path holds now (file/write)
      Conflict,  // hash: what is there instead, nullopt when nothing is
      Error,     // error says why; nothing changed on the host
    };
    Status status = Status::Error;
    std::optional<util::ContentHash> hash;
    std::string error;
    // The request never got an answer (no connection, or it closed): nothing is
    // known about the host, and the operation is worth retrying on the next one.
    bool unreachable = false;
  };
  using WriteDone = std::function<void(WriteResult result)>;
  std::uint64_t WriteFile(std::string path, std::string_view content, const Precondition& expect,
                          std::optional<std::uint32_t> mode, Lane lane, WriteDone done);
  WriteResult WriteFileSync(std::string path, std::string_view content, const Precondition& expect,
                            std::optional<std::uint32_t> mode = std::nullopt,
                            std::chrono::milliseconds timeout = std::chrono::seconds(120));
  enum class TreeOp { MakeDirectory, Rename, Delete };
  std::uint64_t ApplyTreeOp(TreeOp op, std::string path, std::string to, const Precondition& expect,
                            WriteDone done);
  WriteResult ApplyTreeOpSync(TreeOp op, std::string path, std::string to,
                              const Precondition& expect,
                              std::chrono::milliseconds timeout = std::chrono::seconds(120));

 private:
  std::shared_ptr<RemoteServerClient> Client() const;
  // object/fetch and file/read share the streaming and the checking.
  std::uint64_t StreamObjects(std::string_view method, util::JsonValue params,
                              std::vector<std::string> names, Lane lane, FetchDone done);

  std::shared_ptr<RemoteConnection> connection_;
};

}  // namespace microide::project::remote
