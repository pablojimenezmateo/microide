#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace microide::platform {

// One inbound JSONL line tagged with the connection it arrived on, so the host
// can route the reply back to the right client.
struct ControlInboundMessage {
  std::uint64_t connection_id = 0;
  std::string line;  // raw line, newline stripped
};

// Result of scanning a connection read buffer for complete newline-delimited
// request lines. Pure, platform-independent, and unit-testable: the byte-cap
// decision lives here rather than being applied only to the residual trailing
// line after the complete lines were already copied out.
struct ControlRequestLineScan {
  // Complete, non-empty lines (trailing '\n' and any trailing '\r' stripped),
  // each guaranteed <= max_line_bytes. Scanning stops before the first complete
  // line that exceeds the cap.
  std::vector<std::string> lines;
  // Prefix of the input fully consumed (safe to erase from the front of the read
  // buffer). Does NOT include a rejected over-cap line — the caller sheds the
  // connection instead of advancing past it.
  std::size_t consumed_bytes = 0;
  // A complete newline-terminated line exceeded max_line_bytes. The caller must
  // shed the connection: a hostile local peer must not queue an unbounded line
  // just because it ended it with a newline.
  bool oversize_line = false;
};

// Split `buffer` into complete request lines, rejecting any complete line whose
// raw length exceeds `max_line_bytes` before copying it. Trailing incomplete
// bytes (no newline yet) are left unconsumed for the next read.
ControlRequestLineScan ScanControlRequestLines(std::string_view buffer,
                                               std::size_t max_line_bytes);

// A single-threaded AF_UNIX line server for the control channel. One background
// I/O thread polls the listen socket plus every client fd; inbound lines are
// queued for the main thread to drain (TakeInbound) and a host-supplied SDL wake
// event is pushed so the main loop wakes. Replies (SendLine) and broadcasts
// (events) are queued from the main thread and flushed by the I/O thread.
//
// Mirrors the DAP client's marshaling discipline: background thread never
// touches host state, only this object's mutex-guarded queues.
class ControlSocketServer {
 public:
  ControlSocketServer();
  ~ControlSocketServer();
  ControlSocketServer(const ControlSocketServer&) = delete;
  ControlSocketServer& operator=(const ControlSocketServer&) = delete;

  // Bind + listen on `socket_path` (created 0600). Removes a stale socket file
  // first. Returns false on failure (already running counts as failure).
  bool Start(const std::filesystem::path& socket_path);
  bool IsRunning() const;
  // Idempotent: stops the I/O thread, closes all fds, unlinks the socket file.
  void Stop();

  // SDL event id pushed when inbound lines are ready. 0 disables the push.
  void SetWakeEventType(std::uint32_t event_type);

  // Main thread: take and clear all queued inbound messages.
  std::vector<ControlInboundMessage> TakeInbound();
  // Main thread: queue a line to one connection / to every connection. A '\n'
  // terminator is appended automatically.
  void SendLine(std::uint64_t connection_id, const std::string& line);
  void Broadcast(const std::string& line);

  std::size_t ConnectionCount() const;

  // Main thread: true (once) when the I/O thread re-bound the listener after the
  // advertised socket file vanished out from under a live process (external
  // $XDG_RUNTIME_DIR cleanup). The host re-writes its discovery descriptor in
  // response. Self-clears on read.
  bool ConsumeRebound();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace microide::platform
