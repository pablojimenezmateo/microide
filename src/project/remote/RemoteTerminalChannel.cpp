#include "project/remote/RemoteTerminalChannel.h"

#include <utility>

#include "project/remote/RemoteProtocol.h"
#include "project/remote/RemoteServerClient.h"
#include "terminal/TerminalHostWire.h"
#include "terminal/TerminalSession.h"

namespace microide::project::remote {
namespace {

util::JsonValue HandleParams(std::uint64_t handle,
                             std::initializer_list<std::pair<const char*, std::uint64_t>> extra = {}) {
  util::JsonObject params;
  params["handle"] = util::JsonValue(static_cast<std::int64_t>(handle));
  for (const auto& [key, value] : extra) {
    params[key] = util::JsonValue(static_cast<std::int64_t>(value));
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
  params["cwd"] = util::JsonValue(request.working_directory.string());
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
  // The reply runs on the I/O thread; a channel closed by then just closes the
  // terminal it was given.
  const std::weak_ptr<RemoteTerminalChannel> weak = channel;
  const std::uint64_t id = client->peer().Request(
      method::kTermOpen, util::JsonValue(std::move(params)), Lane::Interactive,
      [weak, client](std::optional<util::JsonValue> result,
                     std::optional<RemotePeer::RpcError> error) {
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
        channel->Opened(static_cast<std::uint64_t>(handle),
                        static_cast<std::size_t>((*result)["credit_bytes"].AsInt(256 * 1024)));
      });
  return id == 0 ? nullptr : channel;
}

std::shared_ptr<RemoteTerminalChannel> RemoteTerminalChannel::Attach(
    std::shared_ptr<RemoteServerClient> client, std::uint64_t handle,
    terminal::TerminalSession& session, HostToLocal host_to_local) {
  if (client == nullptr || !client->connected() || handle == 0) {
    return nullptr;
  }
  auto channel = std::make_shared<RemoteTerminalChannel>(client, session, std::move(host_to_local));
  const std::weak_ptr<RemoteTerminalChannel> weak = channel;
  const std::uint64_t id = client->peer().Request(
      method::kTermAttach, HandleParams(handle), Lane::Interactive,
      [weak, handle](std::optional<util::JsonValue> result,
                     std::optional<RemotePeer::RpcError> error) {
        const std::shared_ptr<RemoteTerminalChannel> channel = weak.lock();
        if (channel == nullptr) {
          return;
        }
        if (!result.has_value()) {
          channel->OpenFailed(error.has_value() ? error->message : "the host refused term/attach");
          return;
        }
        channel->Opened(handle, 256 * 1024);
      });
  return id == 0 ? nullptr : channel;
}

std::uint64_t RemoteTerminalChannel::handle() const {
  std::lock_guard lock(mutex_);
  return handle_;
}

RemoteTerminalChannel::RemoteTerminalChannel(std::shared_ptr<RemoteServerClient> client,
                                             terminal::TerminalSession& session,
                                             HostToLocal host_to_local)
    : client_(std::move(client)), host_to_local_(std::move(host_to_local)), session_(&session) {}

RemoteTerminalChannel::~RemoteTerminalChannel() { Close(); }

void RemoteTerminalChannel::Opened(std::uint64_t handle, std::size_t credit_bytes) {
  std::string input;
  std::size_t rows = 0;
  std::size_t columns = 0;
  {
    std::lock_guard lock(mutex_);
    if (session_ == nullptr) {
      client_->peer().Notify(method::kTermClose, HandleParams(handle));
      return;
    }
    handle_ = handle;
    credit_bytes_ = std::max<std::size_t>(credit_bytes, 4096);
    input.swap(pending_input_);
    rows = pending_rows_;
    columns = pending_columns_;
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
      if (channel->session_ != nullptr) {
        channel->session_->HostConnectionLost(reason);
      }
    }
  };
  client_->RegisterTerminal(handle, std::move(events));
  if (rows > 0) {
    SendResize(handle, rows, columns);
  }
  if (!input.empty()) {
    client_->peer().SendContent(FrameType::TermInput, handle, input, Lane::Interactive);
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
  if (!frame.has_value()) {
    client_->peer().Fail("malformed terminal frame");
    return;
  }
  if (frame->working_directory.has_value() && !frame->working_directory->empty() &&
      host_to_local_) {
    *frame->working_directory = host_to_local_(*frame->working_directory).string();
  }
  std::lock_guard lock(mutex_);
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
  std::uint64_t handle = 0;
  {
    std::lock_guard lock(mutex_);
    if (session_ == nullptr) {
      return;
    }
    if (handle_ == 0) {
      pending_input_ += bytes;
      return;
    }
    handle = handle_;
  }
  client_->peer().SendContent(FrameType::TermInput, handle, bytes, Lane::Interactive);
}

void RemoteTerminalChannel::SendResize(std::uint64_t handle, std::size_t rows, std::size_t columns) {
  client_->peer().Notify(method::kTermResize,
                         HandleParams(handle, {{"rows", rows}, {"columns", columns}}));
}

void RemoteTerminalChannel::Resize(std::size_t rows, std::size_t columns) {
  std::uint64_t handle = 0;
  {
    std::lock_guard lock(mutex_);
    if (session_ == nullptr) {
      return;
    }
    if (handle_ == 0) {
      pending_rows_ = rows;
      pending_columns_ = columns;
      return;
    }
    handle = handle_;
  }
  SendResize(handle, rows, columns);
}

void RemoteTerminalChannel::Close() {
  std::uint64_t handle = 0;
  {
    std::lock_guard lock(mutex_);
    if (session_ == nullptr) {
      return;
    }
    session_ = nullptr;
    handle = handle_;
  }
  if (handle != 0) {
    client_->UnregisterTerminal(handle);
    client_->peer().Notify(method::kTermClose, HandleParams(handle));
  }
}

}  // namespace microide::project::remote
