#pragma once

#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "util/PerformanceCounters.h"
#include "util/WakePipe.h"
#include "workspace/JsonRpcMessageFraming.h"
#include "workspace/StdioClientQueue.h"
#include "platform/AsyncSubprocess.h"

namespace microide::workspace {

// Everything a stdio JSON-RPC client's transport half needs, shared by the LSP
// and DAP clients. The two spoke byte-identical wire framing and drove
// byte-identical outbound queues, yet each carried its own copy of the I/O
// thread body, the drain, the bounded shutdown-time send and the queue caps —
// so hardening one and not the other left half the picture (the framer had
// already drifted exactly that way once: see JsonRpcMessageFraming.h). This
// mixin is the rest of that extraction: one implementation, two protocols.
//
// CRTP rather than virtuals: the derived Impl supplies the protocol half —
//   void DispatchMessage(util::JsonValue msg);        // route one inbound frame
//   void FailPendingRequests(bool only_expired);      // sweep / fail-all
//   void SetLastError(std::string message);           // surface a transport error
//   void TraceSend(const util::JsonValue& msg);       // per-message send trace (may be a no-op)
// and the base calls them with zero indirection on the per-message path.
//
// Threading contract (unchanged from the two originals, and the reason the
// members live together): `send_mutex` guards the queues + byte budget;
// `write_mutex` serializes every proc.Write so stdin is never written by two
// threads; the io_thread blocks in poll() over stdout + a self-pipe wake, so an
// idle peer makes no fixed-cadence wakeups. The shutdown path bounds its
// write_mutex acquisition with a plain-mutex try_lock poll rather than
// std::timed_mutex::try_lock_for, which GCC 13's libtsan mis-models (spurious
// "unlock of an unlocked mutex").
struct StdioTransportConfig {
  util::PerfCounterId messages_sent;
  util::PerfCounterId bytes_sent;
  util::PerfCounterId bytes_received;
  std::size_t max_message_bytes = kDefaultMaxJsonRpcMessageBytes;
  // Runaway-peer bound on the unframed read buffer: exceeding it tears the
  // session down rather than growing without bound.
  std::size_t max_read_buffer_bytes = kDefaultMaxJsonRpcMessageBytes + (1ull * 1024 * 1024);
  std::size_t max_queued_messages = 50000;
  std::size_t max_queued_bytes = 512u * 1024 * 1024;
  // "lsp" / "dap": prefixes the wedged-peer overflow log line.
  const char* peer_label = "?";
  // SetLastError text for a failed stdin write.
  const char* send_error = "failed to send message to peer";
};

template <typename Derived>
class StdioJsonRpcClientTransport {
 public:
  using QueuedMessage = StdioQueuedMessage;

  explicit StdioJsonRpcClientTransport(const StdioTransportConfig& config)
      : transport_config_(config),
        framer_{.max_message_bytes = config.max_message_bytes},
        max_queued_bytes_(config.max_queued_bytes) {}

  platform::AsyncSubprocess proc;
  std::mutex send_mutex;  // guards the outbound/deferred queues + byte budget
  // Serializes every proc.Write so stdin is never written by two threads. The
  // shutdown path bounds its acquisition (see SendMessageImmediate): the io_thread
  // can hold this across a proc.Write that blocks indefinitely on a
  // wedged-but-alive peer, and the shutdown's force-kill (which unblocks that
  // write) must not be gated behind acquiring this lock.
  std::mutex write_mutex;

  StdioTransportConfig transport_config_;

  // Filled first by the initialize handshake and then handed to the I/O thread;
  // keeping it a member preserves any bytes the peer pushed right after the
  // initialize response (and any partial frame) across that handoff.
  JsonRpcMessageFramer framer_;
  std::thread io_thread;
  std::atomic<bool> stop_io{false};
  util::WakePipe wake_pipe_;  // self-pipe that breaks the I/O thread's poll() on demand
  int cached_stdout_fd_ = -1;

  std::atomic<bool> shutting_down{false};
  std::atomic<bool> process_shutdown_started{false};
  std::atomic<bool> initialized{false};
  // When true, the client behaves as a connected peer for unit tests (no subprocess).
  std::atomic<bool> test_stub_mode{false};

