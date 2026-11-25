#include "server/RemoteServer.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <span>
#include <map>
#include <utility>

#include <sys/stat.h>
#include <unistd.h>

#include "platform/ProcessLauncher.h"
#include "platform/TransientFileStore.h"
#include "project/GitMetadataSource.h"
#include "project/GitRepository.h"
#include "project/remote/RemoteManifest.h"
#include "project/remote/RemoteSearchWire.h"
#include "project/remote/RemoteWorkspace.h"
#include "project/remote/TreeFiles.h"
#include "util/ByteCodec.h"
#include "util/ContentHash.h"
#include "util/DurableFile.h"
#include "util/HeapUsage.h"
#include "util/TextFileIO.h"
#include "util/Zstd.h"
#include "util/Log.h"

namespace microide::server {
namespace {

constexpr std::uint64_t kMaxObjectBytes = remote::RemoteWorkspace::kMaxBufferedObjectBytes;
void ReplyInvalidParams(remote::RemotePeer& peer, std::uint64_t id, bool has_workspace,
                        std::string_view problem) {
  peer.ReplyError(id, remote::kErrorInvalidParams,
                  has_workspace ? std::string(problem)
                                : std::string("no workspace: send server/hello with a root"));
}

constexpr std::uint64_t kMaxDeltaBaseBytes = 8u * 1024 * 1024;
constexpr std::size_t kIndexBaseMinBytes = 16 * 1024;
constexpr std::uint64_t kTrimAfterServedBytes = 4u * 1024 * 1024;

std::optional<std::string> IndexBlobWithHash(const std::filesystem::path& root, std::string_view path,
                                             std::string_view hash_hex) {
  if (project::LocalGitMetadataSource().Availability(root) != project::GitAvailability::Repository) {
    return std::nullopt;
  }
  const project::GitRepository repo(root, platform::LocalProcessLauncher());
  std::optional<project::GitRepository::BlobAtRevision> blob =
      repo.ReadBlobAtRevision(std::filesystem::path(std::string(path)), "");
  if (!blob.has_value() || blob->truncated || blob->content.size() > kMaxDeltaBaseBytes ||
      util::HashContent(blob->content).Hex() != hash_hex) {
    return std::nullopt;
  }
  return std::move(blob->content);
}

util::JsonValue OpResultJson(const remote::FileOpResult& result, std::uint64_t manifest = 0) {
  util::JsonObject json;
  if (manifest != 0) {
    json["manifest"] = util::JsonValue(static_cast<std::int64_t>(manifest));
  }
  if (result.status == remote::FileOpResult::Status::Conflict) {
    json["conflict"] = util::JsonValue(true);
    json["current"] = result.current.has_value() ? util::JsonValue(result.current->Hex())
                                                 : util::JsonValue();
  } else if (result.current.has_value()) {
    json["hash"] = util::JsonValue(result.current->Hex());
  }
  return util::JsonValue(std::move(json));
}

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

}

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
                     ReplyInvalidParams(connection.peer, id, false, {});
                     return;
                   }
                   ServedTree* tree = served.get();
                   const std::uint64_t connection_id = connection.id;
                   tree->queue.Post([this, tree, connection_id, id]() {
                     std::string error;
                     const auto cancelled = [&]() {
                       return tree->closing.load() || !RequestLive(connection_id, id);
                     };
                     std::optional<WorkspaceTree::Manifest> manifest =
                         tree->tree.BuildManifest(&error, cancelled);
                     if (manifest.has_value() && tree->watch) {
                       tree->watch->SetPolledPaths(tree->tree.UnwatchedMembers());
                     }
                     std::vector<std::string> chunks;
                     if (manifest.has_value()) {
                       const std::vector<remote::ManifestRow>& rows = *manifest->rows;
                       ForEachChunk(
                           rows.size(),
                           [&](std::size_t i) { return rows[i].path.size() + rows[i].link_target.size(); },
                           [&](std::size_t from, std::size_t to) {
                             chunks.emplace_back();
                             remote::EncodeManifestRows(rows.data() + from, to - from, chunks.back());
                           });
                       std::lock_guard publish(tree->publish_mutex);
                       if (const auto subscriber = tree->subscribers.find(connection_id);
                           subscriber != tree->subscribers.end()) {
                         subscriber->second.last_sent = manifest->rows;
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
                           util::JsonValue(static_cast<std::int64_t>(manifest->rows->size()));
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
      ReplyInvalidParams(connection.peer, id, false, {});
      return;
    }
    {
      std::lock_guard publish(served->publish_mutex);
      served->subscribers.try_emplace(connection.id);
    }
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
        tree->watch->SetPolledPaths(tree->tree.UnwatchedMembers());
        tree->published_head.SetProjectRoot(tree->tree.root());
        tree->watch_mode.store(tree->watch->native() ? WatchMode::Native : WatchMode::Polling);
      }
      util::JsonObject result;
      result["native"] = util::JsonValue(tree->watch && tree->watch->native());
      WithPeer(connection_id,
               [&](remote::RemotePeer& peer) { peer.Reply(id, util::JsonValue(std::move(result))); });
    });
  });
}

