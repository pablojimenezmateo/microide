#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "project/remote/RemoteFrame.h"
#include "util/JsonValue.h"
#include "util/WakePipe.h"

namespace microide::server {

// The host's processes (dev-docs/design/remote-projects.md § 6.5, `proc/*`): every
// remote spawn — git, the formatter, the language server, the debug adapter, plugin
// tools — is a child of the server, started from an argv ARRAY (no shell, nothing
// quoted or joined) in its own process group.
//
// Each stdio stream is a byte sequence with offsets. The server keeps what the
// client has not acknowledged, so a reattaching client resumes from the offset it
// has through with nothing duplicated and nothing skipped. Sending is bounded per
// handle by a credit window; beyond the window — or beyond the retention cap while
// no client is attached — the server stops reading the pipe and the process feels
// ordinary backpressure. A process not kept on detach is terminated when the client
// that spawned it goes; a kept one keeps running for `Limits::keep_grace` so the
// client that lost its link can take it back with its state, and its exit is
// delivered on the next attach and reaped once a client has released it. A kept
// process nobody reattaches to within the grace is terminated and forgotten like
// an unkept one: every language server a client's crash or SIGTERM left behind
// stayed on the host for the server's lifetime (four clangds with a background
// index after four short sessions on worky, 2026-10-16).
class ProcessTable {
 public:
  // Send a frame or notification to connection `connection` (0 = nobody). The
  // table calls these without holding its own lock.
  struct Sink {
    std::function<void(std::uint64_t connection, project::remote::FrameType type,
                       std::uint64_t handle, std::string_view bytes)>
        content;
    std::function<void(std::uint64_t connection, std::string_view method, std::uint64_t handle,
                        const util::JsonValue& params)>
        notify;
    // A `git` process exited in `cwd`, just before its proc/exit is sent. Returns
    // the git generation that exit made (carried in proc/exit as
    // `git_generation`): a pushed status computed from that generation on is
    // after it (remote-projects.md § 6.5). Optional.
    std::function<std::uint64_t(const std::string& cwd)> git_exited;
  };

  struct Limits {
    std::size_t credit_bytes = 256 * 1024;        // per stream, sent but unacknowledged
    std::size_t retention_bytes = 16u * 1024 * 1024;  // per stream, unacknowledged at all
    std::size_t max_processes = 256;
    // How long a kept process waits for a client to reattach after its client went.
    std::chrono::milliseconds keep_grace{30 * 60 * 1000};
  };

  struct SpawnRequest {
    std::vector<std::string> argv;
    std::string cwd;
    // Set (a value) or unset (nullopt) on top of the server's own environment.
    std::vector<std::pair<std::string, std::optional<std::string>>> env;
    bool keep_on_detach = false;
    // The workspace root of the connection that spawned it ("" = none): what
    // server/status lists the process under. Not from the body -- the server fills
    // it from the connection's hello, as TerminalTable::OpenRequest::root.
    std::string root;
  };
  struct SpawnResult {
    std::uint64_t handle = 0;
    int pid = -1;
    std::string error;
  };

  ProcessTable(Sink sink, Limits limits);
  ProcessTable(Sink sink) : ProcessTable(std::move(sink), Limits{}) {}
  ~ProcessTable();
  ProcessTable(const ProcessTable&) = delete;
  ProcessTable& operator=(const ProcessTable&) = delete;

  // Decode and validate a proc/spawn body (untrusted): nullopt with *error.
  static std::optional<SpawnRequest> ParseSpawn(const util::JsonValue& params, std::string* error);

  SpawnResult Spawn(std::uint64_t connection, SpawnRequest request);
  bool WriteStdin(std::uint64_t handle, std::string_view bytes);
  bool CloseStdin(std::uint64_t handle);
  bool Signal(std::uint64_t handle, int signal);
  // The client has received stdout/stderr through these offsets.
  void Ack(std::uint64_t handle, std::uint64_t stdout_offset, std::uint64_t stderr_offset);
  // Take over a kept handle from `offsets`; false with *error for an unknown
  // handle or an offset outside what the server still holds.
  bool Attach(std::uint64_t connection, std::uint64_t handle, std::uint64_t stdout_offset,
              std::uint64_t stderr_offset, std::string* error);
  // The client is done with an exited handle: forget it.
  void Release(std::uint64_t handle);
  // proc/end (the Host view's End, remote-host-activity D4): terminate it as a
  // detach terminates an unkept process -- SIGTERM to its group, SIGKILL after a
  // grace -- and forget it once it has exited and no client holds it. A client
  // that does hold it gets its proc/exit as usual. False for an unknown handle.
  bool End(std::uint64_t handle);
  // A connection went away: terminate what it spawned without keep_on_detach, and
  // detach the rest.
  void Detach(std::uint64_t connection);