  std::deque<QueuedMessage> deferred_messages;
  std::deque<QueuedMessage> outbound_messages;
  // Aggregate approximate bytes across deferred_messages + outbound_messages,
  // guarded by send_mutex. Charged at enqueue, released when a batch drains / the
  // queues are cleared. Bounds retained payload under the message-count cap
  // (TD-2026-07-17A-071).
  std::size_t outbound_queued_bytes_ = 0;
  // Effective byte budget (config.max_queued_bytes in production; lowered in tests).
  std::size_t max_queued_bytes_ = 0;
  // One-shot guard so the wedged-peer overflow warning logs once, not per message.
  bool outbound_overflow_logged_ = false;

  // --- self-pipe wakeup -----------------------------------------------------
  void OpenWakePipe() { wake_pipe_.Open(); }
  void CloseWakePipe() { wake_pipe_.Close(); }
  void Wake() { wake_pipe_.Wake(); }

  // Single funnel for every outbound message (requests, handshakes, and
  // reverse-request responses): trace, serialize, frame.
  std::string SerializeMessage(const util::JsonValue& msg) const {
    derived().TraceSend(msg);
    const std::string json = util::SerializeJson(msg);
    std::string framed;
    framed.reserve(32 + json.size());
    framed += "Content-Length: ";
    framed += std::to_string(json.size());
    framed += "\r\n\r\n";
    framed += json;
    return framed;
  }

  // Flush every queued outbound message. Runs only on the I/O thread; holds
  // write_mutex across the proc.Write calls so it never races a shutdown-time
  // SendMessageImmediate on stdin. The queue lock is released before writing so a
  // main-thread enqueue never blocks behind a slow write.
  void DrainOutbound() {
    std::deque<QueuedMessage> batch;
    {
      std::lock_guard lock(send_mutex);
      batch.swap(outbound_messages);
    }
    if (batch.empty()) {
      return;
    }
    // Release the drained batch's charged bytes (the swap emptied outbound_messages).
    {
      std::size_t drained_bytes = 0;
      for (const QueuedMessage& queued : batch) {
        drained_bytes += queued.approx_bytes;
      }
      std::lock_guard lock(send_mutex);
      outbound_queued_bytes_ -= std::min(outbound_queued_bytes_, drained_bytes);
    }
    std::lock_guard wlock(write_mutex);
    for (QueuedMessage& queued : batch) {
      util::AddPerformanceCounter(transport_config_.messages_sent);
      util::AddPerformanceCounter(transport_config_.bytes_sent, queued.approx_bytes);
      if (!proc.Write(std::move(queued).TakeSerialized())) {
        derived().SetLastError(transport_config_.send_error);
        stop_io.store(true, std::memory_order_release);
        return;
      }
    }
  }