void RemoteServer::InstallGitHandlers(Connection& connection) {
  connection.peer.OnRequest("git/subscribe", [this, &connection](std::uint64_t id,
                                                                 const util::JsonValue&) {
    const std::shared_ptr<ServedTree> served = TreeOf(connection);
    if (!served) {
      ReplyInvalidParams(connection.peer, id, false, {});
      return;
    }
    {
      std::lock_guard publish(served->publish_mutex);
      served->git_subscribers.insert(connection.id);
    }
    ServedTree* tree = served.get();
    tree->queue.Post([this, tree]() {
      tree->git_last_pushed.reset();
      if (!tree->git_sampler && !tree->closing.load()) {
        tree->git_sampler = std::make_unique<GitStatusSampler>(
            tree->tree.root(), std::chrono::seconds(30), [this, tree]() {
              ScheduleGitStatus(*tree);
              tree->queue.PostLatest("watch", [this, tree]() {
                if (tree->watch) {
                  PublishWatchBatch(*tree);
                }
              });
            });
      }
    });
    ScheduleGitStatus(*tree);
    connection.peer.Reply(id, util::JsonValue(true));
  });
}

void RemoteServer::InstallSearchHandlers(Connection& connection) {
  connection.peer.OnRequest("search/run", [this, &connection](std::uint64_t id,
                                                              const util::JsonValue& params) {
    const std::shared_ptr<ServedTree> served = TreeOf(connection);
    std::string error;
    auto parsed = remote::ParseSearchRunParams(params, &error);
    if (!served || !parsed.has_value()) {
      ReplyInvalidParams(connection.peer, id, served != nullptr, error);
      return;
    }
    const std::uint64_t connection_id = connection.id;
    search_executor_.Submit([this, served, connection_id, id, query = std::move(parsed->first),
                             options = std::move(parsed->second)](const util::CancellationToken& token) {
      std::string why;
      std::optional<std::vector<std::string>> paths = served->tree.SearchPaths(&why);
      project::ProjectSearchCompletion completion;
      if (!paths.has_value()) {
        completion.error = why.empty() ? "the host could not list the project" : why;
      } else {
        std::atomic_bool cancel{false};
        std::size_t searched = 0;
        std::size_t total = 0;
        std::mutex send_mutex;
        const project::ProjectSearchSink sink{
            .results =
                [&](std::vector<project::ProjectSearchResult> batch) {
                  std::lock_guard lock(send_mutex);
                  const util::JsonValue payload = remote::SearchResultsJson(batch, searched, total);
                  WithPeer(connection_id, [&](remote::RemotePeer& peer) {
                    peer.Notify("search/results", payload, remote::Lane::Bulk, id);
                  });
                },
            .progress =
                [&](std::size_t searched_files, std::size_t total_files) {
                  {
                    std::lock_guard lock(send_mutex);
                    searched = searched_files;
                    total = total_files;
                  }
                  if (!RequestLive(connection_id, id)) {
                    cancel.store(true);
                  }
                }};
        completion = project::RunProjectSearch(
            served->tree.root(), query, options,
            std::make_shared<const std::vector<std::string>>(std::move(*paths)), sink, cancel, token);
        sink.results({});
      }
      WithPeer(connection_id, [&](remote::RemotePeer& peer) {
        peer.Reply(id, remote::SearchCompletionJson(completion), remote::Lane::Bulk);
      });
    });
  });
}

