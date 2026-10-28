#pragma once

#include <atomic>
#include <chrono>
#include <utility>
#include <optional>
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
// answers bulk bytes with Ack frames, and the window adapts to the bandwidth those
// acks measure times (the base round trip + kQueueAllowance), between 64 KiB and
// 1 MiB -- the bandwidth-delay product, which keeps the link full, plus exactly
// the queue an echo is allowed to wait behind (the echo budget is 1 RTT + 5 ms).
// The base round trip is the smallest bulk-frame-to-ack time seen on this
// connection (TCP Vegas's baseRTT): a recent or smoothed one would include the
// very queue the window controls and grow it without bound. Before a sample
// exists the target is ~100 ms of bandwidth. That bound, not a second channel, is
// what keeps an echo from queueing behind a backfill in the network.
//
// Threading: Send and Stop from any thread. Callbacks run on the I/O thread; the
// owner marshals them to its own. on_closed runs exactly once, after which no
// frame is delivered.
class RemoteFrameTransport {
 public:
  // Bulk senders cut content into frames no larger than this, so an interactive
  // frame queued behind a bulk one waits for at most this many bytes.
  static constexpr std::size_t kMaxBulkChunkBytes = 64 * 1024;
  // The queueing an interactive frame may meet behind bulk in the network.
  static constexpr std::chrono::milliseconds kQueueAllowance{5};

  struct Callbacks {
    std::function<void(Frame)> on_frame;
    std::function<void(std::string_view reason)> on_closed;
    // On the I/O thread, first Options::tick_interval after Start (the heartbeat's
    // clock). Returns how long until it wants the next tick: the caller knows its
    // own next deadline, so an idle connection wakes once per heartbeat rather
    // than on a fixed polling tick. Unset: every tick_interval.
    std::function<std::chrono::milliseconds()> on_tick;
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
    // 0 = no ticks; else the delay to the first. The poll() wait is bounded by the
    // next tick, so an idle connection wakes only for its heartbeat.
    std::chrono::milliseconds tick_interval{0};
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

  // Wait for every frame queued so far to be written (a reply sent just before a
  // deliberate teardown). Gives up when nothing more was written for `stall`, or
  // at `limit` in all: a peer that reads slowly still gets what was queued, and
  // one that has stopped reading costs only `stall`. False when it gave up or the
  // transport closed first. Not from the I/O thread.
  bool Flush(std::chrono::milliseconds stall, std::chrono::milliseconds limit);

  // Stop the I/O thread and close owned descriptors. Idempotent. Does not run
  // on_closed if the transport had not already closed.
  void Stop();
  // End the connection from above (a dead link, a protocol error in a body):
  // reports `reason` through on_closed once and stops the I/O loop. Any thread.
  void Fail(std::string_view reason);

  bool closed() const { return closed_.load(std::memory_order_acquire); }
  // True on the I/O thread, where every callback runs. The transport must not be
  // destroyed there: its destructor joins that thread, and a thread cannot join
  // itself (the std::thread is then destroyed joinable: std::terminate).
  bool OnIoThread() const { return io_thread_.get_id() == std::this_thread::get_id(); }
  // The current bulk window (bytes of bulk payload allowed unacknowledged).
  std::size_t bulk_window() const { return bulk_window_.load(std::memory_order_relaxed); }
  // What the window is aimed from: the smallest bulk frame-to-ack time seen (0 =
  // none yet) and the smoothed bandwidth the acks measure (bytes/s). Diagnostics;
  // any thread.
  std::chrono::microseconds base_rtt() const {
    return std::chrono::microseconds(base_rtt_us_.load(std::memory_order_relaxed));
  }
  double bulk_bandwidth() const { return bulk_bandwidth_seen_.load(std::memory_order_relaxed); }
  // Pin the bulk window (`remote.backfill_inflight_bytes`); 0 returns it to the
  // adaptive bound. Any thread.
  void SetFixedBulkWindow(std::size_t bytes);

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
  std::atomic<std::size_t> fixed_bulk_window_{0};

  std::mutex queue_mutex_;  // guards the two queues and queued_bytes_
  Queue interactive_;
  Queue bulk_;
  std::size_t queued_bytes_ = 0;
  bool overflowed_ = false;

  // I/O thread only.
  std::string writing_;
  std::size_t writing_offset_ = 0;
  // Bytes accepted by Send and not yet fully written (queued or in writing_).
  std::atomic<std::size_t> unwritten_{0};
  FrameDecoder decoder_;
  std::uint64_t bulk_bytes_sent_ = 0;   // bulk payload bytes put on the wire
  std::uint64_t bulk_bytes_acked_ = 0;  // ... and acknowledged by the peer
  std::uint64_t bulk_bytes_received_ = 0;
  std::uint64_t bulk_bytes_received_acked_ = 0;
  std::chrono::steady_clock::time_point last_ack_time_{};
  std::chrono::steady_clock::time_point next_tick_{};
  std::uint64_t last_ack_bytes_ = 0;
  // Bulk frames on the wire: (bulk_bytes_sent_ after the frame, when it went), for
  // the frame-to-ack round trip. I/O thread only.
  std::deque<std::pair<std::uint64_t, std::chrono::steady_clock::time_point>> bulk_in_flight_;
  std::optional<std::chrono::steady_clock::duration> base_rtt_;
  double bulk_bandwidth_ = 0.0;  // smoothed bytes/s the acks measure
  std::atomic<std::int64_t> base_rtt_us_{0};       // base_rtt_, published
  std::atomic<double> bulk_bandwidth_seen_{0.0};   // bulk_bandwidth_, published
};

}  // namespace microide::project::remote
