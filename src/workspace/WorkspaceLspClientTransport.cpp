// LspClient::Impl transport: the I/O thread body plus the outbound send/drain
// path. Split out of WorkspaceLspClientInternal.h so the 1332-line transport
// header carries declarations and small accessors while these heavier bodies
// compile in their own TU. SendMessageImmediate keeps its exact qualified name —
// tsan.supp suppresses a benign race on it by mangled symbol.
#include "workspace/WorkspaceLspClientInternal.h"

#include <chrono>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>

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
  std::lock_guard wlock(write_mutex);
  for (QueuedMessage& queued : batch) {
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
    std::function<std::string()> build_serialized) {
  if (shutting_down.load(std::memory_order_acquire)) {
    return false;
  }
  std::lock_guard lock(send_mutex);
  if (shutting_down.load(std::memory_order_acquire)) {
    return false;
  }
  // OOM backstop: a wedged server that has stopped reading stdin. Refuse rather
  // than grow without bound or corrupt sync by dropping mid-stream messages.
  if (deferred_messages.size() + outbound_messages.size() >= kMaxQueuedMessages) {
    if (!outbound_overflow_logged_) {
      outbound_overflow_logged_ = true;
      std::fprintf(stderr,
                   "[lsp] outbound queue exceeded %zu messages; server appears wedged, "
                   "refusing further messages\n",
                   kMaxQueuedMessages);
    }
    return false;
  }
  if (!initialized.load(std::memory_order_acquire)) {
    deferred_messages.push_back(QueuedMessage{
        .serialized = {},
        .build_serialized = std::move(build_serialized),
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
  outbound_messages.push_back(QueuedMessage{
      .serialized = {},
      .build_serialized = std::move(build_serialized),
  });
  Wake();
  return true;
}

// Wait until stdout has data/EOF or an outbound wake arrives. Returns true when
// stdout should be read. On POSIX this is a single poll() over stdout + the wake
// pipe, so an idle server makes no fixed-cadence wakeups; elsewhere it degrades
// to a short read timeout (no wake fd available).
bool LspClient::Impl::WaitStdoutReadable(int timeout_ms) {
#if defined(__unix__) || defined(__APPLE__)
  if (cached_stdout_fd_ >= 0) {
    pollfd fds[2] = {};
    int nfds = 0;
    const int out_index = nfds;
    fds[nfds].fd = cached_stdout_fd_;
    fds[nfds].events = POLLIN | POLLHUP;
    ++nfds;
    int wake_index = -1;
    if (const int wake_read_fd = wake_pipe_.read_fd(); wake_read_fd >= 0) {
      wake_index = nfds;
      fds[nfds].fd = wake_read_fd;
      fds[nfds].events = POLLIN;
      ++nfds;
    }
    const int ready = ::poll(fds, nfds, timeout_ms);
    if (ready <= 0) {
      return false;  // timeout or EINTR — loop drains outbound and re-polls
    }
    if (wake_index >= 0 && (fds[wake_index].revents & POLLIN) != 0) {
      DrainWakePipe();
    }
    const short out_revents = fds[out_index].revents;
    return (out_revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0;
  }
#endif
  // Fallback: no pollable fd; let Read() block briefly and report data directly.
  return true;
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
    if (!chunk->empty()) framer_.Append(*chunk);
    if (framer_.BufferedBytes() > kMaxLspReadBufferBytes) {
      break;  // runaway server (no valid frame): tear the session down
    }
  }
  ParseBufferedMessages();
  DrainOutbound();  // final flush (e.g. an exit notification queued during stop)
  // The server is gone (EOF / exited). When this is an unexpected death rather than
  // a shutdown we requested, fail any still-pending requests so their UI loading
  // state clears instead of hanging forever.
  if (!shutting_down.load(std::memory_order_acquire)) {
    FailPendingRequests(/*only_expired=*/false);
  }
  // The io_thread is exiting (EOF / fatal read / runaway buffer). Signal that no
  // thread is draining outbound anymore so the send path refuses further requests
  // instead of stranding them — covers a server that closed stdout but is still
  // alive, where proc.IsRunning() alone would keep accepting sends. DoInitialize
  // resets this to false before spawning a fresh io_thread on restart.
  stop_io.store(true, std::memory_order_release);
}

}  // namespace microide::workspace
