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

class RemotePeer {
 public:
  struct RpcError {
    std::int64_t code = 0;
    std::string message;
  };
  using ResponseHandler =
      std::function<void(std::optional<util::JsonValue> result, std::optional<RpcError> error)>;
  using RequestHandler = std::function<void(std::uint64_t id, const util::JsonValue& params)>;
  using NotificationHandler = std::function<void(std::uint64_t id, const util::JsonValue& params)>;
  using ContentHandler = std::function<void(FrameType type, std::uint64_t handle, std::string bytes)>;

  struct Options {
    RemoteFrameTransport::Options transport;
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
  void OnDispatch(std::function<void(std::string_view method)> handler);

  [[nodiscard]] bool Start(int read_fd, int write_fd, Options options);
  void Stop();
  void Fail(std::string_view reason) { transport_.Fail(reason); }
  bool closed() const { return transport_.closed(); }
  bool OnIoThread() const { return transport_.OnIoThread(); }
  bool Flush(std::chrono::milliseconds stall, std::chrono::milliseconds limit) {
    return transport_.Flush(stall, limit);
  }
  bool FlushForTeardown() { return Flush(std::chrono::milliseconds(500), std::chrono::seconds(5)); }

  std::uint64_t Request(std::string_view method, const util::JsonValue& params, Lane lane,
                        ResponseHandler handler);
  std::uint64_t Request(std::string_view method, const util::JsonValue& params, Lane lane,
                        ResponseHandler handler,
                        const std::function<void(std::uint64_t id)>& before_send);
  bool Cancel(std::uint64_t id);
  bool Notify(std::string_view method, const util::JsonValue& params,
              Lane lane = Lane::Interactive, std::uint64_t id = 0);
  bool Reply(std::uint64_t id, const util::JsonValue& result, Lane lane = Lane::Interactive);
  bool ReplyError(std::uint64_t id, std::int64_t code, std::string_view message);
  bool SendContent(FrameType type, std::uint64_t handle, std::string_view bytes,
                   Lane lane = Lane::Interactive);

  bool IsCancelled(std::uint64_t id) const;
  std::size_t pending_request_count() const;
  std::optional<std::int64_t> rtt_ms() const;
  std::size_t bulk_window() const { return transport_.bulk_window(); }
  void SetFixedBulkWindow(std::size_t bytes) { transport_.SetFixedBulkWindow(bytes); }

 private:
  void HandleFrame(Frame frame);
  void HandleClosed(std::string_view reason, bool report);
  std::chrono::milliseconds HandleTick();
  bool SendControl(FrameType type, Lane lane, std::uint64_t id, const util::JsonValue& body);

  std::map<std::string, RequestHandler, std::less<>> request_handlers_;
  std::map<std::string, NotificationHandler, std::less<>> notification_handlers_;
  ContentHandler content_handler_;
  std::function<void(std::uint64_t)> cancel_handler_;
  std::function<void(std::string_view)> closed_handler_;
  std::function<void(std::string_view)> dispatch_handler_;

  RemoteFrameTransport transport_;
  bool send_pings_ = false;
  std::chrono::steady_clock::time_point epoch_ = std::chrono::steady_clock::now();

  mutable std::mutex mutex_;
  std::uint64_t next_request_id_ = 1;
  struct PendingRequest {
    ResponseHandler handler;
    bool heartbeat = false;
    bool sent = false;
  };
  std::map<std::uint64_t, PendingRequest> pending_;
  std::unordered_set<std::uint64_t> incoming_in_flight_;
  std::unordered_set<std::uint64_t> incoming_cancelled_;
  LinkHeartbeat heartbeat_;
  bool closed_ = false;

  std::uint64_t SendRequest(std::string_view method, const util::JsonValue& params, Lane lane,
                            ResponseHandler handler,
                            const std::function<void(std::uint64_t id)>& before_send, bool heartbeat);
};

}