  // lock_timeout > 0 bounds how long we wait for write_mutex before giving up:
  // the io_thread can be stuck in a proc.Write to a wedged-but-alive peer while
  // holding it, so a shutdown-path send must not block teardown forever. On
  // timeout it returns false and the caller falls through to the force-kill
  // (which unblocks that write so the io_thread can be joined).
  bool SendMessageImmediate(const util::JsonValue& msg, bool allow_during_shutdown = false,
                            std::chrono::milliseconds lock_timeout = std::chrono::milliseconds::zero()) {
    if (!allow_during_shutdown && shutting_down.load(std::memory_order_acquire)) {
      return false;
    }
    const std::string serialized = SerializeMessage(msg);
    // Counted here as well as in DrainOutbound: this path bypasses the outbound
    // queue entirely (the initialize handshake and every shutdown-time send), so
    // counting only the drain would silently omit them.
    util::AddPerformanceCounter(transport_config_.messages_sent);
    util::AddPerformanceCounter(transport_config_.bytes_sent, serialized.size());
    if (lock_timeout > std::chrono::milliseconds::zero()) {
      // Bounded acquisition via try_lock polling (not timed_mutex::try_lock_for,
      // which ThreadSanitizer mis-models). On timeout the caller force-kills,
      // which unblocks a wedged proc.Write so the io_thread can be joined.
      std::unique_lock<std::mutex> wlock(write_mutex, std::defer_lock);
      const auto deadline = std::chrono::steady_clock::now() + lock_timeout;
      while (!wlock.try_lock()) {
        if (std::chrono::steady_clock::now() >= deadline) {
          return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      return proc.Write(serialized);
    }
    std::lock_guard wlock(write_mutex);
    return proc.Write(serialized);
  }

  // Defer serialization to the I/O thread: move the message into the outbound
  // builder so SerializeMessage (const, no shared state) runs during
  // DrainOutbound rather than on the calling (often UI) thread. Ordering is
  // fixed at enqueue time under send_mutex, so lazy serialization never
  // reorders. The shutdown/initialize paths use SendMessageImmediate and stay
  // eager. (TD-2026-07-16-25.)
  bool SendMessageAfterInitialize(util::JsonValue msg) {
    return SendMessageBuilderAfterInitialize(
        [this, m = std::move(msg)]() { return SerializeMessage(m); });
  }

  // Queue a message for the I/O thread (deferred until initialized). Refuses on
  // a wedged/dead peer rather than growing unbounded. `approx_bytes` is the
  // payload's approximate size, charged against the aggregate outbound byte
  // budget (0 for small control frames).
  bool SendMessageBuilderAfterInitialize(std::function<std::string()> build_serialized,
                                         std::size_t approx_bytes = 0) {
    if (shutting_down.load(std::memory_order_acquire)) {
      return false;
    }
    std::lock_guard lock(send_mutex);
    if (shutting_down.load(std::memory_order_acquire)) {
      return false;
    }
    // OOM backstop: a wedged peer that has stopped reading stdin. Refuse rather
    // than grow without bound or corrupt sync by dropping mid-stream messages.
    // Bound BOTH the message count and the aggregate charged bytes.
    if (deferred_messages.size() + outbound_messages.size() >= transport_config_.max_queued_messages ||
        outbound_queued_bytes_ + approx_bytes > max_queued_bytes_) {
      if (!outbound_overflow_logged_) {
        outbound_overflow_logged_ = true;
        std::fprintf(stderr,
                     "[%s] outbound queue exceeded %zu messages / %zu bytes; peer appears "
                     "wedged, refusing further messages\n",
                     transport_config_.peer_label, transport_config_.max_queued_messages,
                     max_queued_bytes_);
      }
      return false;
    }
    if (!initialized.load(std::memory_order_acquire)) {
      outbound_queued_bytes_ += approx_bytes;
      deferred_messages.push_back(QueuedMessage{
          .serialized = {},
          .build_serialized = std::move(build_serialized),
          .approx_bytes = approx_bytes,
      });
      return true;
    }
    if (test_stub_mode.load(std::memory_order_acquire)) {
      return true;
    }
    // Refuse once the io_thread has stopped draining. A peer that closes its
    // stdout but stays alive makes IoMain exit on EOF while proc.IsRunning()
    // stays true; without the stop_io check we would keep queuing requests no
    // thread will ever send or time out, so their callbacks (and the UI loading
    // state) hang.
    if (stop_io.load(std::memory_order_acquire) || !proc.IsRunning()) {
      return false;
    }
    outbound_queued_bytes_ += approx_bytes;
    outbound_messages.push_back(QueuedMessage{
        .serialized = {},
        .build_serialized = std::move(build_serialized),
        .approx_bytes = approx_bytes,
    });
    Wake();
    return true;
  }

  void ClearDeferredMessages() {
    std::lock_guard lock(send_mutex);
    deferred_messages.clear();
    outbound_messages.clear();
    outbound_queued_bytes_ = 0;
  }

  void ShutdownProcessOnce(int timeout_ms = 3000) {
    bool expected = false;
    if (!process_shutdown_started.compare_exchange_strong(expected, true,
                                                          std::memory_order_acq_rel)) {
      return;
    }
    proc.Shutdown(timeout_ms);
  }

  // Wait until stdout has data/EOF or an outbound wake arrives. Returns true when
  // stdout should be read. On POSIX this is a single poll() over stdout + the
  // wake pipe, so an idle peer makes no fixed-cadence wakeups; elsewhere it
  // degrades to a short read timeout (no wake fd available). Fetch the stdout fd
  // fresh on every poll rather than reusing the once-captured cached_stdout_fd_
  // — see WakePipe::PollReadableOrWake for why a cached descriptor number is
  // unsafe here.
  bool WaitStdoutReadable(int timeout_ms) {
    return wake_pipe_.PollReadableOrWake(proc.stdout_fd(), timeout_ms);
  }

  void ParseBufferedMessages() {
    while (true) {
      const std::size_t buffered_before = framer_.BufferedBytes();
      auto msg_opt = framer_.Next();
      if (msg_opt) {
        derived().DispatchMessage(std::move(*msg_opt));
        continue;
      }
      // nullopt is overloaded: either the framer consumed a frame it could not
      // surface (malformed header / oversized / invalid JSON) — in which case any
      // well-formed frames already buffered behind it must still be drained now,
      // not ~1s later on the next poll — or no complete frame is available yet,
      // in which case the buffer is unchanged and we wait for more bytes.
      if (framer_.BufferedBytes() == buffered_before) {
        break;
      }
    }
  }

  // The single I/O thread body: parse buffered frames, drain outbound, sweep
  // timed-out requests, then poll stdout.
  void IoMain() {
    cached_stdout_fd_ = proc.stdout_fd();
    const int read_timeout = cached_stdout_fd_ >= 0 ? 0 : 50;
    while (!stop_io.load(std::memory_order_acquire)) {
      // Parse anything already buffered first: the initialize handoff can leave
      // peer-pushed messages in framer_ with no *new* stdout data behind them,
      // and a single read can carry several messages. Dispatch may enqueue replies.
      ParseBufferedMessages();
      DrainOutbound();
      // WaitStdoutReadable wakes at least once per second, so this sweeps
      // timed-out requests within ~1s even when the peer sends nothing.
      if (!shutting_down.load(std::memory_order_acquire)) {
        derived().FailPendingRequests(/*only_expired=*/true);
      }
      if (stop_io.load(std::memory_order_acquire)) {
        break;
      }
      if (!WaitStdoutReadable(1000)) {
        continue;  // woke for outbound/timeout — re-parse, re-drain, re-poll
      }
      auto chunk = proc.Read(4096, read_timeout);
      if (!chunk) {
        break;  // EOF / fatal read error
      }
      if (!chunk->empty()) {
        util::AddPerformanceCounter(transport_config_.bytes_received, chunk->size());
        framer_.Append(*chunk);
      }
      if (framer_.BufferedBytes() > transport_config_.max_read_buffer_bytes) {
        break;  // runaway peer (no valid frame): tear the session down
      }
    }
    ParseBufferedMessages();
    DrainOutbound();  // final flush (e.g. an exit notification queued during stop)
    // The peer is gone (EOF / exited / killed). When this is an unexpected death
    // rather than a shutdown we requested, fail any still-pending requests so
    // their UI loading state clears instead of hanging forever, and wake the main
    // thread even if none were pending: an idle session with no in-flight request
    // would otherwise leave readiness/session state stale until some unrelated
    // event drove a frame. OnPeerGone is the derived client's "fail everything +
    // PushWake" hook.
    if (!shutting_down.load(std::memory_order_acquire)) {
      derived().FailPendingRequests(/*only_expired=*/false);
      derived().WakeMainThread();
    }
    // The io_thread is exiting (EOF / fatal read / runaway buffer). Signal that
    // no thread is draining outbound anymore so the send path refuses further
    // requests instead of stranding them — covers a peer that closed stdout but
    // is still alive, where proc.IsRunning() alone would keep accepting sends.
    // The initialize path resets this to false before spawning a fresh io_thread
    // on restart.
    stop_io.store(true, std::memory_order_release);
  }

 private:
  Derived& derived() { return static_cast<Derived&>(*this); }
  const Derived& derived() const { return static_cast<const Derived&>(*this); }
};

}  // namespace microide::workspace
