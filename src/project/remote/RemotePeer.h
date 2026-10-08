#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>

#include "project/remote/LinkHeartbeat.h"
#include "project/remote/RemoteFrame.h"
#include "project/remote/RemoteFrameTransport.h"
#include "util/JsonValue.h"

namespace microide::project::remote {

// Requests, responses, notifications and content over a RemoteFrameTransport —
// the same layer on both ends: the client's RemoteServerClient and the server's
// workspace both talk through one of these.
//
// Built in: `link/ping` is answered automatically, a peer started with pings on
// sends them and declares the link dead after three misses (LinkHeartbeat), and
// `op/cancel` marks an in-flight incoming request cancelled (IsCancelled) and
// calls the on_cancel hook so a streaming handler stops.
//
// Threading: handlers and callbacks run on the transport's I/O thread. Register
// every handler before Start; Request, Notify, Reply, SendContent and Cancel are
// callable from any thread.
class RemotePeer {
 public:
  struct RpcError {
    std::int64_t code = 0;
    std::string message;
  };
  // Exactly one of the two is set.
  using ResponseHandler =
      std::function<void(std::optional<util::JsonValue> result, std::optional<RpcError> error)>;
  using RequestHandler = std::function<void(std::uint64_t id, const util::JsonValue& params)>;
  using NotificationHandler = std::function<void(std::uint64_t id, const util::JsonValue& params)>;
  using ContentHandler = std::function<void(FrameType type, std::uint64_t handle, std::string bytes)>;

  struct Options {
    RemoteFrameTransport::Options transport;
    // Send link/ping on this cadence and fail the connection after LinkHeartbeat's
    // misses. The client turns it on; the server only answers.
    bool send_pings = false;
    LinkHeartbeat::Config heartbeat;
  };

  RemotePeer() = default;
  ~RemotePeer();
  RemotePeer(const RemotePeer&) = delete;
  RemotePeer& operator=(const RemotePeer&) = delete;

  void OnRequest(std::string_view method, RequestHandler handler);
  void OnNotification(std::string_view method, NotificationHandler handler);
  void OnContent(ContentHandler handler);
  void OnCancel(std::function<void(std::uint64_t id)> handler);
  void OnClosed(std::function<void(std::string_view reason)> handler);

  [[nodiscard]] bool Start(int read_fd, int write_fd, Options options);
  void Stop();
  // End the connection with `reason` (reported through OnClosed).
  void Fail(std::string_view reason) { transport_.Fail(reason); }
  bool closed() const { return transport_.closed(); }
  // Wait up to `timeout` for what was sent so far to be written (RemoteFrameTransport::Flush).
  bool Flush(std::chrono::milliseconds timeout) { return transport_.Flush(timeout); }

  // Send a request; `handler` runs once with its result or error — including when
  // the connection closes first (a kErrorCancelled-coded "connection closed").
  // Returns the request id, 0 when the request could not be sent (handler not run).
  std::uint64_t Request(std::string_view method, const util::JsonValue& params, Lane lane,
                        ResponseHandler handler);
  // As above, with `before_send(id)` run after the id is allocated and before the
  // request can reach the peer — where a caller registers a route for content
  // frames that answer it, which may otherwise arrive before Request returns.
  std::uint64_t Request(std::string_view method, const util::JsonValue& params, Lane lane,
                        ResponseHandler handler,
                        const std::function<void(std::uint64_t id)>& before_send);
  // Ask the peer to stop request `id`; the local handler still runs once, with the
  // peer's terminal reply.
  bool Cancel(std::uint64_t id);
  bool Notify(std::string_view method, const util::JsonValue& params,
              Lane lane = Lane::Interactive, std::uint64_t id = 0);
  bool Reply(std::uint64_t id, const util::JsonValue& result, Lane lane = Lane::Interactive);
  bool ReplyError(std::uint64_t id, std::int64_t code, std::string_view message);
  bool SendContent(FrameType type, std::uint64_t handle, std::string_view bytes,
                   Lane lane = Lane::Interactive);

  // Incoming request `id` was cancelled by the peer (op/cancel) and not yet replied.
  bool IsCancelled(std::uint64_t id) const;
  std::optional<std::int64_t> rtt_ms() const;
  std::size_t bulk_window() const { return transport_.bulk_window(); }

 private:
  void HandleFrame(Frame frame);
  void HandleClosed(std::string_view reason, bool report);
  void HandleTick();
  bool SendControl(FrameType type, Lane lane, std::uint64_t id, const util::JsonValue& body);

  std::map<std::string, RequestHandler, std::less<>> request_handlers_;
  std::map<std::string, NotificationHandler, std::less<>> notification_handlers_;
  ContentHandler content_handler_;
  std::function<void(std::uint64_t)> cancel_handler_;
  std::function<void(std::string_view)> closed_handler_;

  RemoteFrameTransport transport_;
  bool send_pings_ = false;
  std::chrono::steady_clock::time_point epoch_ = std::chrono::steady_clock::now();

  mutable std::mutex mutex_;  // guards everything below
  std::uint64_t next_request_id_ = 1;
  std::map<std::uint64_t, ResponseHandler> pending_;
  std::unordered_set<std::uint64_t> incoming_in_flight_;
  std::unordered_set<std::uint64_t> incoming_cancelled_;
  LinkHeartbeat heartbeat_;
  bool closed_ = false;
};

}  // namespace microide::project::remote
