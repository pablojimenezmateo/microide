#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace microide::platform {

// A long-running subprocess with bidirectional stdin/stdout communication.
// Designed for LSP, DAP, and similar protocol-backed servers.
// POSIX-only; returns "not implemented" results elsewhere.
class AsyncSubprocess {
 public:
  AsyncSubprocess();
  ~AsyncSubprocess();
  AsyncSubprocess(const AsyncSubprocess&) = delete;
  AsyncSubprocess& operator=(const AsyncSubprocess&) = delete;
  AsyncSubprocess(AsyncSubprocess&&) noexcept;
  AsyncSubprocess& operator=(AsyncSubprocess&&) noexcept;

  // Launch the process. Returns false on failure.
  bool Start(const std::vector<std::string>& argv, const std::string& cwd = {});

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

  // Send SIGTERM; if the process does not exit within `timeout_ms` ms, send SIGKILL.
  void Shutdown(int timeout_ms = 3000);

  // pid of the child, or -1 if not running.
  int pid() const;

 private:
  struct Impl;
  Impl* impl_;  // non-owning pointer to platform-specific state; managed by POSIX impl
};

}  // namespace microide::platform
