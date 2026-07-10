#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

namespace microide::platform {

// A blocking one-shot AF_UNIX line client for the control channel — the
// counterpart to ControlSocketServer used by `microide control-send`. It keeps
// the connection open while reading replies (the server's graceful-close logic
// makes a half-closed client still receive its reply), and buffers across reads
// so callers get one newline-delimited line at a time.
//
// Unsupported on non-POSIX platforms: Connect returns false there.
class ControlSocketClient {
 public:
  ControlSocketClient() = default;
  ~ControlSocketClient();
  ControlSocketClient(const ControlSocketClient&) = delete;
  ControlSocketClient& operator=(const ControlSocketClient&) = delete;

  // Connect to the socket at `socket_path`. Returns false on failure.
  bool Connect(const std::filesystem::path& socket_path);

  // Send one line; a '\n' terminator is appended. The write honors an overall
  // `timeout`: a peer that accepts the connection but never reads (filling the
  // socket buffer) makes SendLine return false at the deadline instead of blocking
  // forever. An over-large line is rejected before framing. Returns false on write
  // error, timeout, or oversize.
  bool SendLine(const std::string& line,
                std::chrono::milliseconds timeout = std::chrono::seconds(10));

  // Half-close the write side: signals the server we will send nothing more, so
  // it can reap us as soon as it has flushed our replies. Safe to call once.
  void ShutdownWrite();

  // Read the next newline-delimited line, waiting up to `timeout`. Returns
  // nullopt on timeout or on EOF with no further complete line. Leftover bytes
  // are buffered for the next call.
  std::optional<std::string> ReadLine(std::chrono::milliseconds timeout);

  void Close();

 private:
  int fd_ = -1;
  std::string read_buf_;
};

}  // namespace microide::platform
