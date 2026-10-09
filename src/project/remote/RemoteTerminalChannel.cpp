#include "project/remote/RemoteTerminalChannel.h"

#include <algorithm>
#include <utility>

#include "project/remote/RemoteConnection.h"
#include "project/remote/RemoteProtocol.h"
#include "project/remote/RemoteServerClient.h"
#include "terminal/TerminalHostWire.h"
#include "terminal/TerminalSession.h"

namespace microide::project::remote {
namespace {

constexpr std::size_t kDefaultCreditBytes = 256 * 1024;

util::JsonValue HandleParams(std::uint64_t handle,
                             std::initializer_list<std::pair<const char*, std::uint64_t>> extra = {}) {
  util::JsonObject params;
  params["handle"] = util::JsonValue(static_cast<std::int64_t>(handle));
  for (const auto& [key, value] : extra) {
    params[key] = util::JsonValue(static_cast<std::int64_t>(value));
  }
  return util::JsonValue(std::move(params));
}

util::JsonValue AttachParams(std::uint64_t handle,
                             const std::optional<terminal::TerminalSession::HostResumePoint>& resume) {
  util::JsonObject params;
  params["handle"] = util::JsonValue(static_cast<std::int64_t>(handle));
  if (resume.has_value()) {
    params["screen_top"] = util::JsonValue(static_cast<std::int64_t>(resume->screen_top));
    params["alternate"] = util::JsonValue(resume->alternate);
  }
  return util::JsonValue(std::move(params));
}

}  // namespace

std::shared_ptr<RemoteTerminalChannel> RemoteTerminalChannel::Open(
    std::shared_ptr<RemoteServerClient> client,
    const terminal::HostTerminalSource::OpenRequest& request, terminal::TerminalSession& session,
    HostToLocal host_to_local) {
  if (client == nullptr || !client->connected()) {
    return nullptr;
  }
  auto channel = std::make_shared<RemoteTerminalChannel>(client, session, std::move(host_to_local));
  util::JsonObject params;
  if (!request.working_directory.empty()) {
    params["cwd"] = util::JsonValue(request.working_directory.string());
  }
  if (!request.command.empty()) {
    params["command"] = util::JsonValue(request.command);
  }
  if (!request.shell.empty()) {
    util::JsonArray shell;
    for (const std::string& arg : request.shell) {
      shell.push_back(util::JsonValue(arg));
    }
    params["shell"] = util::JsonValue(std::move(shell));
  }
  params["rows"] = util::JsonValue(static_cast<std::int64_t>(request.rows));
  params["columns"] = util::JsonValue(static_cast<std::int64_t>(request.columns));
  params["scrollback_lines"] = util::JsonValue(static_cast<std::int64_t>(request.scrollback_lines));
  if (request.credit_bytes != 0) {
    params["credit_bytes"] = util::JsonValue(static_cast<std::int64_t>(request.credit_bytes));
  }
  if (request.prefetch_lines != 0) {
    params["prefetch_lines"] = util::JsonValue(static_cast<std::int64_t>(request.prefetch_lines));
  }
  // The reply runs on the I/O thread; a channel closed by then just closes the
  // terminal it was given.
  const std::weak_ptr<RemoteTerminalChannel> weak = channel;
  // The client WEAKLY: this callback lives in the client's own peer, so a strong
  // capture is a cycle, and the reply arriving released the last reference on
  // the transport's I/O thread — destroying the transport from inside its own
  // thread, which is std::terminate (a joinable std::thread destroyed).
  const std::weak_ptr<RemoteServerClient> weak_client = client;
  const std::uint64_t id = client->peer().Request(
      method::kTermOpen, util::JsonValue(std::move(params)), Lane::Interactive,
      [weak, weak_client](std::optional<util::JsonValue> result,
                          std::optional<RemotePeer::RpcError> error) {
        const std::shared_ptr<RemoteServerClient> client = weak_client.lock();
        if (client == nullptr) {
          return;  // the connection is gone with everything it served
        }
        const std::shared_ptr<RemoteTerminalChannel> channel = weak.lock();
        const std::int64_t handle = result.has_value() ? (*result)["handle"].AsInt(0) : 0;
        if (channel == nullptr) {
          if (handle > 0) {
            client->peer().Notify(method::kTermClose, HandleParams(static_cast<std::uint64_t>(handle)));
          }
          return;
        }
        if (handle <= 0) {
          channel->OpenFailed(error.has_value() ? error->message : "the host refused term/open");
          return;
        }
        channel->Attached(client, static_cast<std::uint64_t>(handle),
                          static_cast<std::size_t>(
                              (*result)["credit_bytes"].AsInt(kDefaultCreditBytes)),
                          /*reattach=*/false);
      });
  return id == 0 ? nullptr : channel;
}

std::shared_ptr<RemoteTerminalChannel> RemoteTerminalChannel::Attach(
    std::shared_ptr<RemoteServerClient> client, std::uint64_t handle,
    terminal::TerminalSession& session, HostToLocal host_to_local,
    std::optional<terminal::TerminalSession::HostResumePoint> resume) {
  if (client == nullptr || !client->connected() || handle == 0) {
    return nullptr;
  }
  auto channel = std::make_shared<RemoteTerminalChannel>(client, session, std::move(host_to_local));
  {
    std::lock_guard lock(channel->mutex_);
    channel->handle_ = handle;
  }
  return channel->SendAttach(client, handle, resume, /*reattach=*/false) ? channel : nullptr;
}

void RemoteTerminalChannel::Reattach(std::shared_ptr<RemoteServerClient> client) {
  std::shared_ptr<RemoteServerClient> previous;
  std::uint64_t handle = 0;
  std::optional<terminal::TerminalSession::HostResumePoint> resume;
  {
    std::lock_guard lock(mutex_);
    if (session_ == nullptr || handle_ == 0 || client == nullptr) {
      return;
    }
    previous = std::exchange(client_, client);
    handle = handle_;
    resume = session_->host_resume_point();
    attached_ = false;
    reattaching_ = false;
    received_bytes_ = 0;
    acked_bytes_ = 0;
  }
  if (previous != nullptr) {
    previous->UnregisterTerminal(handle);
  }
  SendAttach(client, handle, resume, /*reattach=*/true);
}

bool RemoteTerminalChannel::SendAttach(
    const std::shared_ptr<RemoteServerClient>& client, std::uint64_t handle,
    const std::optional<terminal::TerminalSession::HostResumePoint>& resume, bool reattach) {
  const std::weak_ptr<RemoteTerminalChannel> weak = weak_from_this();
  const std::weak_ptr<RemoteServerClient> weak_client = client;  // see Open
  return client->peer().Request(
             method::kTermAttach, AttachParams(handle, resume), Lane::Interactive,
             [weak, weak_client, handle, reattach](std::optional<util::JsonValue> result,
                                                   std::optional<RemotePeer::RpcError> error) {
               const std::shared_ptr<RemoteTerminalChannel> channel = weak.lock();
               const std::shared_ptr<RemoteServerClient> client = weak_client.lock();
               if (channel == nullptr || client == nullptr) {
                 return;
               }
               if (!result.has_value()) {
                 channel->OpenFailed(error.has_value() ? error->message
                                                       : "the host refused term/attach");
                 return;
               }
               channel->Attached(client, handle, kDefaultCreditBytes, reattach);
             }) != 0;
}

std::optional<std::chrono::milliseconds> RemoteTerminalChannel::RoundTrip() const {
  const std::shared_ptr<RemoteServerClient> client = Client();
  const std::optional<std::int64_t> rtt =
      client != nullptr ? client->peer().rtt_ms() : std::optional<std::int64_t>();
  if (!rtt.has_value()) {
    return std::nullopt;
  }
  return std::chrono::milliseconds(*rtt);
}

std::uint64_t RemoteTerminalChannel::handle() const {
  std::lock_guard lock(mutex_);
  return handle_;
}

std::shared_ptr<RemoteServerClient> RemoteTerminalChannel::Client() const {
  std::lock_guard lock(mutex_);
  return client_;
}

RemoteTerminalChannel::RemoteTerminalChannel(std::shared_ptr<RemoteServerClient> client,
                                             terminal::TerminalSession& session,
                                             HostToLocal host_to_local)
    : host_to_local_(std::move(host_to_local)), client_(std::move(client)), session_(&session) {}

RemoteTerminalChannel::~RemoteTerminalChannel() { Close(); }

void RemoteTerminalChannel::Attached(const std::shared_ptr<RemoteServerClient>& client,
                                     std::uint64_t handle, std::size_t credit_bytes,
                                     bool reattach) {
  std::string input;
  std::size_t rows = 0;
  std::size_t columns = 0;
  terminal::TerminalSession* session = nullptr;
  {
    std::lock_guard lock(mutex_);
    if (session_ == nullptr || client_ != client) {
      // Closed meanwhile, or superseded by a newer reconnect.
      if (session_ == nullptr && !reattach) {
        client->peer().Notify(method::kTermClose, HandleParams(handle));
      }
      return;
    }
    handle_ = handle;
    attached_ = true;
    credit_bytes_ = std::max<std::size_t>(credit_bytes, 4096);
    input.swap(pending_input_);
    rows = pending_rows_;
    columns = pending_columns_;
    session = session_;
  }
  auto events = std::make_shared<RemoteServerClient::TerminalEvents>();
  const std::weak_ptr<RemoteTerminalChannel> weak = weak_from_this();
  events->frame = [weak](std::string_view bytes) {
    if (const auto channel = weak.lock()) {
      channel->ApplyFrame(bytes);
    }
  };
  events->lost = [weak](std::string_view reason) {
    if (const auto channel = weak.lock()) {
      std::lock_guard lock(channel->mutex_);
      channel->attached_ = false;
      if (channel->session_ != nullptr) {
        channel->session_->HostConnectionLost(reason);
      }
    }
  };
  if (reattach) {
    // Before the held frames replay: they are this connection's frames for the
    // same mirror, and the session must take input again.
    session->ReattachHost(shared_from_this());
  }
  client->RegisterTerminal(handle, std::move(events));
  if (rows > 0) {
    SendResize(client, handle, rows, columns);
  }
  if (!input.empty()) {
    client->peer().SendContent(FrameType::TermInput, handle, input, Lane::Interactive);
  }
}

void RemoteTerminalChannel::OpenFailed(const std::string& error) {
  std::lock_guard lock(mutex_);
  if (session_ != nullptr) {
    session_->HostConnectionLost("could not open a terminal on the host: " + error);
  }
}

void RemoteTerminalChannel::ApplyFrame(std::string_view bytes) {
  std::optional<terminal::TerminalHostFrame> frame = terminal::DecodeTerminalHostFrame(bytes);
  std::lock_guard lock(mutex_);
  if (!frame.has_value()) {
    client_->peer().Fail("malformed terminal frame");
    return;
  }
  if (frame->working_directory.has_value() && !frame->working_directory->empty() &&
      host_to_local_) {
    *frame->working_directory = host_to_local_(*frame->working_directory).string();
  }
  // A host program's `file:///abs` link (the host session normalized its own
  // host name away) names a host path: open it where the editor has it.
  if (host_to_local_) {
    for (terminal::TerminalHostLink& link : frame->links) {
      if (link.uri.starts_with("file:///")) {
        link.uri = "file://" + host_to_local_(link.uri.substr(7)).string();
      }
    }
  }
  if (session_ == nullptr) {
    return;
  }
  const bool reset = frame->has(terminal::TerminalHostFrame::kReset);
  const bool consistent = session_->ApplyHostFrame(std::move(*frame));
  received_bytes_ += bytes.size();
  if (received_bytes_ - acked_bytes_ >= credit_bytes_ / 4) {
    acked_bytes_ = received_bytes_;
    client_->peer().Notify(method::kTermAck, HandleParams(handle_, {{"bytes", received_bytes_}}));
  }
  if (reset) {
    reattaching_ = false;
  }
  if (!consistent && !reattaching_) {
    // The mirror disagrees with the host's model of it (a bug, or frames lost):
    // ask for a cold frame rather than draw a screen that is wrong.
    reattaching_ = true;
    received_bytes_ = 0;
    acked_bytes_ = 0;
    client_->peer().Request(method::kTermAttach, HandleParams(handle_), Lane::Interactive,
                            [](std::optional<util::JsonValue>, std::optional<RemotePeer::RpcError>) {});
  }
}

void RemoteTerminalChannel::Send(const terminal::TerminalInputEvent& event) {
  std::string bytes;
  terminal::EncodeTerminalInputEvent(bytes, event);
  std::shared_ptr<RemoteServerClient> client;
  std::uint64_t handle = 0;
  {
    std::lock_guard lock(mutex_);
    if (session_ == nullptr) {
      return;
    }
    if (!attached_) {
      // Before the open (or a reattach) is answered: keep it, in order.
      pending_input_ += bytes;
      return;
    }
    client = client_;
    handle = handle_;
  }
  client->peer().SendContent(FrameType::TermInput, handle, bytes, Lane::Interactive);
}

void RemoteTerminalChannel::SendResize(const std::shared_ptr<RemoteServerClient>& client,
                                       std::uint64_t handle, std::size_t rows,
                                       std::size_t columns) {
  client->peer().Notify(method::kTermResize,
                        HandleParams(handle, {{"rows", rows}, {"columns", columns}}));
}

void RemoteTerminalChannel::Resize(std::size_t rows, std::size_t columns) {
  std::shared_ptr<RemoteServerClient> client;
  std::uint64_t handle = 0;
  {
    std::lock_guard lock(mutex_);
    if (session_ == nullptr) {
      return;
    }
    pending_rows_ = rows;
    pending_columns_ = columns;
    if (!attached_) {
      return;
    }
    client = client_;
    handle = handle_;
  }
  SendResize(client, handle, rows, columns);
}

void RemoteTerminalChannel::Close() {
  std::shared_ptr<RemoteServerClient> client;
  std::uint64_t handle = 0;
  {
    std::lock_guard lock(mutex_);
    if (session_ == nullptr) {
      return;
    }
    session_ = nullptr;
    client = client_;
    handle = handle_;
  }
  if (handle != 0 && client != nullptr) {
    client->UnregisterTerminal(handle);
    client->peer().Notify(method::kTermClose, HandleParams(handle));
  }
}

}  // namespace microide::project::remote
