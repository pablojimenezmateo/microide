#pragma once

#include <atomic>
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
#include "project/GitMetadataSource.h"
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
  // After a failed Connect*: the server answered, but with a protocol outside
  // this client's range (install the matching one).
  bool incompatible() const { return incompatible_; }
  // After a failed ConnectCommand: the command's exit status, waiting up to
  // `wait` for it to exit. 127 is a shell's "command not found".
  std::optional<int> command_exit_code(std::chrono::milliseconds wait);
  // Runs once, on the I/O thread, when an established connection closes (the
  // link died, or the server went away) — after every terminal heard of it.
  // Set before Connect*.
  void SetClosedHandler(std::function<void(std::string_view reason)> handler) {
    closed_handler_ = std::move(handler);
  }
  const HelloReply& hello() const { return hello_reply_; }
  RemotePeer& peer() { return peer_; }

  Spawned Spawn(const std::vector<std::string>& argv, const std::filesystem::path& host_cwd,
                const std::vector<std::pair<std::string, std::optional<std::string>>>& env,
                bool keep_on_detach, ProcessEvents events);
  // Route an existing handle's events here (a kept process taken over after a
  // reconnect); output that arrived before this is replayed, as for Spawn.
  void RegisterProcess(std::uint64_t handle, ProcessEvents events);
  // proc/attach: resume a kept process from the byte offsets already received.
  // False with *error when the host no longer has it (it restarted).
  bool AttachProcess(std::uint64_t handle, std::uint64_t stdout_offset,
                     std::uint64_t stderr_offset, std::string* error);
  bool WriteStdin(std::uint64_t handle, std::string_view bytes);
  bool CloseStdin(std::uint64_t handle);
  bool Signal(std::uint64_t handle, std::string_view signal);  // "TERM", "KILL", ...
  // Acknowledge output through these offsets: the server's credit window refills.
  void Ack(std::uint64_t handle, std::uint64_t stdout_offset, std::uint64_t stderr_offset);
  // Done with an exited handle; its events are dropped.
  void Release(std::uint64_t handle);

  std::optional<GitMetadata> QueryGitMetadata(const std::filesystem::path& host_root);

  // Pushed git status (git/subscribe, remote-projects.md § 6.5): the host pushes
  // `git status` after every change to the workspace or its repository, so the
  // sidebar renders from what is held here. `changed` runs on the I/O thread with
  // the host root of every push. False with *error when the host refused.
  bool SubscribeGitStatus(std::function<void(const std::string& host_root)> changed,
                          std::string* error);
  // The pushed status of `host_root`, only while it is CURRENT: no git command
  // that may change a repository is running through this client, and the push
  // was computed after the last one exited (its proc/exit `git_generation`).
  std::optional<project::GitStatusOutput> CurrentGitStatus(const std::string& host_root);
  // Around a git command that may change a repository (RemoteProcessLauncher::Run).
  void BeginGitMutation();
  void EndGitMutation();

  // Host terminals (terminal/TerminalHostWire.h). `frame` runs on the I/O thread
  // with each TermFrame for `handle`, in order — including any that arrived
  // before the registration (the server's first frame races its term/open
  // reply). `lost` runs once if the connection closes while registered.
  struct TerminalEvents {
    std::function<void(std::string_view frame)> frame;
    std::function<void(std::string_view reason)> lost;
  };
  void RegisterTerminal(std::uint64_t handle, std::shared_ptr<TerminalEvents> events);
  void UnregisterTerminal(std::uint64_t handle);

  // A request answered by content frames (id = the request) and then a response:
  // `content` runs on the I/O thread for each frame, in order, and `done` once
  // after the last. Returns the request id (0 when it could not be sent; `done`
  // does not run then).
  using StreamContent = std::function<void(FrameType type, std::string bytes)>;
  std::uint64_t RequestStream(std::string_view method, const util::JsonValue& params, Lane lane,
                              StreamContent content, RemotePeer::ResponseHandler done);

  // WatchDelta frames (id = the manifest id they bring the client to), in order, on
  // the I/O thread. Set before watch/subscribe.
  void SetWatchHandler(std::function<void(std::uint64_t manifest_id, std::string bytes)> handler) {
    std::lock_guard lock(mutex_);
    watch_handler_ = std::make_shared<std::function<void(std::uint64_t, std::string)>>(std::move(handler));
  }

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
  void DeliverTerminalFrame(std::uint64_t handle, std::string bytes);

  platform::AsyncSubprocess command_;  // the server (or ssh), when ConnectCommand ran it
  RemotePeer peer_;
  std::atomic<bool> connected_{false};  // read by the I/O thread's close handler
  bool incompatible_ = false;
  std::function<void(std::string_view)> closed_handler_;
  HelloReply hello_reply_;

  std::mutex mutex_;
  std::map<std::uint64_t, std::shared_ptr<ProcessEvents>> processes_;
  // Output that arrived before its spawn reply was handled (the server's process
  // thread and its reply race); replayed when the handle is registered.
  std::map<std::uint64_t, Orphan> orphans_;
  std::map<std::uint64_t, std::shared_ptr<TerminalEvents>> terminals_;
  // Frames for a terminal whose open reply has not been handled yet.
  std::map<std::uint64_t, Orphan> terminal_orphans_;
  // Content routes of in-flight RequestStream calls, by request id.
  std::map<std::uint64_t, std::shared_ptr<StreamContent>> streams_;
  std::shared_ptr<std::function<void(std::uint64_t, std::string)>> watch_handler_;
  struct PushedGitStatus {
    project::GitStatusOutput status;
    std::uint64_t generation = 0;
  };
  std::map<std::string, PushedGitStatus, std::less<>> pushed_git_;
  std::shared_ptr<std::function<void(const std::string&)>> git_status_handler_;
  int git_mutations_in_flight_ = 0;
  std::uint64_t git_required_generation_ = 0;
  // The highest proc/exit `git_generation` seen; recorded before the exit is
  // delivered, so a Run that returns has already raised it.
  std::atomic<std::uint64_t> last_git_generation_{0};
};

}  // namespace microide::project::remote