void RemoteServer::ScheduleGitStatus(ServedTree& tree) {
  {
    std::lock_guard publish(tree.publish_mutex);
    if (tree.git_subscribers.empty()) {
      return;
    }
  }
  ServedTree* raw = &tree;
  tree.queue.PostLatest("git", [this, raw]() { PublishGitStatus(*raw); });
}

void RemoteServer::AddGitMetadata(const std::string& root, util::JsonObject& into) {
  const project::GitMetadataSource& local = project::LocalGitMetadataSource();
  into["repository"] = util::JsonValue(local.Availability(root) == project::GitAvailability::Repository);
  const auto git_dir = local.ReadableGitDirectory(root);
  into["git_dir"] = util::JsonValue(git_dir.has_value() ? git_dir->string() : std::string());
}

void RemoteServer::PublishGitStatus(ServedTree& tree) {
  if (tree.closing.load()) {
    return;
  }
  const project::GitRepository repo(tree.tree.root(), platform::LocalProcessLauncher());
  const std::vector<std::string> arguments(project::kGitStatusArguments.begin(),
                                           project::kGitStatusArguments.end());
  project::GitRepository::CommandResult result;
  std::uint64_t generation = 0;
  for (int attempt = 0; attempt < 4; ++attempt) {
    generation = git_generation_.load();
    result = repo.Execute(arguments, false);
    if (git_generation_.load() == generation || tree.closing.load()) {
      break;
    }
  }
  if (tree.closing.load()) {
    return;
  }
  util::JsonObject params;
  params["root"] = util::JsonValue(tree.tree.root().string());
  params["generation"] = util::JsonValue(static_cast<std::int64_t>(generation));
  AddGitMetadata(tree.tree.root().string(), params);
  const bool unchanged = tree.git_last_pushed.has_value() &&
                         tree.git_last_pushed->exit_code == result.exit_code &&
                         tree.git_last_pushed->truncated == result.truncated &&
                         tree.git_last_pushed->output == result.output;
  if (unchanged) {
    params["unchanged"] = util::JsonValue(true);
  } else {
    params["exit_code"] = util::JsonValue(static_cast<std::int64_t>(result.exit_code));
    params["output"] = util::JsonValue(result.output);
    params["truncated"] = util::JsonValue(result.truncated);
    tree.git_last_pushed = project::GitStatusOutput{
        .exit_code = result.exit_code, .output = std::move(result.output), .truncated = result.truncated};
  }
  std::vector<std::uint64_t> subscribers;
  {
    std::lock_guard publish(tree.publish_mutex);
    subscribers.assign(tree.git_subscribers.begin(), tree.git_subscribers.end());
  }
  const util::JsonValue payload(std::move(params));
  for (const std::uint64_t connection_id : subscribers) {
    WithPeer(connection_id, [&](remote::RemotePeer& peer) {
      peer.Notify("git/status", payload, remote::Lane::Bulk);
    });
  }
}

void RemoteServer::PublishWatchBatch(ServedTree& tree) {
  ScheduleGitStatus(tree);
  const std::vector<project::RepositoryChange> repository = tree.published_head.SampleChanges();
  const bool head_moved =
      std::any_of(repository.begin(), repository.end(), [](const project::RepositoryChange& change) {
        return change.kind == project::RepositoryChangeKind::HeadChanged;
      });
  {
    std::lock_guard publish(tree.publish_mutex);
    const bool anyone_primed =
        std::any_of(tree.subscribers.begin(), tree.subscribers.end(),
                    [](const auto& entry) { return entry.second.last_sent != nullptr; });
    if (!anyone_primed) {
      return;
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
  if (tree.tree.last_update_was_full() && tree.watch) {
    tree.watch->SetPolledPaths(tree.tree.UnwatchedMembers());
  }
  std::shared_ptr<const std::vector<remote::ManifestRow>> current = std::move(manifest->rows);
  std::map<const std::vector<remote::ManifestRow>*, std::vector<std::string>> frames_for;
  std::vector<std::pair<std::uint64_t, const std::vector<remote::ManifestRow>*>> sends;
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
      if (frames.empty() && !head_moved) {
        continue;
      }
      WithPeer(connection_id, [&](remote::RemotePeer& peer) {
        if (head_moved) {
          util::JsonObject moved;
          moved["manifest"] = util::JsonValue(static_cast<std::int64_t>(manifest->id));
          peer.Notify(remote::method::kWatchMoved, util::JsonValue(std::move(moved)), remote::Lane::Bulk);
        }
        for (const std::string& frame : frames) {
          peer.SendContent(remote::FrameType::WatchDelta, manifest->id, frame, remote::Lane::Bulk);
        }
      });
    }
  }
}

