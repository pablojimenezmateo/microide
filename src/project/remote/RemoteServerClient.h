#pragma once

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "platform/AsyncSubprocess.h"
#include "project/remote/RemoteFrame.h"
#include "project/remote/RemotePeer.h"
#include "project/remote/RemoteProtocol.h"

namespace microide::project::remote {

// The editor's end of one connection to a microide-server (dev-docs/design/
// remote-projects.md § 6.4): it owns the RemotePeer, does the hello, and routes
// process output and exits to whoever spawned them. Locality-agnostic about HOW the
// bytes travel — a local `microide-server serve-stdio` for tests, `ssh host --
// microide-server attach` for a real host — because both are just a command whose
// stdio is the protocol.
//
// Threading: synchronous calls (Connect*, Spawn, GitMetadata) block the calling
// thread on a reply and must not be made from the connection's own I/O thread;
// the per-process callbacks run on that I/O thread.
class RemoteServerClient {
 public:
  struct ProcessEvents {
    // stdout or stderr bytes, in order per stream.
    std::function<void(FrameType stream, std::string_view bytes)> output;
    // The process exited: a code, or the signal that killed it. Runs once, after
    // every byte of its output.
    std::function<void(std::optional<int> code, std::optional<int> signal)> exit;
  };

  struct Spawned {
    std::uint64_t handle = 0;
    int pid = -1;
    std::string error;
  };

  struct GitMetadata {
    bool repository = false;
    std::string git_dir;  // a HOST path
  };

  RemoteServerClient() = default;
  ~RemoteServerClient();
  RemoteServerClient(const RemoteServerClient&) = delete;
  RemoteServerClient& operator=(const RemoteServerClient&) = delete;

  // Run `argv` (the server, or the ssh command that reaches it) and speak the
  // protocol over its stdio. False with *error, which names an incompatible server.
  bool ConnectCommand(const std::vector<std::string>& argv, const HelloRequest& hello,
                      std::string* error);
  // Speak it over descriptors this client takes ownership of.
  bool ConnectFds(int read_fd, int write_fd, const HelloRequest& hello, std::string* error);

  bool connected() const { return connected_ && !peer_.closed(); }
  const HelloReply& hello() const { return hello_reply_; }
  RemotePeer& peer() { return peer_; }

  Spawned Spawn(const std::vector<std::string>& argv, const std::filesystem::path& host_cwd,
                const std::vector<std::pair<std::string, std::optional<std::string>>>& env,
                bool keep_on_detach, ProcessEvents events);
  bool WriteStdin(std::uint64_t handle, std::string_view bytes);
  bool CloseStdin(std::uint64_t handle);
  bool Signal(std::uint64_t handle, std::string_view signal);  // "TERM", "KILL", ...
  // Acknowledge output through these offsets: the server's credit window refills.
  void Ack(std::uint64_t handle, std::uint64_t stdout_offset, std::uint64_t stderr_offset);
  // Done with an exited handle; its events are dropped.
  void Release(std::uint64_t handle);

  std::optional<GitMetadata> QueryGitMetadata(const std::filesystem::path& host_root);

  // One synchronous request; nullopt with *error on failure or after `timeout`.
  std::optional<util::JsonValue> Call(std::string_view method, const util::JsonValue& params,
                                      std::string* error,
                                      std::chrono::milliseconds timeout = std::chrono::seconds(30));

 private:
  struct Orphan {
    std::vector<std::pair<FrameType, std::string>> output;
    std::optional<util::JsonValue> exit;
    std::size_t bytes = 0;
  };

  void InstallRouting();
  bool Handshake(const HelloRequest& hello, std::string* error);
  void Deliver(std::uint64_t handle, FrameType type, std::string bytes);
  void DeliverExit(std::uint64_t handle, const util::JsonValue& params);

  platform::AsyncSubprocess command_;  // the server (or ssh), when ConnectCommand ran it
  RemotePeer peer_;
  bool connected_ = false;
  HelloReply hello_reply_;

  std::mutex mutex_;
  std::map<std::uint64_t, std::shared_ptr<ProcessEvents>> processes_;
  // Output that arrived before its spawn reply was handled (the server's process
  // thread and its reply race); replayed when the handle is registered.
  std::map<std::uint64_t, Orphan> orphans_;
};

}  // namespace microide::project::remote
