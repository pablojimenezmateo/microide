// The workspace half of the server's protocol (dev-docs/design/remote-projects.md
// § 6.2-6.4): the manifest, content reads, writes and tree operations of the root a
// connection's hello named. Every handler here hands its work to the workspace's
// workers and returns; a connection's I/O thread never touches the disk.
#include "server/RemoteServer.h"

#include <algorithm>
#include <map>
#include <utility>

#include "project/remote/RemoteManifest.h"
#include "project/remote/TreeFiles.h"
#include "util/ByteCodec.h"
#include "util/Log.h"

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

// Cut `count` rows into frames under the bulk chunk size, so an interactive frame
// queued behind them waits for at most one. `bytes_of(i)` is a row's variable
// size; the fixed part is estimated.
template <typename BytesOf, typename Emit>
void ForEachChunk(std::size_t count, const BytesOf& bytes_of, const Emit& emit) {
  std::size_t begin = 0;
  std::size_t estimate = 0;
  for (std::size_t i = 0; i < count; ++i) {
    const std::size_t row_bytes = bytes_of(i) + 64;
    if (i > begin && estimate + row_bytes > remote::RemoteFrameTransport::kMaxBulkChunkBytes) {
      emit(begin, i);
      begin = i;
      estimate = 0;
    }
    estimate += row_bytes;
  }
  if (begin < count) {
    emit(begin, count);
  }
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
                     // Encode outside the server lock (WithPeer holds it).
                     std::vector<std::string> chunks;
                     if (manifest.has_value()) {
                       const auto& rows = manifest->rows;
                       ForEachChunk(
                           rows.size(),
                           [&](std::size_t i) { return rows[i].path.size() + rows[i].link_target.size(); },
                           [&](std::size_t from, std::size_t to) {
                             chunks.emplace_back();
                             remote::EncodeManifestRows(rows.data() + from, to - from, chunks.back());
                           });
                       // A watching connection's baseline is exactly what it is sent.
                       std::lock_guard publish(tree->publish_mutex);
                       if (const auto subscriber = tree->subscribers.find(connection_id);
                           subscriber != tree->subscribers.end()) {
                         subscriber->second.last_sent =
                             std::make_shared<const std::vector<remote::ManifestRow>>(rows);
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

void RemoteServer::InstallWatchHandlers(Connection& connection) {
  connection.peer.OnRequest(remote::method::kWatchSubscribe, [this, &connection](
                                                                 std::uint64_t id,
                                                                 const util::JsonValue&) {
    const std::shared_ptr<ServedTree> served = TreeOf(connection);
    if (!served) {
      connection.peer.ReplyError(id, remote::kErrorInvalidParams,
                                 "no workspace: send server/hello with a root");
      return;
    }
    {
      std::lock_guard publish(served->publish_mutex);
      served->subscribers.try_emplace(connection.id);
    }
    // Starting the watch walks the tree: on the workspace's worker, not here. The
    // reply follows it, so `native` is known.
    ServedTree* tree = served.get();
    const std::uint64_t connection_id = connection.id;
    tree->queue.Post([this, tree, connection_id, id]() {
      if (!tree->watch && !tree->closing.load()) {
        tree->watch = std::make_unique<WorkspaceWatch>(
            tree->tree.root(), WorkspaceWatch::Options{},
            [this, tree](WorkspaceTree::Changes changes) {
              if (tree->closing.load()) {
                return;
              }
              {
                // Batches that pile up behind a slow one fold into the next.
                std::lock_guard pending(tree->watch_mutex);
                WorkspaceTree::Changes& into = tree->watch_changes;
                into.touched.insert(into.touched.end(), changes.touched.begin(), changes.touched.end());
                into.deleted_directories.insert(into.deleted_directories.end(),
                                                changes.deleted_directories.begin(),
                                                changes.deleted_directories.end());
                into.full = into.full || changes.full;
              }
              tree->queue.PostLatest("watch", [this, tree]() { PublishWatchBatch(*tree); });
            });
      }
      util::JsonObject result;
      result["native"] = util::JsonValue(tree->watch && tree->watch->native());
      WithPeer(connection_id,
               [&](remote::RemotePeer& peer) { peer.Reply(id, util::JsonValue(std::move(result))); });
    });
  });
}

void RemoteServer::PublishWatchBatch(ServedTree& tree) {
  {
    std::lock_guard publish(tree.publish_mutex);
    const bool anyone_primed =
        std::any_of(tree.subscribers.begin(), tree.subscribers.end(),
                    [](const auto& entry) { return entry.second.last_sent != nullptr; });
    if (!anyone_primed) {
      return;  // nobody has a baseline yet: their next manifest carries this
    }
  }
  WorkspaceTree::Changes changes;
  {
    std::lock_guard pending(tree.watch_mutex);
    changes = std::exchange(tree.watch_changes, {});
  }
  std::string error;
  std::optional<WorkspaceTree::Manifest> manifest =
      tree.tree.UpdateManifest(changes, &error, [&tree]() { return tree.closing.load(); });
  if (!manifest.has_value()) {
    util::Log("watch batch for " + tree.tree.root().string() + " failed: " + error);
    return;
  }
  auto current = std::make_shared<const std::vector<remote::ManifestRow>>(std::move(manifest->rows));
  // Subscribers primed by the same manifest share a baseline: one diff for each
  // distinct baseline, not one per connection.
  std::map<const std::vector<remote::ManifestRow>*, std::vector<std::string>> frames_for;
  std::vector<std::pair<std::uint64_t, const std::vector<remote::ManifestRow>*>> sends;
  // The replaced baselines stay alive until their frames are sent: they key the map.
  std::vector<std::shared_ptr<const std::vector<remote::ManifestRow>>> previous;
  {
    std::lock_guard publish(tree.publish_mutex);
    for (auto& [connection_id, subscriber] : tree.subscribers) {
      if (!subscriber.last_sent) {
        continue;
      }
      const std::vector<remote::ManifestRow>* baseline = subscriber.last_sent.get();
      if (!frames_for.contains(baseline)) {
        std::vector<const remote::ManifestRow*> changed;
        std::vector<const std::string*> deleted;
        remote::DiffManifests(*baseline, *current, changed, deleted);
        std::vector<std::string>& frames = frames_for[baseline];
        // Deletes ride the first frame; rows are chunked.
        ForEachChunk(
            changed.size(), [&](std::size_t i) { return changed[i]->path.size(); },
            [&](std::size_t from, std::size_t to) {
              frames.emplace_back();
              const bool first = from == 0;
              remote::EncodeWatchDelta(deleted.data(), first ? deleted.size() : 0,
                                       changed.data() + from, to - from, frames.back());
            });
        if (changed.empty() && !deleted.empty()) {
          frames.emplace_back();
          remote::EncodeWatchDelta(deleted.data(), deleted.size(), nullptr, 0, frames.back());
        }
      }
      sends.emplace_back(connection_id, baseline);
      previous.push_back(std::exchange(subscriber.last_sent, current));
    }
    for (const auto& [connection_id, baseline] : sends) {
      const std::vector<std::string>& frames = frames_for[baseline];
      if (frames.empty()) {
        continue;  // the batch changed nothing this connection can see
      }
      WithPeer(connection_id, [&](remote::RemotePeer& peer) {
        for (const std::string& frame : frames) {
          peer.SendContent(remote::FrameType::WatchDelta, manifest->id, frame, remote::Lane::Bulk);
        }
      });
    }
  }
}

namespace {

// Stream one file's bytes as ObjectData frames (varint `index`, then a piece) and
// answer what object/fetch and file/read answer for it.
template <typename Send, typename Read>
util::JsonValue StreamObject(std::size_t index, const Send& send, const Read& read) {
  std::string prefix;
  util::PutVarint(prefix, index);
  std::string frame = prefix;
  const auto flush = [&]() {
    if (frame.size() > prefix.size()) {
      send(frame);
    }
    frame = prefix;
  };
  const remote::FileOpResult result = read([&](std::string_view chunk) {
    while (!chunk.empty()) {
      const std::size_t room = remote::RemoteFrameTransport::kMaxBulkChunkBytes - frame.size();
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
  if (result.ok()) {
    entry["hash"] = util::JsonValue(result.current->Hex());
  } else if (result.status == remote::FileOpResult::Status::Conflict) {
    entry["missing"] = util::JsonValue(true);
  } else {
    entry["error"] = util::JsonValue(result.error);
  }
  return util::JsonValue(std::move(entry));
}

}  // namespace

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
        results.push_back(StreamObject(
            index,
            [&](std::string_view frame) {
              WithPeer(connection_id, [&](remote::RemotePeer& peer) {
                peer.SendContent(remote::FrameType::ObjectData, id, frame, lane);
              });
            },
            [&](const std::function<void(std::string_view)>& sink) {
              return remote::ReadTreeFile(tree->tree.root(), paths[index], max_bytes, sink);
            }));
      }
      util::JsonObject reply;
      reply["objects"] = util::JsonValue(std::move(results));
      WithPeer(connection_id, [&](remote::RemotePeer& peer) {
        peer.Reply(id, util::JsonValue(std::move(reply)), lane);
      });
    });
  });

  peer.OnRequest(remote::method::kFileRead, [this, &connection](std::uint64_t id,
                                                                const util::JsonValue& params) {
    const std::shared_ptr<ServedTree> served = TreeOf(connection);
    const util::JsonValue& path = params["path"];
    if (!served || !path.IsString() || path.AsString().empty() || path.AsString().front() != '/') {
      connection.peer.ReplyError(id, remote::kErrorInvalidParams,
                                 served ? "file/read needs an absolute path"
                                        : "no workspace: send server/hello with a root");
      return;
    }
    ServedTree* tree = served.get();
    const std::uint64_t connection_id = connection.id;
    tree->io_queue.Post([this, connection_id, id, path = path.AsString()]() {
      util::JsonArray results;
      results.push_back(StreamObject(
          0,
          [&](std::string_view frame) {
            WithPeer(connection_id, [&](remote::RemotePeer& peer) {
              peer.SendContent(remote::FrameType::ObjectData, id, frame, remote::Lane::Interactive);
            });
          },
          [&](const std::function<void(std::string_view)>& sink) {
            return remote::ReadHostFile(path, kMaxObjectBytes, sink);
          }));
      util::JsonObject reply;
      reply["objects"] = util::JsonValue(std::move(results));
      WithPeer(connection_id, [&](remote::RemotePeer& peer) {
        peer.Reply(id, util::JsonValue(std::move(reply)));
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
