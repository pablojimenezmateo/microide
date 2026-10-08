#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "project/remote/RemoteFrame.h"
#include "util/WakePipe.h"

namespace microide::project::remote {

// One connection's frames over a pair of file descriptors — the client's pipes to
// `ssh … microide-server attach`, the server's stdin/stdout, or a socket. It keeps
// the discipline of the stdio JSON-RPC transport (one poll() I/O thread, bounded
// outbound queues, wedged-peer teardown, perf counters) and replaces its codec
// with RemoteFrame (dev-docs/design/remote-projects.md § 6.4).
//
// Two lanes. The writer always drains the interactive queue first and re-checks it
// between every frame, so a keystroke waits for at most the ONE bulk frame already
// on the wire — which is why bulk senders chunk (kMaxBulkChunkBytes). And bulk may
// have at most bulk_window() payload bytes unacknowledged: the receiver's transport
// answers bulk bytes with Ack frames, and the window adapts to ~100 ms of the
// bandwidth those acks measure, between 64 KiB and 1 MiB. That bound, not a second
// channel, is what keeps an echo from queueing behind a backfill in the network.
//
// Threading: Send and Stop from any thread. Callbacks run on the I/O thread; the
// owner marshals them to its own. on_closed runs exactly once, after which no
// frame is delivered.
class RemoteFrameTransport {
 public:
  // Bulk senders cut content into frames no larger than this, so an interactive
  // frame queued behind a bulk one waits for at most this many bytes.
  static constexpr std::size_t kMaxBulkChunkBytes = 64 * 1024;

  struct Callbacks {
    std::function<void(Frame)> on_frame;
    std::function<void(std::string_view reason)> on_closed;
  };

  struct Options {
    // Bytes queued but not yet written, both lanes together. Exceeding it means the
    // peer has stopped reading; the connection is torn down rather than grown.
    std::size_t max_queued_bytes = 256u * 1024 * 1024;
    std::size_t bulk_window_floor = 64 * 1024;
    std::size_t bulk_window_cap = 1024 * 1024;
    std::size_t bulk_window_initial = 256 * 1024;
    // The receiver acks after this many bulk payload bytes, or when it has read
    // everything available, whichever is first.
    std::size_t ack_every_bytes = 32 * 1024;
    // Close the descriptors on Stop (false when someone else owns them).
    bool owns_fds = true;
  };

  RemoteFrameTransport() = default;
  ~RemoteFrameTransport();
  RemoteFrameTransport(const RemoteFrameTransport&) = delete;
  RemoteFrameTransport& operator=(const RemoteFrameTransport&) = delete;

  // Make both descriptors non-blocking and start the I/O thread. The two may be
  // the same descriptor (a socket).
  [[nodiscard]] bool Start(int read_fd, int write_fd, Callbacks callbacks, Options options);
  [[nodiscard]] bool Start(int read_fd, int write_fd, Callbacks callbacks) {
    return Start(read_fd, write_fd, std::move(callbacks), Options{});
  }

  // Queue a frame. False when the transport is closed, the payload is over the
  // frame ceiling, or queuing it would exceed max_queued_bytes (which also closes
  // the transport — a peer that does not read is a dead peer). Ack is internal.
  bool Send(FrameType type, Lane lane, std::uint64_t id, std::string_view payload);

  // Stop the I/O thread and close owned descriptors. Idempotent. Does not run
  // on_closed if the transport had not already closed.
  void Stop();

  bool closed() const { return closed_.load(std::memory_order_acquire); }
  // The current bulk window (bytes of bulk payload allowed unacknowledged).
  std::size_t bulk_window() const { return bulk_window_.load(std::memory_order_relaxed); }

 private:
  struct Queue {
    std::deque<std::string> frames;  // encoded
    std::deque<std::size_t> bulk_payload_bytes;  // bulk lane only, parallel to frames
  };

  void Run();
  void Close(std::string_view reason);
  // I/O thread: write until the descriptor would block. False on a write error.
  bool FlushWrites();
  // I/O thread: read what is available and dispatch. False on EOF/error/protocol error.
  bool ReadAvailable();
  void HandleAck(std::string_view payload);
  void QueueAck();
  bool HasWritable();

  int read_fd_ = -1;
  int write_fd_ = -1;
  Callbacks callbacks_;
  Options options_;
  std::thread io_thread_;
  util::WakePipe wake_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> closed_{false};
  std::atomic<bool> close_reported_{false};
  std::atomic<std::size_t> bulk_window_{0};

  std::mutex queue_mutex_;  // guards the two queues and queued_bytes_
  Queue interactive_;
  Queue bulk_;
  std::size_t queued_bytes_ = 0;
  bool overflowed_ = false;

  // I/O thread only.
  std::string writing_;
  std::size_t writing_offset_ = 0;
  FrameDecoder decoder_;
  std::uint64_t bulk_bytes_sent_ = 0;   // bulk payload bytes put on the wire
  std::uint64_t bulk_bytes_acked_ = 0;  // ... and acknowledged by the peer
  std::uint64_t bulk_bytes_received_ = 0;
  std::uint64_t bulk_bytes_received_acked_ = 0;
  std::chrono::steady_clock::time_point last_ack_time_{};
  std::uint64_t last_ack_bytes_ = 0;
};

}  // namespace microide::project::remote
