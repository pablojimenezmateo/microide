#include "project/remote/RemoteWorkspace.h"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <utility>

#include "project/remote/RemoteProtocol.h"
#include "project/remote/RemoteServerClient.h"

namespace microide::project::remote {

std::uint64_t RemoteWorkspace::FetchManifest(ManifestDone done) {
  struct State {
    Manifest manifest;
    bool malformed = false;
  };
  auto state = std::make_shared<State>();
  RemoteServerClient& client = client_;
  const std::uint64_t id = client_.RequestStream(
      method::kTreeManifest, util::JsonValue(util::JsonObject{}), Lane::Bulk,
      [state, &client](FrameType, std::string bytes) {
        if (state->malformed) {
          return;
        }
        if (!DecodeManifestRows(bytes, state->manifest.rows) ||
            state->manifest.rows.size() > kMaxManifestRows) {
          state->malformed = true;
          client.peer().Fail("the server sent malformed manifest rows");
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
  struct Wait {
    std::mutex mutex;
    std::condition_variable cv;
    bool finished = false;
    std::optional<Manifest> manifest;
    std::string error;
  };
  auto wait = std::make_shared<Wait>();
  const std::uint64_t id = FetchManifest([wait](std::optional<Manifest> manifest, std::string why) {
    std::lock_guard lock(wait->mutex);
    wait->manifest = std::move(manifest);
    wait->error = std::move(why);
    wait->finished = true;
    wait->cv.notify_all();
  });
  std::unique_lock lock(wait->mutex);
  if (!wait->cv.wait_for(lock, timeout, [&] { return wait->finished; })) {
    lock.unlock();
    client_.peer().Cancel(id);
    if (error != nullptr) {
      *error = "timed out waiting for the manifest";
    }
    return std::nullopt;
  }
  if (error != nullptr) {
    *error = wait->error;
  }
  return std::move(wait->manifest);
}

}  // namespace microide::project::remote
