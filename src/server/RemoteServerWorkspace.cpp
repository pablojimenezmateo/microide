// The workspace half of the server's protocol (dev-docs/design/remote-projects.md
// § 6.2-6.4): the manifest, content reads, writes and tree operations of the root a
// connection's hello named. Every handler here hands its work to the workspace's
// workers and returns; a connection's I/O thread never touches the disk.
#include "server/RemoteServer.h"

#include <utility>

#include "project/remote/RemoteManifest.h"
#include "project/remote/TreeFiles.h"
#include "util/ByteCodec.h"

namespace microide::server {
namespace {

// The largest object a fetch serves unless the request asks for less.
constexpr std::uint64_t kMaxObjectBytes = 64u * 1024 * 1024;

util::JsonValue OpResultJson(const remote::FileOpResult& result) {
  util::JsonObject json;
  if (result.status == remote::FileOpResult::Status::Conflict) {
    json["conflict"] = util::JsonValue(true);
    json["current"] = result.current.has_value() ? util::JsonValue(result.current->Hex())
                                                 : util::JsonValue();
  } else if (result.current.has_value()) {
    json["hash"] = util::JsonValue(result.current->Hex());
  }
  return util::JsonValue(std::move(json));
}

}  // namespace

std::shared_ptr<RemoteServer::ServedTree> RemoteServer::TreeOf(const Connection& connection) {
  std::lock_guard lock(mutex_);
  if (connection.root.empty()) {
    return nullptr;
  }
  const auto workspace = workspaces_.find(connection.root);
  return workspace == workspaces_.end() ? nullptr : workspace->second.tree;
}

bool RemoteServer::RequestLive(std::uint64_t connection_id, std::uint64_t request_id) {
  bool live = false;
  WithPeer(connection_id, [&](remote::RemotePeer& peer) {
    live = !peer.closed() && !peer.IsCancelled(request_id);
  });
  return live;
}

void RemoteServer::InstallTreeHandlers(Connection& connection) {
  remote::RemotePeer& peer = connection.peer;
  peer.OnRequest(remote::method::kTreeManifest,
                 [this, &connection](std::uint64_t id, const util::JsonValue&) {
                   const std::shared_ptr<ServedTree> served = TreeOf(connection);
                   if (!served) {
                     connection.peer.ReplyError(id, remote::kErrorInvalidParams,
                                                "no workspace: send server/hello with a root");
                     return;
                   }
                   // The raw pointer is safe in the job: ~ServedTree joins the queue
                   // before the tree goes.
                   ServedTree* tree = served.get();
                   const std::uint64_t connection_id = connection.id;
                   tree->queue.Post([this, tree, connection_id, id]() {
                     std::string error;
                     const auto cancelled = [&]() {
                       return tree->closing.load() || !RequestLive(connection_id, id);
                     };
                     std::optional<WorkspaceTree::Manifest> manifest =
                         tree->tree.BuildManifest(&error, cancelled);
                     // Encode outside the server lock (WithPeer holds it), in chunks
                     // under the bulk frame size so a keystroke's frame waits for at
                     // most one of them.
                     std::vector<std::string> chunks;
                     if (manifest.has_value()) {
                       const auto& rows = manifest->rows;
                       const auto encode = [&](std::size_t from, std::size_t to) {
                         chunks.emplace_back();
                         remote::EncodeManifestRows(rows.data() + from, to - from, chunks.back());
                       };
                       std::size_t begin = 0;
                       std::size_t estimate = 0;
                       for (std::size_t i = 0; i < rows.size(); ++i) {
                         const std::size_t row_bytes =
                             rows[i].path.size() + rows[i].link_target.size() + 64;
                         if (i > begin && estimate + row_bytes >
                                              remote::RemoteFrameTransport::kMaxBulkChunkBytes) {
                           encode(begin, i);
                           begin = i;
                           estimate = 0;
                         }
                         estimate += row_bytes;
                       }
                       if (begin < rows.size()) {
                         encode(begin, rows.size());
                       }
                     }
                     WithPeer(connection_id, [&](remote::RemotePeer& peer) {
                       if (!manifest.has_value()) {
                         peer.ReplyError(id,
                                         error == "cancelled" ? remote::kErrorCancelled
                                                              : remote::kErrorInvalidParams,
                                         error);
                         return;
                       }
                       for (const std::string& chunk : chunks) {
                         peer.SendContent(remote::FrameType::TreeRows, id, chunk,
                                          remote::Lane::Bulk);
                       }
                       util::JsonObject result;
                       result["manifest_id"] =
                           util::JsonValue(static_cast<std::int64_t>(manifest->id));
                       result["rows"] =
                           util::JsonValue(static_cast<std::int64_t>(manifest->rows.size()));
                       result["git"] = util::JsonValue(manifest->git);
                       peer.Reply(id, util::JsonValue(std::move(result)), remote::Lane::Bulk);
                     });
                   });
                 });
}

void RemoteServer::InstallFileHandlers(Connection& connection) {
  remote::RemotePeer& peer = connection.peer;
  peer.OnRequest(remote::method::kObjectFetch, [this, &connection](std::uint64_t id,
                                                                   const util::JsonValue& params) {
    const std::shared_ptr<ServedTree> served = TreeOf(connection);
    const util::JsonValue& objects = params["objects"];
    if (!served || !objects.IsArray() || objects.AsArray().size() > 4096) {
      connection.peer.ReplyError(id, remote::kErrorInvalidParams,
                                 served ? "objects must be an array of at most 4096"
                                        : "no workspace: send server/hello with a root");
      return;
    }
    std::vector<std::string> paths;
    for (const util::JsonValue& object : objects.AsArray()) {
      paths.push_back(object["path"].AsString());
    }
    const std::int64_t asked_max = params["max_bytes"].AsInt(0);
    const std::uint64_t max_bytes =
        asked_max > 0 ? std::min<std::uint64_t>(static_cast<std::uint64_t>(asked_max), kMaxObjectBytes)
                      : kMaxObjectBytes;
    // Content and the reply travel on the lane the request came on: the file the
    // user just opened is interactive, backfill is bulk.
    const remote::Lane lane = params["bulk"].AsBool(false) ? remote::Lane::Bulk
                                                           : remote::Lane::Interactive;
    ServedTree* tree = served.get();
    const std::uint64_t connection_id = connection.id;
    tree->io_queue.Post([this, tree, connection_id, id, paths = std::move(paths), max_bytes, lane]() {
      util::JsonArray results;
      for (std::size_t index = 0; index < paths.size(); ++index) {
        if (tree->closing.load() || !RequestLive(connection_id, id)) {
          WithPeer(connection_id, [&](remote::RemotePeer& peer) {
            peer.ReplyError(id, remote::kErrorCancelled, "cancelled");
          });
          return;
        }
        std::string prefix;
        util::PutVarint(prefix, index);
        std::string frame;
        const auto flush = [&]() {
          if (frame.size() > prefix.size()) {
            WithPeer(connection_id, [&](remote::RemotePeer& peer) {
              peer.SendContent(remote::FrameType::ObjectData, id, frame, lane);
            });
          }
          frame = prefix;
        };
        frame = prefix;
        const remote::FileOpResult read = remote::ReadTreeFile(
            tree->tree.root(), paths[index], max_bytes, [&](std::string_view chunk) {
              while (!chunk.empty()) {
                const std::size_t room =
                    remote::RemoteFrameTransport::kMaxBulkChunkBytes - frame.size();
                const std::size_t take = std::min(room, chunk.size());
                frame.append(chunk.substr(0, take));
                chunk.remove_prefix(take);
                if (frame.size() == remote::RemoteFrameTransport::kMaxBulkChunkBytes) {
                  flush();
                }
              }
            });
        flush();
        util::JsonObject entry;
        if (read.ok()) {
          entry["hash"] = util::JsonValue(read.current->Hex());
        } else if (read.status == remote::FileOpResult::Status::Conflict) {
          entry["missing"] = util::JsonValue(true);
        } else {
          entry["error"] = util::JsonValue(read.error);
        }
        results.push_back(util::JsonValue(std::move(entry)));
      }
      util::JsonObject reply;
      reply["objects"] = util::JsonValue(std::move(results));
      WithPeer(connection_id, [&](remote::RemotePeer& peer) {
        peer.Reply(id, util::JsonValue(std::move(reply)), lane);
      });
    });
  });

  peer.OnRequest(remote::method::kFileWrite, [this, &connection](std::uint64_t id,
                                                                 const util::JsonValue& params) {
    // Claim the content first, whatever the verdict, so a refused write cannot pin it.
    std::string content;
    if (const auto upload = connection.uploads.find(id); upload != connection.uploads.end()) {
      content = std::move(upload->second);
      connection.upload_bytes -= content.size();
      connection.uploads.erase(upload);
    }
    const std::shared_ptr<ServedTree> served = TreeOf(connection);
    const std::optional<remote::Precondition> expect = remote::PreconditionFromJson(params["expect"]);
    const util::JsonValue& path = params["path"];
    const util::JsonValue& mode = params["mode"];
    if (!served || !expect.has_value() || !path.IsString() || (!mode.IsNull() && !mode.IsInt())) {
      connection.peer.ReplyError(id, remote::kErrorInvalidParams,
                                 served ? "file/write needs a path and an expect"
                                        : "no workspace: send server/hello with a root");
      return;
    }
    std::optional<std::uint32_t> bits;
    if (mode.IsInt()) {
      bits = static_cast<std::uint32_t>(mode.AsInt(0) & 07777);
    }
    ServedTree* tree = served.get();
    const std::uint64_t connection_id = connection.id;
    tree->io_queue.Post([this, tree, connection_id, id, path = path.AsString(),
                         content = std::move(content), expect = *expect, bits]() {
      const remote::FileOpResult result = remote::WriteTreeFile(tree->tree.root(), path, content, expect, bits);
      WithPeer(connection_id, [&](remote::RemotePeer& peer) {
        if (result.status == remote::FileOpResult::Status::Error) {
          peer.ReplyError(id, remote::kErrorInvalidParams, result.error);
        } else {
          peer.Reply(id, OpResultJson(result));
        }
      });
    });
  });

  peer.OnRequest(remote::method::kFsOp, [this, &connection](std::uint64_t id,
                                                            const util::JsonValue& params) {
    const std::shared_ptr<ServedTree> served = TreeOf(connection);
    const std::string& op = params["op"].AsString();
    const util::JsonValue& path = params["path"];
    const util::JsonValue& to = params["to"];
    std::optional<remote::Precondition> expect = remote::Precondition::Anything();
    if (!params["expect"].IsNull()) {
      expect = remote::PreconditionFromJson(params["expect"]);
    }
    const bool known = op == "mkdir" || op == "rename" || op == "delete";
    if (!served || !known || !path.IsString() || !expect.has_value() ||
        (op == "rename" && !to.IsString())) {
      connection.peer.ReplyError(id, remote::kErrorInvalidParams,
                                 served ? "fs/op needs a known op, a path and a valid expect"
                                        : "no workspace: send server/hello with a root");
      return;
    }
    ServedTree* tree = served.get();
    const std::uint64_t connection_id = connection.id;
    tree->io_queue.Post([this, tree, connection_id, id, op, path = path.AsString(),
                         to = to.IsString() ? to.AsString() : std::string(), expect = *expect]() {
      const std::filesystem::path& root = tree->tree.root();
      const remote::FileOpResult result = op == "mkdir"    ? remote::MakeTreeDirectory(root, path)
                                  : op == "rename" ? remote::RenameTreeEntry(root, path, to, expect)
                                                   : remote::DeleteTreeEntry(root, path, expect);
      WithPeer(connection_id, [&](remote::RemotePeer& peer) {
        if (result.status == remote::FileOpResult::Status::Error) {
          peer.ReplyError(id, remote::kErrorInvalidParams, result.error);
        } else {
          peer.Reply(id, OpResultJson(result));
        }
      });
    });
  });
}

}  // namespace microide::server
