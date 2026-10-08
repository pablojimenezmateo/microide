#include "project/remote/RemoteServerClient.h"

#include <atomic>
#include <thread>
#include <utility>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

namespace microide::project::remote {
namespace {

// Output held for a handle whose spawn reply has not been handled yet. Bounded:
// the race this covers is a few frames, not a stream.
constexpr std::size_t kMaxOrphanBytes = 4 * 1024 * 1024;

}  // namespace

RemoteServerClient::~RemoteServerClient() {
  peer_.Stop();
  if (command_.IsRunning()) {
    command_.Shutdown(2000);
  }
}

bool RemoteServerClient::ConnectCommand(const std::vector<std::string>& argv,
                                        const HelloRequest& hello, std::string* error) {
  if (!command_.Start(argv)) {
    *error = "could not run " + (argv.empty() ? std::string("<nothing>") : argv.front());
    return false;
  }
  RemotePeer::Options options;
  options.send_pings = true;
  // The descriptors belong to command_, which closes them when it shuts down.
  options.transport.owns_fds = false;
  InstallRouting();
  if (!peer_.Start(command_.stdout_fd(), command_.stdin_fd(), options)) {
    *error = "could not start the connection";
    return false;
  }
  return Handshake(hello, error);
}

std::optional<int> RemoteServerClient::command_exit_code(std::chrono::milliseconds wait) {
  const auto deadline = std::chrono::steady_clock::now() + wait;
  while (command_.IsRunning() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return command_.exit_code();
}

bool RemoteServerClient::ConnectFds(int read_fd, int write_fd, const HelloRequest& hello,
                                    std::string* error) {
  RemotePeer::Options options;
  options.send_pings = true;
  InstallRouting();
  if (!peer_.Start(read_fd, write_fd, options)) {
    *error = "could not start the connection";
    return false;
  }
  return Handshake(hello, error);
}

void RemoteServerClient::InstallRouting() {
  // Before Start: the peer reads its handler tables unlocked from its I/O thread.
  peer_.OnContent([this](FrameType type, std::uint64_t handle, std::string bytes) {
    if (type == FrameType::TermFrame) {
      DeliverTerminalFrame(handle, std::move(bytes));
      return;
    }
    Deliver(handle, type, std::move(bytes));
  });
  peer_.OnClosed([this](std::string_view reason) {
    std::map<std::uint64_t, std::shared_ptr<TerminalEvents>> terminals;
    {
      std::lock_guard lock(mutex_);
      terminals.swap(terminals_);
    }
    for (auto& [handle, events] : terminals) {
      (void)handle;
      if (events->lost) {
        events->lost(reason);
      }
    }
    if (connected_ && closed_handler_) {
      closed_handler_(reason);
    }
  });
  peer_.OnNotification("proc/exit", [this](std::uint64_t handle, const util::JsonValue& params) {
    DeliverExit(handle, params);
  });
}

bool RemoteServerClient::Handshake(const HelloRequest& hello, std::string* error) {
  std::string why;
  const std::optional<util::JsonValue> reply =
      Call(method::kServerHello, ToJson(hello), &why, std::chrono::seconds(30));
  if (!reply.has_value()) {
    *error = why.empty() ? "the server did not answer the hello" : why;
    return false;
  }
  std::optional<HelloReply> parsed = HelloReplyFromJson(*reply, &why);
  if (!parsed.has_value()) {
    *error = "malformed hello reply: " + why;
    return false;
  }
  if (const auto incompatible =
          CheckProtocolCompatibility(hello.protocol, hello.min_protocol, hello.release,
                                     parsed->protocol, parsed->min_protocol, parsed->release)) {
    *error = *incompatible;
    incompatible_ = true;
    return false;
  }
  hello_reply_ = std::move(*parsed);
  connected_ = true;
  return true;
}

std::optional<util::JsonValue> RemoteServerClient::Call(std::string_view method,
                                                        const util::JsonValue& params,
                                                        std::string* error,
                                                        std::chrono::milliseconds timeout) {
  struct State {
    std::mutex mutex;
    std::condition_variable done;
    bool finished = false;
    std::optional<util::JsonValue> result;
    std::string error;
  };
  // Shared: a late reply after a timeout must not touch a dead stack frame.
  auto state = std::make_shared<State>();
  const std::uint64_t id = peer_.Request(
      method, params, Lane::Interactive,
      [state](std::optional<util::JsonValue> result, std::optional<RemotePeer::RpcError> rpc) {
        std::lock_guard lock(state->mutex);
        state->result = std::move(result);
        if (rpc.has_value()) {
          state->error = rpc->message;
        }
        state->finished = true;
        state->done.notify_all();
      });
  if (id == 0) {
    *error = "not connected";
    return std::nullopt;
  }
  std::unique_lock lock(state->mutex);
  if (!state->done.wait_for(lock, timeout, [&]() { return state->finished; })) {
    *error = std::string(method) + " timed out";
    return std::nullopt;
  }
  *error = state->error;
  return std::move(state->result);
}

RemoteServerClient::Spawned RemoteServerClient::Spawn(
    const std::vector<std::string>& argv, const std::filesystem::path& host_cwd,
    const std::vector<std::pair<std::string, std::optional<std::string>>>& env,
    bool keep_on_detach, ProcessEvents events) {
  util::JsonArray args;
  for (const std::string& arg : argv) {
    args.push_back(util::JsonValue(arg));
  }
  util::JsonObject params;
  params["argv"] = util::JsonValue(std::move(args));
  if (!host_cwd.empty()) {
    params["cwd"] = util::JsonValue(host_cwd.string());
  }
  if (!env.empty()) {
    util::JsonObject env_object;
    for (const auto& [key, value] : env) {
      env_object[key] = value.has_value() ? util::JsonValue(*value) : util::JsonValue(nullptr);
    }
    params["env"] = util::JsonValue(std::move(env_object));
  }
  params["keep_on_detach"] = util::JsonValue(keep_on_detach);
  std::string error;
  const std::optional<util::JsonValue> reply =
      Call("proc/spawn", util::JsonValue(std::move(params)), &error);
  if (!reply.has_value() || (*reply)["handle"].AsInt(0) <= 0) {
    return Spawned{.error = error.empty() ? "proc/spawn failed" : error};
  }
  const auto handle = static_cast<std::uint64_t>((*reply)["handle"].AsInt());
  auto shared = std::make_shared<ProcessEvents>(std::move(events));
  Orphan held;
  {
    std::lock_guard lock(mutex_);
    processes_[handle] = shared;
    if (const auto orphan = orphans_.find(handle); orphan != orphans_.end()) {
      held = std::move(orphan->second);
      orphans_.erase(orphan);
    }
  }
  for (auto& [type, bytes] : held.output) {
    if (shared->output) {
      shared->output(type, bytes);
    }
  }
  if (held.exit.has_value()) {
    DeliverExit(handle, *held.exit);
  }
  return Spawned{.handle = handle, .pid = static_cast<int>((*reply)["pid"].AsInt(-1))};
}

void RemoteServerClient::Deliver(std::uint64_t handle, FrameType type, std::string bytes) {
  std::shared_ptr<ProcessEvents> events;
  {
    std::lock_guard lock(mutex_);
    const auto it = processes_.find(handle);
    if (it == processes_.end()) {
      Orphan& orphan = orphans_[handle];
      if (orphan.bytes + bytes.size() <= kMaxOrphanBytes) {
        orphan.bytes += bytes.size();
        orphan.output.emplace_back(type, std::move(bytes));
      }
      return;
    }
    events = it->second;
  }
  if (events->output) {
    events->output(type, bytes);
  }
}

void RemoteServerClient::DeliverExit(std::uint64_t handle, const util::JsonValue& params) {
  std::shared_ptr<ProcessEvents> events;
  {
    std::lock_guard lock(mutex_);
    const auto it = processes_.find(handle);
    if (it == processes_.end()) {
      orphans_[handle].exit = params;
      return;
    }
    events = it->second;
  }
  if (events->exit) {
    std::optional<int> code;
    std::optional<int> signal;
    if (params.HasKey("signal")) {
      signal = static_cast<int>(params["signal"].AsInt());
    } else {
      code = static_cast<int>(params["code"].AsInt(-1));
    }
    events->exit(code, signal);
  }
}

void RemoteServerClient::DeliverTerminalFrame(std::uint64_t handle, std::string bytes) {
  std::shared_ptr<TerminalEvents> events;
  {
    std::lock_guard lock(mutex_);
    const auto it = terminals_.find(handle);
    if (it == terminals_.end()) {
      Orphan& orphan = terminal_orphans_[handle];
      if (orphan.bytes + bytes.size() <= kMaxOrphanBytes) {
        orphan.bytes += bytes.size();
        orphan.output.emplace_back(FrameType::TermFrame, std::move(bytes));
      }
      return;
    }
    events = it->second;
  }
  if (events->frame) {
    events->frame(bytes);
  }
}

void RemoteServerClient::RegisterTerminal(std::uint64_t handle,
                                          std::shared_ptr<TerminalEvents> events) {
  // Replayed under the lock, so a frame arriving meanwhile on the I/O thread
  // cannot overtake the held ones; the I/O thread waits on mutex_ for that long.
  std::lock_guard lock(mutex_);
  if (const auto orphan = terminal_orphans_.find(handle); orphan != terminal_orphans_.end()) {
    for (auto& [type, bytes] : orphan->second.output) {
      (void)type;
      if (events->frame) {
        events->frame(bytes);
      }
    }
    terminal_orphans_.erase(orphan);
  }
  terminals_[handle] = std::move(events);
}

void RemoteServerClient::UnregisterTerminal(std::uint64_t handle) {
  std::lock_guard lock(mutex_);
  terminals_.erase(handle);
  terminal_orphans_.erase(handle);
}

bool RemoteServerClient::WriteStdin(std::uint64_t handle, std::string_view bytes) {
  while (!bytes.empty()) {
    const std::size_t chunk = std::min<std::size_t>(bytes.size(), 64 * 1024);
    if (!peer_.SendContent(FrameType::ProcStdin, handle, bytes.substr(0, chunk))) {
      return false;
    }
    bytes.remove_prefix(chunk);
  }
  return true;
}

bool RemoteServerClient::CloseStdin(std::uint64_t handle) {
  util::JsonObject params;
  params["handle"] = util::JsonValue(static_cast<std::int64_t>(handle));
  return peer_.Notify("proc/stdin_close", util::JsonValue(std::move(params)));
}

bool RemoteServerClient::Signal(std::uint64_t handle, std::string_view signal) {
  util::JsonObject params;
  params["handle"] = util::JsonValue(static_cast<std::int64_t>(handle));
  params["signal"] = util::JsonValue(std::string(signal));
  return peer_.Notify("proc/signal", util::JsonValue(std::move(params)));
}

void RemoteServerClient::Ack(std::uint64_t handle, std::uint64_t stdout_offset,
                             std::uint64_t stderr_offset) {
  util::JsonObject params;
  params["handle"] = util::JsonValue(static_cast<std::int64_t>(handle));
  params["stdout"] = util::JsonValue(static_cast<std::int64_t>(stdout_offset));
  params["stderr"] = util::JsonValue(static_cast<std::int64_t>(stderr_offset));
  peer_.Notify("proc/ack", util::JsonValue(std::move(params)));
}

void RemoteServerClient::Release(std::uint64_t handle) {
  {
    std::lock_guard lock(mutex_);
    processes_.erase(handle);
    orphans_.erase(handle);
  }
  util::JsonObject params;
  params["handle"] = util::JsonValue(static_cast<std::int64_t>(handle));
  peer_.Notify("proc/release", util::JsonValue(std::move(params)));
}

std::optional<RemoteServerClient::GitMetadata> RemoteServerClient::QueryGitMetadata(
    const std::filesystem::path& host_root) {
  util::JsonObject params;
  params["root"] = util::JsonValue(host_root.string());
  std::string error;
  const auto reply = Call("git/metadata", util::JsonValue(std::move(params)), &error);
  if (!reply.has_value()) {
    return std::nullopt;
  }
  return GitMetadata{.repository = (*reply)["repository"].AsBool(false),
                     .git_dir = (*reply)["git_dir"].AsString()};
}

}  // namespace microide::project::remote