namespace {

template <typename Send, typename Read>
util::JsonValue StreamObject(std::size_t index, const Send& send, const Read& read,
                             std::string* kept = nullptr, std::size_t keep_limit = 0) {
  std::string prefix;
  util::PutVarint(prefix, index);
  std::string frame = prefix;
  const auto flush = [&]() {
    if (frame.size() > prefix.size()) {
      send(frame);
    }
    frame = prefix;
  };
  bool keeping = kept != nullptr;
  const remote::FileOpResult result = read([&](std::string_view chunk) {
    if (keeping) {
      if (kept->size() + chunk.size() > keep_limit) {
        keeping = false;
        kept->clear();
        kept->shrink_to_fit();
      } else {
        kept->append(chunk);
      }
    }
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
  if (kept != nullptr && (!keeping || !result.ok())) {
    kept->clear();
  }
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

}

std::filesystem::path RemoteServer::UploadsDirectory() const {
  if (!config_.socket_path.empty()) {
    return config_.socket_path.parent_path() / "uploads";
  }
  return std::filesystem::temp_directory_path() / ("microide-uploads-" + std::to_string(::geteuid()));
}

std::filesystem::path RemoteServer::WorkspaceStateDirectory(const std::filesystem::path& root) const {
  const std::filesystem::path home =
      !config_.socket_path.empty()
          ? config_.socket_path.parent_path() / "workspace-state"
          : std::filesystem::temp_directory_path() / ("microide-state-" + std::to_string(::geteuid()));
  return home / util::HashContent(root.lexically_normal().string()).Hex().substr(0, 32);
}

void RemoteServer::InstallFileHandlers(Connection& connection) {
  remote::RemotePeer& peer = connection.peer;
  peer.OnRequest(remote::method::kObjectFetch,
                 [this, &connection](std::uint64_t id, const util::JsonValue& params) {
                   HandleObjectFetch(connection, id, params);
                 });

  peer.OnRequest(remote::method::kFileRead, [this, &connection](std::uint64_t id,
                                                                const util::JsonValue& params) {
    const std::shared_ptr<ServedTree> served = TreeOf(connection);
    const util::JsonValue& path = params["path"];
    if (!served || !path.IsString() || path.AsString().empty() || path.AsString().front() != '/') {
      ReplyInvalidParams(connection.peer, id, served != nullptr,
                         "file/read needs an absolute path");
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
      ReplyInvalidParams(connection.peer, id, served != nullptr,
                         "file/write needs a path and an expect");
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
      const std::uint64_t manifest = tree->tree.NewestManifestId();
      WithPeer(connection_id, [&](remote::RemotePeer& peer) {
        if (result.status == remote::FileOpResult::Status::Error) {
          peer.ReplyError(id, result.blocked ? remote::kErrorPathRefused : remote::kErrorInvalidParams,
                          result.error);
        } else {
          peer.Reply(id, OpResultJson(result, manifest));
        }
      });
    });
  });

  peer.OnRequest(remote::method::kFileUpload,
                 [this, &connection](std::uint64_t id, const util::JsonValue& params) {
                   HandleFileUpload(connection, id, params);
                 });

  const auto on_state = [this, &connection](bool put) {
    return [this, &connection, put](std::uint64_t id, const util::JsonValue& params) {
      const std::shared_ptr<ServedTree> served = TreeOf(connection);
      const std::string key = params["key"].AsString();
      const util::JsonValue& text = params["text"];
      if (!served || !remote::method::IsWorkspaceStateKey(key) ||
          (put && (!text.IsString() || text.AsString().size() > remote::method::kMaxWorkspaceStateBytes))) {
        ReplyInvalidParams(connection.peer, id, served != nullptr,
                           "a workspace state entry needs a key of [a-z0-9-] and a text of at most 512 KiB");
        return;
      }
      ServedTree* tree = served.get();
      const std::uint64_t connection_id = connection.id;
      const std::filesystem::path directory = WorkspaceStateDirectory(tree->tree.root());
      tree->io_queue.Post([this, connection_id, id, put, path = directory / key,
                           text = put ? text.AsString() : std::string()]() {
        util::JsonObject reply;
        std::string error;
        const bool usable = platform::EnsurePrivateDirectory(path.parent_path().parent_path(), &error) &&
                            platform::EnsurePrivateDirectory(path.parent_path(), &error);
        if (put) {
          std::error_code ec;
          if (!usable) {
          } else if (!util::WriteTextFileAtomically(path, text, &ec)) {
            error = "cannot write " + path.string() + ": " + ec.message();
          } else {
            ::chmod(path.c_str(), 0600);
          }
        } else if (!usable) {
          error.clear();
          reply["missing"] = util::JsonValue(true);
        } else if (std::optional<std::string> held = util::ReadTextFile(path);
                   held.has_value() && held->size() <= remote::method::kMaxWorkspaceStateBytes) {
          reply["text"] = util::JsonValue(std::move(*held));
        } else {
          reply["missing"] = util::JsonValue(true);
        }
        WithPeer(connection_id, [&](remote::RemotePeer& peer) {
          if (!error.empty()) {
            peer.ReplyError(id, remote::kErrorInvalidParams, error);
          } else {
            peer.Reply(id, util::JsonValue(std::move(reply)));
          }
        });
      });
    };
  };
  peer.OnRequest(remote::method::kStatePut, on_state(true));
  peer.OnRequest(remote::method::kStateGet, on_state(false));

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
      ReplyInvalidParams(connection.peer, id, served != nullptr,
                         "fs/op needs a known op, a path and a valid expect");
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


void RemoteServer::HandleObjectFetch(Connection& connection, std::uint64_t id,
                                     const util::JsonValue& params) {
  const std::shared_ptr<ServedTree> served = TreeOf(connection);
  const util::JsonValue& objects = params["objects"];
  if (!served || !objects.IsArray() || objects.AsArray().size() > 4096) {
    ReplyInvalidParams(connection.peer, id, served != nullptr,
                       "objects must be an array of at most 4096");
    return;
  }
  std::vector<std::string> paths;
  std::vector<std::string> bases;
  for (const util::JsonValue& object : objects.AsArray()) {
    paths.push_back(object["path"].AsString());
    bases.push_back(object["base"].AsString());
  }
  const std::int64_t asked_max = params["max_bytes"].AsInt(0);
  const std::uint64_t max_bytes =
      asked_max > 0 ? std::min<std::uint64_t>(static_cast<std::uint64_t>(asked_max),
                                              remote::RemoteWorkspace::kMaxStreamedObjectBytes)
                    : kMaxObjectBytes;
  const remote::Lane lane = params["bulk"].AsBool(false) ? remote::Lane::Bulk
                                                         : remote::Lane::Interactive;
  ServedTree* tree = served.get();
  const std::uint64_t connection_id = connection.id;
  tree->io_queue.Post([this, tree, connection_id, id, paths = std::move(paths),
                       bases = std::move(bases), max_bytes, lane]() {
    util::JsonArray results;
    std::uint64_t sent_bytes = 0;
    for (std::size_t index = 0; index < paths.size(); ++index) {
      if (tree->closing.load() || !RequestLive(connection_id, id)) {
        WithPeer(connection_id, [&](remote::RemotePeer& peer) {
          peer.ReplyError(id, remote::kErrorCancelled, "cancelled");
        });
        return;
      }
      const auto send = [&](std::string_view frame) {
        sent_bytes += frame.size();
        WithPeer(connection_id, [&](remote::RemotePeer& peer) {
          peer.SendContent(remote::FrameType::ObjectData, id, frame, lane);
        });
      };
      if (!bases[index].empty()) {
        std::string content;
        const remote::FileOpResult read = remote::ReadTreeFile(
            tree->tree.root(), paths[index], kMaxDeltaBaseBytes,
            [&](std::string_view chunk) { content.append(chunk); });
        if (read.ok()) {
          const std::string hash = read.current->Hex();
          const bool unchanged = hash == bases[index];
          std::optional<std::string> delta;
          if (!unchanged) {
            std::optional<std::string> base = tree->recent_objects.Get(bases[index]);
            if (!base.has_value() && content.size() >= kIndexBaseMinBytes) {
              base = IndexBlobWithHash(tree->tree.root(), paths[index], bases[index]);
            }
            if (base.has_value()) {
              delta = util::ZstdDelta(*base, content);
            }
          }
          util::JsonValue entry;
          if (unchanged || (delta.has_value() && delta->size() < content.size() - content.size() / 8)) {
            util::JsonObject object;
            object["hash"] = util::JsonValue(hash);
            object["delta"] = util::JsonValue(true);
            (void)StreamObject(index, send, [&](const std::function<void(std::string_view)>& sink) {
              if (!unchanged) {
                sink(*delta);
              }
              return remote::FileOpResult{};
            });
            entry = util::JsonValue(std::move(object));
          } else {
            entry = StreamObject(index, send, [&](const std::function<void(std::string_view)>& sink) {
              sink(content);
              return read;
            });
          }
          tree->recent_objects.Put(hash, std::move(content));
          results.push_back(std::move(entry));
          continue;
        }
      }
      results.push_back(StreamObject(index, send, [&](const std::function<void(std::string_view)>& sink) {
        return remote::ReadTreeFile(tree->tree.root(), paths[index], max_bytes, sink);
      }));
    }
    util::JsonObject reply;
    reply["objects"] = util::JsonValue(std::move(results));
    WithPeer(connection_id, [&](remote::RemotePeer& peer) {
      peer.Reply(id, util::JsonValue(std::move(reply)), lane);
    });
    tree->served_since_trim.fetch_add(sent_bytes);
    tree->io_queue.PostLatest("heap-trim", [tree]() {
      if (tree->served_since_trim.load() >= kTrimAfterServedBytes) {
        tree->served_since_trim.store(0);
        util::ReleaseFreedHeap();
      }
    });
  });
}

void RemoteServer::HandleFileUpload(Connection& connection, std::uint64_t id,
                                    const util::JsonValue& params) {
  std::string content;
  if (const auto upload = connection.uploads.find(id); upload != connection.uploads.end()) {
    content = std::move(upload->second);
    connection.upload_bytes -= content.size();
    connection.uploads.erase(upload);
  }
  const std::shared_ptr<ServedTree> served = TreeOf(connection);
  const std::string name = params["name"].AsString();
  constexpr std::size_t kMaxNameBytes = 255;
  const bool named = !name.empty() && name != "." && name != ".." &&
                     name.find_first_of(std::string_view("/\0", 2)) == std::string::npos;
  if (!served || !named) {
    ReplyInvalidParams(connection.peer, id, served != nullptr, "file/upload needs a file name");
    return;
  }
  if (name.size() > kMaxNameBytes) {
    ReplyInvalidParams(connection.peer, id, true,
                       "the file's name is " + std::to_string(name.size()) +
                           " bytes; a name on the host can be at most 255");
    return;
  }
  const std::filesystem::path root = UploadsDirectory();
  ServedTree* tree = served.get();
  const std::uint64_t connection_id = connection.id;
  tree->io_queue.Post([this, root, connection_id, id, name, content = std::move(content)]() {
    std::string error;
    const std::optional<std::filesystem::path> path = platform::StoreTransientFile(root, name, content, &error);
    WithPeer(connection_id, [&](remote::RemotePeer& peer) {
      if (!path.has_value()) {
        peer.ReplyError(id, remote::kErrorInvalidParams, error);
        return;
      }
      util::JsonObject reply;
      reply["path"] = util::JsonValue(path->string());
      peer.Reply(id, util::JsonValue(std::move(reply)));
    });
  });
}

}
