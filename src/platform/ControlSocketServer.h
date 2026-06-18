#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace microide::platform {

// One inbound JSONL line tagged with the connection it arrived on, so the host
// can route the reply back to the right client.
struct ControlInboundMessage {
  std::uint64_t connection_id = 0;
  std::string line;  // raw line, newline stripped
};

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

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace microide::platform
