// LspClient::Impl transport: the I/O thread body plus the outbound send/drain
// path. Split out of WorkspaceLspClientInternal.h so the 1332-line transport
// header carries declarations and small accessors while these heavier bodies
// compile in their own TU. SendMessageImmediate keeps its exact qualified name —
// tsan.supp suppresses a benign race on it by mangled symbol.
#include "workspace/lsp/WorkspaceLspClientInternal.h"

#include <chrono>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>

#include "util/PerformanceCounters.h"

namespace microide::workspace {

// Flush every queued outbound message. Runs only on the I/O thread; holds
// write_mutex across the proc.Write calls so it never races a shutdown-time
// SendMessageImmediate on stdin. The queue lock is released before writing so a
// main-thread enqueue never blocks behind a slow write.
void LspClient::Impl::DrainOutbound() {
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
    util::AddPerformanceCounter(util::PerfCounterId::LspMessagesSent);
    util::AddPerformanceCounter(util::PerfCounterId::LspBytesSent, queued.approx_bytes);
    if (!proc.Write(std::move(queued).TakeSerialized())) {
      SetLastError("failed to send message to language server");
      stop_io.store(true, std::memory_order_release);
      return;
    }
  }
}

bool LspClient::Impl::SendMessageImmediate(const util::JsonValue& msg, bool allow_during_shutdown,
                                           std::chrono::milliseconds lock_timeout) {
  if (!allow_during_shutdown && shutting_down.load(std::memory_order_acquire)) {
    return false;
  }
  const std::string serialized = SerializeMessage(msg);
  // Counted here as well as in DrainOutbound: this path bypasses the outbound
  // queue entirely (the initialize handshake and every shutdown-time send), so
  // counting only the drain would silently omit them.
  util::AddPerformanceCounter(util::PerfCounterId::LspMessagesSent);
  util::AddPerformanceCounter(util::PerfCounterId::LspBytesSent, serialized.size());
  if (lock_timeout > std::chrono::milliseconds::zero()) {
    // Bounded acquisition via try_lock polling (not timed_mutex::try_lock_for,
    // which ThreadSanitizer mis-models). On timeout the caller force-kills, which
    // unblocks a wedged proc.Write so the io_thread can be joined.
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

bool LspClient::Impl::SendMessageBuilderAfterInitialize(
    std::function<std::string()> build_serialized, std::size_t approx_bytes) {
  if (shutting_down.load(std::memory_order_acquire)) {
    return false;
  }
  std::lock_guard lock(send_mutex);
  if (shutting_down.load(std::memory_order_acquire)) {
    return false;
  }
  // OOM backstop: a wedged server that has stopped reading stdin. Refuse rather
  // than grow without bound or corrupt sync by dropping mid-stream messages. Bound
  // BOTH the message count and the aggregate charged bytes.
  if (deferred_messages.size() + outbound_messages.size() >= kMaxQueuedMessages ||
      outbound_queued_bytes_ + approx_bytes > max_queued_bytes_) {
    if (!outbound_overflow_logged_) {
      outbound_overflow_logged_ = true;
      std::fprintf(stderr,
                   "[lsp] outbound queue exceeded %zu messages / %zu bytes; server appears "
                   "wedged, refusing further messages\n",
                   kMaxQueuedMessages, max_queued_bytes_);
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
  // Refuse once the io_thread has stopped draining. A server that closes its
  // stdout but stays alive makes IoMain exit on EOF while proc.IsRunning() stays
  // true; without the stop_io check we would keep queuing requests no thread will
  // ever send or time out, so their callbacks (and on_send_failure) never fire.
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

// Wait until stdout has data/EOF or an outbound wake arrives. Returns true when
// stdout should be read. On POSIX this is a single poll() over stdout + the wake
// pipe, so an idle server makes no fixed-cadence wakeups; elsewhere it degrades
// to a short read timeout (no wake fd available).
bool LspClient::Impl::WaitStdoutReadable(int timeout_ms) {
  // Fetch the stdout fd fresh on every poll rather than reusing the once-captured
  // cached_stdout_fd_ — see WakePipe::PollReadableOrWake for why a cached
  // descriptor number is unsafe here. That poll is shared with the DAP client so
  // the two transports cannot drift apart again.
  return wake_pipe_.PollReadableOrWake(proc.stdout_fd(), timeout_ms);
}

void LspClient::Impl::IoMain() {
  cached_stdout_fd_ = proc.stdout_fd();
  const int read_timeout = cached_stdout_fd_ >= 0 ? 0 : 50;
  while (!stop_io.load(std::memory_order_acquire)) {
    // Parse anything already buffered first: the initialize handoff can leave
    // server-pushed messages in framer_ with no *new* stdout data behind them,
    // and a single read can carry several messages. Dispatch may enqueue replies.
    ParseBufferedMessages();
    DrainOutbound();
    // WaitStdoutReadable wakes at least once per second, so this sweeps timed-out
    // requests within ~1s even when the server sends nothing.
    if (!shutting_down.load(std::memory_order_acquire)) {
      FailPendingRequests(/*only_expired=*/true);
    }
    if (stop_io.load(std::memory_order_acquire)) {
      break;
    }
    if (!WaitStdoutReadable(1000)) {
      continue;  // woke for outbound/timeout — re-parse, re-drain, re-poll
    }
    auto chunk = proc.Read(4096, read_timeout);
    if (!chunk) break;  // EOF / fatal read error
    if (!chunk->empty()) {
      util::AddPerformanceCounter(util::PerfCounterId::LspBytesReceived, chunk->size());
      framer_.Append(*chunk);
    }
    if (framer_.BufferedBytes() > kMaxLspReadBufferBytes) {
      break;  // runaway server (no valid frame): tear the session down
    }
  }
  ParseBufferedMessages();
  DrainOutbound();  // final flush (e.g. an exit notification queued during stop)
  // The server is gone (EOF / exited). When this is an unexpected death rather than
  // a shutdown we requested, fail any still-pending requests so their UI loading
  // state clears instead of hanging forever, and — mirroring the DAP transport —
  // wake the main thread even if none were pending: an idle session with no
  // in-flight request would otherwise leave the readiness/status indicator stale
  // (the manager re-gates on IsRunning) until some unrelated SDL event drove a frame.
  if (!shutting_down.load(std::memory_order_acquire)) {
    FailPendingRequests(/*only_expired=*/false);
    main_mailbox.PushWake();
  }
  // The io_thread is exiting (EOF / fatal read / runaway buffer). Signal that no
  // thread is draining outbound anymore so the send path refuses further requests
  // instead of stranding them — covers a server that closed stdout but is still
  // alive, where proc.IsRunning() alone would keep accepting sends. DoInitialize
  // resets this to false before spawning a fresh io_thread on restart.
  stop_io.store(true, std::memory_order_release);
}

}  // namespace microide::workspace