  std::size_t LiveCount() const;
  std::size_t CountFor(std::uint64_t connection) const;

  // One process as server/status lists it (remote-host-activity D3): what a user
  // needs to recognise it ("clangd", kept since a detach) and what it holds.
  struct Listing {
    std::uint64_t handle = 0;
    std::string root;               // the spawning connection's workspace root
    std::vector<std::string> argv;  // as spawned, bounded (kListedArgs x kListedArgBytes)
    int pid = -1;
    std::string cwd;
    bool kept = false;  // keep_on_detach
    // How long ago its client went; nullopt while one is attached.
    std::optional<std::chrono::milliseconds> detached_for;
    bool exited = false;
    std::size_t buffered_bytes = 0;  // stdout + stderr not yet acknowledged
  };
  // The argv a listing keeps: the row's name, not a transcript.
  static constexpr std::size_t kListedArgs = 8;
  static constexpr std::size_t kListedArgBytes = 256;
  // Every process the table holds (live, or exited and not yet released), by
  // handle; at most Limits::max_processes of them.
  std::vector<Listing> List() const;

 private:
  struct Stream {
    int fd = -1;
    bool eof = false;
    std::string pending;         // bytes from `base` onward not yet acknowledged
    std::uint64_t base = 0;      // offset of pending[0]
    std::uint64_t sent = 0;      // offset sent to the attached client
    std::uint64_t total() const { return base + pending.size(); }
  };
  struct Process {
    std::uint64_t handle = 0;
    int pid = -1;
    // Readable when the process exits (Linux pidfd_open), so the exit is reaped and
    // sent the moment it happens; -1 where there is none, and the loop polls.
    int pid_fd = -1;
    int stdin_fd = -1;
    Stream out;
    Stream err;
    bool keep_on_detach = false;
    std::uint64_t connection = 0;
    bool git_mutation = false;  // a git that may change a repository: its exit pushes status
    std::string cwd;
    std::string root;               // SpawnRequest::root
    std::vector<std::string> argv;  // the listed prefix (List)
    bool exited = false;
    int exit_code = -1;
    int exit_signal = 0;
    bool exit_sent = false;
    // Set when a detach terminated it: SIGKILL its group if it is still alive then.
    std::optional<std::chrono::steady_clock::time_point> kill_at;
    // When the table first saw it exited: its pipes get kExitDrainGrace to reach EOF
    // (a grandchild may hold them). On the process, not in a side table, so it goes
    // with the process however it leaves (Release erased it from under a side map,
    // which then kept the loop polling forever: TD-2026-10-28-418).
    std::optional<std::chrono::steady_clock::time_point> exited_at;
    // A kept process whose client went: when, so the keep grace can run out.
    std::optional<std::chrono::steady_clock::time_point> detached_at;
  };
  struct Outgoing {
    std::uint64_t connection = 0;
    bool is_exit = false;
    std::optional<std::string> git_cwd;  // the exit of a git that may have changed a repository
    project::remote::FrameType type = project::remote::FrameType::ProcStdout;
    std::uint64_t handle = 0;
    std::string bytes;
    util::JsonValue params;
  };

  void Run();
  bool Readable(const Stream& stream) const;
  void Pump(Process& process, std::vector<Outgoing>& outgoing);
  void Flush(std::vector<Outgoing>& outgoing);
  void Reap();

  Sink sink_;
  Limits limits_;
  util::WakePipe wake_;
  std::thread thread_;
  std::atomic<bool> stop_{false};

  mutable std::mutex mutex_;
  std::uint64_t next_handle_ = 1;
  std::map<std::uint64_t, std::unique_ptr<Process>> processes_;
};

}  // namespace microide::server
