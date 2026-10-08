#include "project/remote/RemotePeer.h"

#include <utility>
#include <vector>

#include "project/remote/RemoteProtocol.h"

namespace microide::project::remote {
namespace {

// Bounds on what one peer may hold the other to.
constexpr std::size_t kMaxPendingIncoming = 4096;

}  // namespace

RemotePeer::~RemotePeer() {
  Stop();
}

void RemotePeer::OnRequest(std::string_view method, RequestHandler handler) {
  request_handlers_[std::string(method)] = std::move(handler);
}

void RemotePeer::OnNotification(std::string_view method, NotificationHandler handler) {
  notification_handlers_[std::string(method)] = std::move(handler);
}

void RemotePeer::OnContent(ContentHandler handler) {
  content_handler_ = std::move(handler);
}

void RemotePeer::OnCancel(std::function<void(std::uint64_t)> handler) {
  cancel_handler_ = std::move(handler);
}

void RemotePeer::OnClosed(std::function<void(std::string_view)> handler) {
  closed_handler_ = std::move(handler);
}

bool RemotePeer::Start(int read_fd, int write_fd, Options options) {
  send_pings_ = options.send_pings;
  {
    std::lock_guard lock(mutex_);
    heartbeat_ = LinkHeartbeat(options.heartbeat);
  }
  RemoteFrameTransport::Options transport_options = options.transport;
  if (send_pings_) {
    // Tick at a quarter of the ping interval: the heartbeat decides when one is
    // due, and a dead link is noticed within a quarter interval of its deadline.
    transport_options.tick_interval =
        std::chrono::milliseconds(std::max<std::int64_t>(options.heartbeat.interval_ms / 4, 1));
  }
  return transport_.Start(read_fd, write_fd,
                          RemoteFrameTransport::Callbacks{
                              .on_frame = [this](Frame frame) { HandleFrame(std::move(frame)); },
                              .on_closed = [this](std::string_view reason) {
                                HandleClosed(reason, /*report=*/true);
                              },
                              .on_tick = [this]() { HandleTick(); },
                          },
                          transport_options);
}

void RemotePeer::Stop() {
  transport_.Stop();
  // A request still pending when the connection goes away gets its one answer.
  // The closed hook is not run: Stop is the owner's own decision, usually made
  // while it is being torn down.
  HandleClosed("connection closed", /*report=*/false);
}

bool RemotePeer::SendControl(FrameType type, Lane lane, std::uint64_t id,
                             const util::JsonValue& body) {
  return transport_.Send(type, lane, id, util::SerializeJson(body));
}

std::uint64_t RemotePeer::Request(std::string_view method, const util::JsonValue& params,
                                  Lane lane, ResponseHandler handler) {
  std::uint64_t id = 0;
  {
    std::lock_guard lock(mutex_);
    if (closed_) {
      return 0;
    }
    id = next_request_id_++;
    pending_[id] = std::move(handler);
  }
  util::JsonObject body;
  body["method"] = util::JsonValue(std::string(method));
  body["params"] = params;
  if (!SendControl(FrameType::Request, lane, id, util::JsonValue(std::move(body)))) {
    std::lock_guard lock(mutex_);
    pending_.erase(id);
    return 0;
  }
  return id;
}

bool RemotePeer::Cancel(std::uint64_t id) {
  {
    std::lock_guard lock(mutex_);
    if (pending_.find(id) == pending_.end()) {
      return false;
    }
  }
  util::JsonObject params;
  params["id"] = util::JsonValue(static_cast<std::int64_t>(id));
  return Notify(method::kOpCancel, util::JsonValue(std::move(params)));
}

bool RemotePeer::Notify(std::string_view method_name, const util::JsonValue& params, Lane lane,
                        std::uint64_t id) {
  util::JsonObject body;
  body["method"] = util::JsonValue(std::string(method_name));
  body["params"] = params;
  return SendControl(FrameType::Notification, lane, id, util::JsonValue(std::move(body)));
}

bool RemotePeer::Reply(std::uint64_t id, const util::JsonValue& result, Lane lane) {
  {
    std::lock_guard lock(mutex_);
    incoming_in_flight_.erase(id);
    incoming_cancelled_.erase(id);
  }
  util::JsonObject body;
  body["result"] = result;
  return SendControl(FrameType::Response, lane, id, util::JsonValue(std::move(body)));
}

bool RemotePeer::ReplyError(std::uint64_t id, std::int64_t code, std::string_view message) {
  {
    std::lock_guard lock(mutex_);
    incoming_in_flight_.erase(id);
    incoming_cancelled_.erase(id);
  }
  util::JsonObject error;
  error["code"] = util::JsonValue(code);
  error["message"] = util::JsonValue(std::string(message));
  util::JsonObject body;
  body["error"] = util::JsonValue(std::move(error));
  return SendControl(FrameType::Response, Lane::Interactive, id, util::JsonValue(std::move(body)));
}

bool RemotePeer::SendContent(FrameType type, std::uint64_t handle, std::string_view bytes,
                             Lane lane) {
  if (!IsContentFrameType(type)) {
    return false;
  }
  return transport_.Send(type, lane, handle, bytes);
}

bool RemotePeer::IsCancelled(std::uint64_t id) const {
  std::lock_guard lock(mutex_);
  return incoming_cancelled_.count(id) != 0;
}

std::optional<std::int64_t> RemotePeer::rtt_ms() const {
  std::lock_guard lock(mutex_);
  return heartbeat_.rtt_ms();
}

void RemotePeer::HandleFrame(Frame frame) {
  if (IsContentFrameType(frame.type)) {
    if (content_handler_) {
      content_handler_(frame.type, frame.id, std::move(frame.payload));
    }
    return;
  }
  // Everything below is a JSON control body from an untrusted peer (§ 6.9).
  std::optional<util::JsonValue> body = util::ParseJson(frame.payload);
  const bool well_formed = body.has_value() && body->IsObject();
  switch (frame.type) {
    case FrameType::Request: {
      if (!well_formed || !(*body)["method"].IsString()) {
        ReplyError(frame.id, kErrorProtocol, "malformed request");
        return;
      }
      const std::string& name = (*body)["method"].AsString();
      if (name == method::kLinkPing) {
        Reply(frame.id, (*body)["params"]);
        return;
      }
      const auto handler = request_handlers_.find(name);
      if (handler == request_handlers_.end()) {
        ReplyError(frame.id, kErrorUnknownMethod, "unknown method " + name);
        return;
      }
      {
        std::lock_guard lock(mutex_);
        if (incoming_in_flight_.size() >= kMaxPendingIncoming) {
          // A peer that never waits for answers cannot pin unbounded state here.
          ReplyError(frame.id, kErrorProtocol, "too many requests in flight");
          return;
        }
        incoming_in_flight_.insert(frame.id);
      }
      handler->second(frame.id, (*body)["params"]);
      return;
    }
    case FrameType::Notification: {
      if (!well_formed || !(*body)["method"].IsString()) {
        return;  // nobody to answer; drop it
      }
      const std::string& name = (*body)["method"].AsString();
      if (name == method::kOpCancel) {
        const std::int64_t id = (*body)["params"]["id"].AsInt(-1);
        if (id <= 0) {
          return;
        }
        bool live = false;
        {
          std::lock_guard lock(mutex_);
          live = incoming_in_flight_.count(static_cast<std::uint64_t>(id)) != 0;
          if (live) {
            incoming_cancelled_.insert(static_cast<std::uint64_t>(id));
          }
        }
        if (live && cancel_handler_) {
          cancel_handler_(static_cast<std::uint64_t>(id));
        }
        return;
      }
      const auto handler = notification_handlers_.find(name);
      if (handler != notification_handlers_.end()) {
        handler->second(frame.id, (*body)["params"]);
      }
      return;
    }
    case FrameType::Response: {
      ResponseHandler handler;
      {
        std::lock_guard lock(mutex_);
        const auto pending = pending_.find(frame.id);
        if (pending == pending_.end()) {
          return;  // an answer to nothing we asked (or already answered)
        }
        handler = std::move(pending->second);
        pending_.erase(pending);
      }
      if (!well_formed) {
        handler(std::nullopt, RpcError{kErrorProtocol, "malformed response"});
        return;
      }
      if (body->HasKey("error")) {
        const util::JsonValue& error = (*body)["error"];
        handler(std::nullopt, RpcError{error["code"].AsInt(kErrorProtocol),
                                       error["message"].AsString()});
        return;
      }
      handler((*body)["result"], std::nullopt);
      return;
    }
    default:
      return;
  }
}

void RemotePeer::HandleClosed(std::string_view reason, bool report) {
  std::map<std::uint64_t, ResponseHandler> orphaned;
  bool first = false;
  {
    std::lock_guard lock(mutex_);
    first = !closed_;
    closed_ = true;
    orphaned.swap(pending_);
  }
  for (auto& [id, handler] : orphaned) {
    (void)id;
    handler(std::nullopt, RpcError{kErrorCancelled, "connection closed: " + std::string(reason)});
  }
  if (first && report && closed_handler_) {
    closed_handler_(reason);
  }
}

void RemotePeer::HandleTick() {
  if (!send_pings_) {
    return;
  }
  const std::int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - epoch_)
                                  .count();
  std::optional<std::int64_t> stamp;
  bool dead = false;
  {
    std::lock_guard lock(mutex_);
    stamp = heartbeat_.Tick(now_ms);
    dead = heartbeat_.dead();
  }
  if (dead) {
    transport_.Fail("link dead: no answer to three pings");
    return;
  }
  if (!stamp.has_value()) {
    return;
  }
  util::JsonObject params;
  params["clock"] = util::JsonValue(*stamp);
  const std::int64_t sent = *stamp;
  Request(method::kLinkPing, util::JsonValue(std::move(params)), Lane::Interactive,
          [this, sent](std::optional<util::JsonValue> result, std::optional<RpcError> error) {
            if (!result.has_value() || error.has_value()) {
              return;
            }
            const std::int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now() - epoch_)
                                         .count();
            std::lock_guard lock(mutex_);
            heartbeat_.OnPong(sent, now);
          });
}

}  // namespace microide::project::remote
