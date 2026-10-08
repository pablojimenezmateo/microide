#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "platform/SubprocessSandbox.h"

namespace microide::platform {

// A long-running subprocess with bidirectional stdin/stdout communication.
// Designed for LSP and similar protocol-backed servers.
// POSIX-only; returns "not implemented" results elsewhere.
// The process behind an ADOPTED AsyncSubprocess — one that runs somewhere else (a
// remote host, through microide-server) with its stdio carried by two local pipes.
// Liveness, exit status and termination are asked of it instead of waitpid/kill.
class AsyncProcessController {
 public:
  virtual ~AsyncProcessController() = default;
  virtual bool IsRunning() const = 0;
  virtual std::optional<int> exit_code() const = 0;
  virtual int pid() const = 0;
  // SIGTERM, then SIGKILL if it has not exited within `timeout_ms`.
  virtual void Terminate(int timeout_ms) = 0;
};

class AsyncSubprocess {
 public:
  AsyncSubprocess();
  ~AsyncSubprocess();
  AsyncSubprocess(const AsyncSubprocess&) = delete;
  AsyncSubprocess& operator=(const AsyncSubprocess&) = delete;
  AsyncSubprocess(AsyncSubprocess&&) noexcept;
  AsyncSubprocess& operator=(AsyncSubprocess&&) noexcept;

  // Launch the process. Returns false on failure. When `sandbox.enabled`, the child is confined
  // (Linux Landlock/seccomp/setrlimit) between fork and exec — used for plugin-contributed servers.
  bool Start(const std::vector<std::string>& argv, const std::string& cwd = {},
             const SubprocessSandbox& sandbox = {});

  // Take over a process started elsewhere: `stdin_fd` is written to feed its stdin,
  // `stdout_fd` read for its stdout (both now owned, made non-blocking), and
  // `controller` answers for the process itself. Everything else — Write, Read,
  // stdout_fd() for a poll loop, CloseStdin — works exactly as for a local child,
  // which is what lets the language server and debug adapter clients run a remote
  // process without knowing it is one. False when already running.
  bool Adopt(int stdin_fd, int stdout_fd, std::shared_ptr<AsyncProcessController> controller);

  // True while the child process is believed to be alive.
  bool IsRunning() const;

  // Write bytes to stdin. Returns false if stdin is closed or the process is dead.
  bool Write(std::string_view data);

  // Read up to `max_bytes` from stdout, waiting at most `timeout_ms` milliseconds.
  // Returns nullopt if the process has exited or a fatal read error occurred.
  // Returns an empty string if the timeout expired without any data.
  std::optional<std::string> Read(std::size_t max_bytes = 65536, int timeout_ms = 1000);

  // Read exactly `n` bytes from stdout (blocking until available or error).
  // Returns nullopt on EOF/error.
  std::optional<std::string> ReadExact(std::size_t n, int timeout_ms = 10000);

  // Close the stdin pipe while keeping stdout available for draining.
  void CloseStdin();

  // Send SIGTERM; if the process does not exit within `timeout_ms` ms, send SIGKILL.
  void Shutdown(int timeout_ms = 3000);

  // pid of the child, or -1 if not running.
  int pid() const;

  // Raw stdout read fd (POSIX), for callers that want to poll() it alongside
  // their own wakeup fd in a single I/O loop. Returns -1 off POSIX or if closed.
  int stdout_fd() const;
  // Raw stdin write fd (POSIX), for a caller that drives both pipes from its own
  // I/O loop (the remote server connection). -1 off POSIX or once closed.
  int stdin_fd() const;

  // exit code of the child once it has exited, or nullopt while still running/unknown.
  std::optional<int> exit_code() const;

 private:
  struct Impl;
  Impl* impl_;  // non-owning pointer to platform-specific state; managed by POSIX impl
};

}  // namespace microide::platform
