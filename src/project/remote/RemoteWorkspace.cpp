#include "project/remote/RemoteWorkspace.h"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <utility>

#include "project/remote/RemoteConnection.h"
#include "project/remote/RemoteProtocol.h"
#include "project/remote/RemoteServerClient.h"
#include "util/ByteCodec.h"

namespace microide::project::remote {
namespace {

// Run an asynchronous call to completion on the calling thread (never the
// connection's I/O thread, which is the one that would complete it). On timeout the
// request is cancelled and `on_timeout` returned; a late completion lands in the
// shared state, never on a dead stack frame.
template <typename Result, typename Start>
Result Await(Start&& start, const std::shared_ptr<RemoteConnection>& connection,
             std::chrono::milliseconds timeout, Result on_timeout) {
  struct Wait {
    std::mutex mutex;
    std::condition_variable cv;
    std::optional<Result> result;
  };
  auto wait = std::make_shared<Wait>();
  const std::uint64_t id = start([wait](Result result) {
    std::lock_guard lock(wait->mutex);
    wait->result = std::move(result);
    wait->cv.notify_all();
  });
  std::unique_lock lock(wait->mutex);
  if (!wait->cv.wait_for(lock, timeout, [&] { return wait->result.has_value(); })) {
    lock.unlock();
    if (const auto client = connection->client(); client && id != 0) {
      client->peer().Cancel(id);
    }
    return on_timeout;
  }
  return std::move(*wait->result);
}

RemoteWorkspace::WriteResult WriteResultFrom(std::optional<util::JsonValue> result,
                                             std::optional<RemotePeer::RpcError> error) {
  RemoteWorkspace::WriteResult out;
  if (error.has_value() || !result.has_value()) {
    out.error = error.has_value() ? error->message : "no answer";
    return out;
  }
  const util::JsonValue& hash = (*result)["hash"];
  const util::JsonValue& current = (*result)["current"];
  if ((*result)["conflict"].AsBool(false)) {
    out.status = RemoteWorkspace::WriteResult::Status::Conflict;
    if (current.IsString()) {
      out.hash = util::ContentHash::FromHex(current.AsString());
    }
    return out;
  }
  out.status = RemoteWorkspace::WriteResult::Status::Ok;
  if (hash.IsString()) {
    out.hash = util::ContentHash::FromHex(hash.AsString());
  }
  return out;
}

constexpr const char* kNotConnected = "not connected to the host";

}  // namespace

std::shared_ptr<RemoteServerClient> RemoteWorkspace::Client() const {
  return connection_ ? connection_->client() : nullptr;
}

std::uint64_t RemoteWorkspace::FetchManifest(ManifestDone done) {
  struct State {
    Manifest manifest;
    bool malformed = false;
  };
  auto state = std::make_shared<State>();
  const std::shared_ptr<RemoteServerClient> client = Client();
  if (!client) {
    done(std::nullopt, kNotConnected);
    return 0;
  }
  RemoteServerClient* raw = client.get();  // the stream's own frames: it outlives them
  const std::uint64_t id = client->RequestStream(
      method::kTreeManifest, util::JsonValue(util::JsonObject{}), Lane::Bulk,
      [state, raw](FrameType, std::string bytes) {
        if (state->malformed) {
          return;
        }
        if (!DecodeManifestRows(bytes, state->manifest.rows) ||
            state->manifest.rows.size() > kMaxManifestRows) {
          state->malformed = true;
          raw->peer().Fail("the server sent malformed manifest rows");
        }
      },
      [state, done](std::optional<util::JsonValue> result,
                    std::optional<RemotePeer::RpcError> error) {
        if (error.has_value() || !result.has_value()) {
          done(std::nullopt, error.has_value() ? error->message : "no answer");
          return;
        }
        const std::int64_t rows = (*result)["rows"].AsInt(-1);
        const std::int64_t manifest_id = (*result)["manifest_id"].AsInt(0);
        if (state->malformed || manifest_id <= 0 ||
            rows != static_cast<std::int64_t>(state->manifest.rows.size())) {
          done(std::nullopt, "the server's manifest did not match its own row count");
          return;
        }
        // Chunks are each sorted; across chunks the order must hold too, or a
        // later lookup by binary search lies.
        const auto& decoded = state->manifest.rows;
        for (std::size_t i = 1; i < decoded.size(); ++i) {
          if (!(decoded[i - 1].path < decoded[i].path)) {
            done(std::nullopt, "the server's manifest is not sorted and unique");
            return;
          }
        }
        state->manifest.id = static_cast<std::uint64_t>(manifest_id);
        state->manifest.git = (*result)["git"].AsBool(false);
        done(std::move(state->manifest), {});
      });
  if (id == 0) {
    done(std::nullopt, "not connected");
  }
  return id;
}

std::optional<RemoteWorkspace::Manifest> RemoteWorkspace::FetchManifestSync(
    std::string* error, std::chrono::milliseconds timeout) {
  using Outcome = std::pair<std::optional<Manifest>, std::string>;
  Outcome outcome = Await<Outcome>(
      [&](std::function<void(Outcome)> finish) {
        return FetchManifest([finish](std::optional<Manifest> manifest, std::string why) {
          finish(Outcome(std::move(manifest), std::move(why)));
        });
      },
      connection_, timeout, Outcome(std::nullopt, "timed out waiting for the manifest"));
  if (error != nullptr) {
    *error = std::move(outcome.second);
  }
  return std::move(outcome.first);
}

bool RemoteWorkspace::SubscribeWatch(std::function<void(WatchDelta delta)> on_delta, bool* native,
                                     std::string* error) {
  const std::shared_ptr<RemoteServerClient> client = Client();
  if (!client) {
    if (error != nullptr) {
      *error = kNotConnected;
    }
    return false;
  }
  RemoteServerClient* raw = client.get();  // its own handler: it outlives the calls
  client->SetWatchHandler([raw, on_delta = std::move(on_delta)](std::uint64_t manifest_id,
                                                                std::string bytes) {
    WatchDelta delta;
    delta.manifest_id = manifest_id;
    if (!DecodeWatchDelta(bytes, delta.deleted, delta.rows)) {
      raw->peer().Fail("the server sent a malformed watch delta");
      return;
    }
    on_delta(std::move(delta));
  });
  std::string why;
  const std::optional<util::JsonValue> result =
      client->Call(method::kWatchSubscribe, util::JsonValue(util::JsonObject{}), &why);
  if (!result.has_value()) {
    if (error != nullptr) {
      *error = why;
    }
    return false;
  }
  if (native != nullptr) {
    *native = (*result)["native"].AsBool(false);
  }
  return true;
}

std::uint64_t RemoteWorkspace::FetchObjects(std::vector<std::string> paths, Lane lane,
                                            FetchDone done, std::uint64_t max_bytes) {
  struct State {
    std::vector<FetchedObject> objects;
    bool malformed = false;
  };
  auto state = std::make_shared<State>();
  util::JsonArray requested;
  state->objects.resize(paths.size());
  for (std::size_t i = 0; i < paths.size(); ++i) {
    util::JsonObject object;
    object["path"] = util::JsonValue(paths[i]);
    requested.push_back(util::JsonValue(std::move(object)));
    state->objects[i].path = std::move(paths[i]);
  }
  util::JsonObject params;
  params["objects"] = util::JsonValue(std::move(requested));
  params["bulk"] = util::JsonValue(lane == Lane::Bulk);
  if (max_bytes > 0) {
    params["max_bytes"] = util::JsonValue(static_cast<std::int64_t>(max_bytes));
  }
  const std::shared_ptr<RemoteServerClient> client = Client();
  if (!client) {
    done(std::nullopt, kNotConnected);
    return 0;
  }
  RemoteServerClient* raw = client.get();
  const std::uint64_t id = client->RequestStream(
      method::kObjectFetch, util::JsonValue(std::move(params)), lane,
      [state, raw](FrameType, std::string bytes) {
        util::ByteReader in(bytes);
        const std::uint64_t index = in.Varint();
        if (state->malformed || in.failed() || index >= state->objects.size()) {
          if (!state->malformed) {
            state->malformed = true;
            raw->peer().Fail("the server sent object data for no object");
          }
          return;
        }
        state->objects[static_cast<std::size_t>(index)].content.append(
            std::string_view(bytes).substr(bytes.size() - in.remaining()));
      },
      [state, done](std::optional<util::JsonValue> result,
                    std::optional<RemotePeer::RpcError> error) {
        if (error.has_value() || !result.has_value()) {
          done(std::nullopt, error.has_value() ? error->message : "no answer");
          return;
        }
        const util::JsonValue& answers = (*result)["objects"];
        if (state->malformed || !answers.IsArray() ||
            answers.AsArray().size() != state->objects.size()) {
          done(std::nullopt, "the server's object/fetch answer does not match the request");
          return;
        }
        for (std::size_t i = 0; i < state->objects.size(); ++i) {
          FetchedObject& object = state->objects[i];
          const util::JsonValue& answer = answers.AsArray()[i];
          if (answer["missing"].AsBool(false)) {
            object.missing = true;
            object.content.clear();
            continue;
          }
          if (answer["hash"].IsString()) {
            const auto claimed = util::ContentHash::FromHex(answer["hash"].AsString());
            // The object store is keyed by this hash: it is checked, not trusted.
            if (claimed.has_value() && util::HashContent(object.content) == *claimed) {
              object.hash = claimed;
              continue;
            }
            object.error = "the host's bytes for " + object.path + " do not match their hash";
          } else {
            object.error = answer["error"].AsString();
          }
          object.content.clear();
        }
        done(std::move(state->objects), {});
      });
  if (id == 0) {
    done(std::nullopt, "not connected");
  }
  return id;
}

std::optional<std::vector<RemoteWorkspace::FetchedObject>> RemoteWorkspace::FetchObjectsSync(
    std::vector<std::string> paths, Lane lane, std::string* error,
    std::chrono::milliseconds timeout) {
  using Outcome = std::pair<std::optional<std::vector<FetchedObject>>, std::string>;
  Outcome outcome = Await<Outcome>(
      [&](std::function<void(Outcome)> finish) {
        return FetchObjects(std::move(paths), lane,
                            [finish](std::optional<std::vector<FetchedObject>> objects,
                                     std::string why) {
                              finish(Outcome(std::move(objects), std::move(why)));
                            });
      },
      connection_, timeout, Outcome(std::nullopt, "timed out waiting for object/fetch"));
  if (error != nullptr) {
    *error = std::move(outcome.second);
  }
  return std::move(outcome.first);
}

std::uint64_t RemoteWorkspace::WriteFile(std::string path, std::string_view content,
                                         const Precondition& expect,
                                         std::optional<std::uint32_t> mode, Lane lane,
                                         WriteDone done) {
  util::JsonObject params;
  params["path"] = util::JsonValue(std::move(path));
  params["expect"] = ToJson(expect);
  if (mode.has_value()) {
    params["mode"] = util::JsonValue(static_cast<std::int64_t>(*mode));
  }
  const std::shared_ptr<RemoteServerClient> client = Client();
  if (!client) {
    done(WriteResult{.error = kNotConnected});
    return 0;
  }
  RemotePeer& peer = client->peer();
  // The content goes first, on the request's lane, tagged with the request's id:
  // it is on the wire before the request that claims it.
  const std::uint64_t id = peer.Request(
      method::kFileWrite, util::JsonValue(std::move(params)), lane,
      [done](std::optional<util::JsonValue> result, std::optional<RemotePeer::RpcError> error) {
        done(WriteResultFrom(std::move(result), std::move(error)));
      },
      [&](std::uint64_t allocated) {
        constexpr std::size_t kChunk = RemoteFrameTransport::kMaxBulkChunkBytes;
        for (std::size_t offset = 0; offset < content.size(); offset += kChunk) {
          peer.SendContent(FrameType::WriteData, allocated, content.substr(offset, kChunk), lane);
        }
      });
  if (id == 0) {
    done(WriteResult{.error = "not connected"});
  }
  return id;
}

RemoteWorkspace::WriteResult RemoteWorkspace::WriteFileSync(std::string path,
                                                            std::string_view content,
                                                            const Precondition& expect,
                                                            std::optional<std::uint32_t> mode,
                                                            std::chrono::milliseconds timeout) {
  return Await<WriteResult>(
      [&](WriteDone finish) {
        return WriteFile(std::move(path), content, expect, mode, Lane::Interactive,
                         std::move(finish));
      },
      connection_, timeout, WriteResult{.error = "timed out waiting for file/write"});
}

std::uint64_t RemoteWorkspace::ApplyTreeOp(TreeOp op, std::string path, std::string to,
                                           const Precondition& expect, WriteDone done) {
  util::JsonObject params;
  params["op"] = util::JsonValue(std::string(op == TreeOp::MakeDirectory ? "mkdir"
                                             : op == TreeOp::Rename      ? "rename"
                                                                         : "delete"));
  params["path"] = util::JsonValue(std::move(path));
  if (op == TreeOp::Rename) {
    params["to"] = util::JsonValue(std::move(to));
  }
  params["expect"] = ToJson(expect);
  const std::shared_ptr<RemoteServerClient> client = Client();
  if (!client) {
    done(WriteResult{.error = kNotConnected});
    return 0;
  }
  const std::uint64_t id = client->peer().Request(
      method::kFsOp, util::JsonValue(std::move(params)), Lane::Interactive,
      [done](std::optional<util::JsonValue> result, std::optional<RemotePeer::RpcError> error) {
        done(WriteResultFrom(std::move(result), std::move(error)));
      });
  if (id == 0) {
    done(WriteResult{.error = "not connected"});
  }
  return id;
}

RemoteWorkspace::WriteResult RemoteWorkspace::ApplyTreeOpSync(TreeOp op, std::string path,
                                                              std::string to,
                                                              const Precondition& expect,
                                                              std::chrono::milliseconds timeout) {
  return Await<WriteResult>(
      [&](WriteDone finish) {
        return ApplyTreeOp(op, std::move(path), std::move(to), expect, std::move(finish));
      },
      connection_, timeout, WriteResult{.error = "timed out waiting for fs/op"});
}

}  // namespace microide::project::remote
