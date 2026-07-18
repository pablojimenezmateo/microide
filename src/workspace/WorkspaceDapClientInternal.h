#pragma once

#include "workspace/DapProtocol.h"
#include "workspace/WorkspaceDapClient.h"
#include "workspace/ProtocolNumeric.h"
#include "workspace/StdioClientQueue.h"
#include "util/DebugTrace.h"
#include "util/MainThreadMailbox.h"
#include "util/WakePipe.h"

#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>
#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

namespace microide::workspace {

namespace {

// A DAP request that gets no response within its deadline is failed
// synthetically, so a wedged or silent adapter can never leave a request — and the
// UI's loading state — pending forever. The deadline is command-aware: startup
// requests can legitimately take many seconds (loading symbols, launching a large
// program), while a value fetch on a paused target should be near-instant. A slow
// value fetch is the runaway signature (gdb formatting a garbage container), so we
// fail it fast — both to surface "<unavailable>" promptly and to bound how long
// the adapter churns before we stop waiting on it.
constexpr std::chrono::milliseconds kStartupRequestTimeout{30000};
constexpr std::chrono::milliseconds kInteractiveRequestTimeout{6000};

// Longer deadline only for the handshake/run-control requests that can be
// genuinely slow; everything else (variables/evaluate/scopes/stackTrace/
// setVariable/…) uses the short interactive deadline.
inline std::chrono::milliseconds RequestTimeoutFor(const std::string& command) {
  if (command == "launch" || command == "attach" || command == "initialize" ||
      command == "configurationDone" || command == "restart") {
    return kStartupRequestTimeout;
  }
  return kInteractiveRequestTimeout;
}

// An inbound DAP `request_seq` only matches one of OUR pending requests if it is an
// exact integer within int range (our seqs are a small monotonic counter). A value
// outside int range from a hostile/buggy adapter cannot match anything we sent, so
// returning false skips it — mirrors the LSP transport's ResponseIdInRange. A bare
// `static_cast<int>(AsInt())` would wrap e.g. 4294967297 to 1 and could collide with a
// live pending seq, dispatching the wrong callback or forging an initialize match.
inline bool DapResponseSeqInRange(const util::JsonValue& seq_value, int* out) {
  // Our seqs are exact integers; reject a fractional double (5.9) instead of
  // truncating it into a live pending seq (TD-2026-07-17A-117).
  if (!protocol_numeric::IsIntegralJsonNumber(seq_value)) {
    return false;
  }
  const std::int64_t raw = seq_value.AsInt();
  if (raw < static_cast<std::int64_t>(std::numeric_limits<int>::min()) ||
      raw > static_cast<std::int64_t>(std::numeric_limits<int>::max())) {
    return false;
  }
  *out = static_cast<int>(raw);
  return true;
}

// OOM backstop for the outbound/deferred queues. The I/O thread drains outbound
// continuously, so this is never approached in normal operation; the queues only grow
// without bound if the adapter stops reading its stdin while staying alive. At that
// point the session is wedged, so we refuse further messages rather than silently DROP
// queued protocol messages or grow until the IDE OOMs. Refused requests fail cleanly
// via the send-failure path; the timeout sweep clears anything already pending.
constexpr std::size_t kMaxQueuedMessages = 50000;

// Debug adapters are external, possibly-buggy or hostile processes. Bound a
// single decoded message body and, with slack, the whole read-accumulation
// buffer, so an adapter cannot declare a near-INT_MAX Content-Length or stream
// unframed bytes to grow the buffer without limit (OOM). A body over the message
// cap is rejected; a read buffer past the buffer cap tears the session down.
constexpr std::size_t kMaxDapMessageBytes = 64ull * 1024 * 1024;
constexpr std::size_t kMaxDapReadBufferBytes = kMaxDapMessageBytes + (1ull * 1024 * 1024);
// TD-2026-07-17A-098: cap the pre-initialize replay buffer. Individual frames and the read
// buffer are bounded, and the wall-clock deadline prevents infinite waiting, but a hostile
// adapter can stream many valid small output/event frames for the full timeout window and
// grow `early_messages` without an item budget. A conforming adapter emits only a handful
// of events before its initialize response, so exceeding this count is a runaway adapter.
constexpr std::size_t kMaxDapEarlyMessages = 10000;

bool DapLifecycleTraceEnabled() {
  static const bool enabled = []() {
    const char* value = std::getenv("MICROIDE_TRACE_DAP_LIFECYCLE");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
  }();
  return enabled;
}

void TraceDapLifecycle(std::string_view adapter_id, int pid, std::string_view phase,
                       std::string_view detail = {}) {
  if (!DapLifecycleTraceEnabled()) {
    return;
  }
  if (detail.empty()) {
    std::fprintf(stderr, "[dap] %.*s pid=%d %.*s\n", static_cast<int>(adapter_id.size()),
                 adapter_id.data(), pid, static_cast<int>(phase.size()), phase.data());
    return;
  }
  std::fprintf(stderr, "[dap] %.*s pid=%d %.*s | %.*s\n", static_cast<int>(adapter_id.size()),
               adapter_id.data(), pid, static_cast<int>(phase.size()), phase.data(),
               static_cast<int>(detail.size()), detail.data());
}

}  // namespace

// Internal buffer — avoids O(n) prefix-erasure on every framed read.
struct DapReadBuf {
  std::string data;
  std::size_t pos = 0;

  std::string_view view() const { return std::string_view(data).substr(pos); }

  void consume(std::size_t n) {
    pos += n;
    if (pos > 65536 && pos > data.size() / 2) {
      data.erase(0, pos);
      pos = 0;
    }
  }

  void append(std::string_view chunk) { data.append(chunk); }
};

struct DapClient::Impl {
  using QueuedMessage = StdioQueuedMessage;

  platform::AsyncSubprocess proc;
  std::mutex mutex;
  std::mutex send_mutex;
  // Serializes every proc.Write so stdin is never written by two threads. The
  // shutdown path bounds its acquisition (see SendMessageImmediate): the io_thread
  // can hold this across a proc.Write that blocks indefinitely on a wedged-but-alive
  // adapter, and the shutdown's force-kill (which unblocks that write) must not be
  // gated behind acquiring this lock. A plain std::mutex with a try_lock poll rather
  // than std::timed_mutex::try_lock_for: ThreadSanitizer models try_lock correctly
  // but not pthread_mutex_timedlock, which otherwise yields spurious
  // "unlock of an unlocked mutex" reports. Mirrors the LSP client.
  std::mutex write_mutex;

  // Single I/O thread: reads stdout / writes stdin, blocking in poll() over
  // stdout + a self-pipe wake, so an idle adapter makes no fixed-cadence wakeups.
  // io_buf is filled by the initialize handshake and handed to the I/O thread,
  // preserving any events the adapter pushed right after the initialize response.
  DapReadBuf io_buf;
  // Remaining body bytes to drain-and-discard for a frame whose declared
  // Content-Length exceeded kMaxDapMessageBytes; see TryParseOneMessage.
  std::size_t skip_body_bytes_ = 0;
  std::thread io_thread;
  std::atomic<bool> stop_io{false};
  util::WakePipe wake_pipe_;  // self-pipe that breaks the I/O thread's poll() on demand
  int cached_stdout_fd_ = -1;

  std::thread init_thread;
  std::atomic<bool> initializing{false};
  std::atomic<bool> stop_init{false};
  std::thread shutdown_thread;
  std::atomic<bool> shutdown_started{false};
  std::atomic<bool> shutdown_complete{false};
  std::atomic<bool> shutting_down{false};
  std::atomic<bool> process_shutdown_started{false};
  std::condition_variable shutdown_cv;
  bool shutdown_response_received = false;
  int shutdown_request_seq = 0;

  // A registered request awaiting its response: the raw response handler plus the
  // deadline after which the request is failed synthetically.
  struct PendingRequest {
    std::function<void(util::JsonValue)> callback;
    std::chrono::steady_clock::time_point deadline;
  };
  // Pending request callbacks keyed by request seq, guarded by mutex.
  std::unordered_map<int, PendingRequest> pending_requests;
  // Callbacks marshalled to the main thread and drained once per frame. Carries
  // its own mutex + SDL wake event, so it is the innermost lock: producers may
  // post while holding `mutex`, and DrainCallbacks() never takes `mutex`.
  util::MainThreadMailbox main_mailbox;

  std::atomic<bool> test_stub_mode{false};
  std::function<void(const std::string&, const util::JsonValue&, ResponseCallback)>
      test_request_handler;

  EventCallback event_callback;

  std::deque<QueuedMessage> deferred_messages;
  std::deque<QueuedMessage> outbound_messages;
  // One-shot guard so the wedged-adapter overflow warning logs once, not per message.
  bool outbound_overflow_logged_ = false;
  int next_seq = 1;
  std::string adapter_id;
  std::string last_error;
  std::string last_error_snapshot;
  dap_protocol::DapCapabilities capabilities;
  std::atomic<bool> initialized{false};

  void SetLastError(std::string message) {
    std::lock_guard lock(mutex);
    last_error = std::move(message);
  }

  int GetNextSeq() {
    std::lock_guard lock(mutex);
    return next_seq++;
  }

  std::string SerializeMessage(const util::JsonValue& msg) const {
    // Single funnel for every outbound message (requests, the initialize /
    // disconnect handshake, and reverse-request responses).
    util::DebugTrace::Message("send", msg);
    const std::string json = util::SerializeJson(msg);
    std::string framed;
    framed.reserve(32 + json.size());
    framed += "Content-Length: ";
    framed += std::to_string(json.size());
    framed += "\r\n\r\n";
    framed += json;
    return framed;
  }

  // --- self-pipe wakeup -----------------------------------------------------
  // The I/O thread polls wake_pipe_.read_fd() alongside stdout; Wake() breaks that
  // poll immediately. See util::WakePipe.
  void OpenWakePipe() { wake_pipe_.Open(); }
  void CloseWakePipe() { wake_pipe_.Close(); }
  void Wake() { wake_pipe_.Wake(); }
  void DrainWakePipe() { wake_pipe_.Drain(); }

  // --- outbound queue (I/O thread only) -------------------------------------
  void DrainOutbound() {
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
        SetLastError("failed to send message to debug adapter");
        stop_io.store(true, std::memory_order_release);
        return;
      }
    }
  }

  // lock_timeout > 0 bounds how long we wait for write_mutex before giving up: the
  // io_thread can be stuck in a proc.Write to a wedged-but-alive adapter while
  // holding it, so a shutdown-path send must not block teardown forever. On
  // timeout it returns false and the caller falls through to the force-kill (which
  // unblocks that write so the io_thread can be joined).
  bool SendMessageImmediate(const util::JsonValue& msg, bool allow_during_shutdown = false,
                            std::chrono::milliseconds lock_timeout = std::chrono::milliseconds::zero()) {
    if (!allow_during_shutdown && shutting_down.load(std::memory_order_acquire)) {
      return false;
    }
    const std::string serialized = SerializeMessage(msg);
    if (lock_timeout > std::chrono::milliseconds::zero()) {
      // Bounded acquisition via try_lock polling (not timed_mutex::try_lock_for,
      // which ThreadSanitizer mis-models). If the io_thread is stuck writing to a
      // wedged adapter it holds write_mutex; we give up after lock_timeout and the
      // caller force-kills, which unblocks that write so the io_thread can be joined.
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

  bool SendMessageAfterInitialize(util::JsonValue msg) {
    // Defer serialization into the builder (evaluated later, off the caller thread, and
    // only after the queue-count cap passes) — mirroring the LSP client. Previously the
    // caller eagerly serialized the whole framed string even when the message would be
    // refused (wedged adapter / queue full). (TD-2026-07-16-25.)
    return SendMessageBuilderAfterInitialize(
        [this, m = std::move(msg)]() mutable { return SerializeMessage(m); });
  }

  bool SendMessageBuilderAfterInitialize(std::function<std::string()> build_serialized) {
    if (shutting_down.load(std::memory_order_acquire)) {
      return false;
    }
    std::lock_guard lock(send_mutex);
    if (shutting_down.load(std::memory_order_acquire)) {
      return false;
    }
    // OOM backstop: a wedged adapter that has stopped reading stdin. Refuse rather
    // than grow without bound or corrupt the protocol by dropping mid-stream messages.
    if (deferred_messages.size() + outbound_messages.size() >= kMaxQueuedMessages) {
      if (!outbound_overflow_logged_) {
        outbound_overflow_logged_ = true;
        std::fprintf(stderr,
                     "[dap] outbound queue exceeded %zu messages; adapter appears wedged, "
                     "refusing further messages\n",
                     kMaxQueuedMessages);
      }
      return false;
    }
    if (!initialized.load(std::memory_order_acquire)) {
      deferred_messages.push_back(QueuedMessage{.serialized = {},
                                                .build_serialized = std::move(build_serialized)});
      return true;
    }
    if (test_stub_mode.load(std::memory_order_acquire)) {
      return true;
    }
    // Refuse once the io_thread has stopped draining. An adapter that closes its
    // stdout but stays alive makes IoMain exit on EOF while proc.IsRunning() stays
    // true; without the stop_io check we would keep queuing requests no thread will
    // ever send or time out, so their callbacks (and the UI loading state) hang.
    if (stop_io.load(std::memory_order_acquire) || !proc.IsRunning()) {
      return false;
    }
    outbound_messages.push_back(QueuedMessage{.serialized = {},
                                              .build_serialized = std::move(build_serialized)});
    Wake();
    return true;
  }

  void ClearDeferredMessages() {
    std::lock_guard lock(send_mutex);
    deferred_messages.clear();
    outbound_messages.clear();
  }

  void ResetProtocolState() {
    std::lock_guard lock(mutex);
    pending_requests.clear();
    main_mailbox.Clear();
    shutdown_response_received = false;
    shutdown_request_seq = 0;
  }

  void ShutdownProcessOnce(int timeout_ms = 3000) {
    bool expected = false;
    if (!process_shutdown_started.compare_exchange_strong(expected, true,
                                                          std::memory_order_acq_rel)) {
      return;
    }
    proc.Shutdown(timeout_ms);
  }

  // --- framing --------------------------------------------------------------
  std::optional<util::JsonValue> TryParseOneMessage(DapReadBuf& buf) {
    static constexpr std::string_view kPrefix = "Content-Length: ";

    // Draining the body of an oversized frame we chose to skip: consume what is
    // buffered and stop until the rest arrives, so the parser resyncs to the next
    // frame instead of desyncing and tearing the session down.
    if (skip_body_bytes_ > 0) {
      const std::size_t drop = std::min<std::size_t>(skip_body_bytes_, buf.view().size());
      buf.consume(drop);
      skip_body_bytes_ -= drop;
      return std::nullopt;
    }

    const std::string_view v = buf.view();
    const auto nl = v.find('\n');
    if (nl == std::string_view::npos) {
      return std::nullopt;
    }
    std::string_view line = v.substr(0, nl);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    if (line.substr(0, kPrefix.size()) != kPrefix) {
      buf.consume(nl + 1);
      return std::nullopt;
    }
    const std::string_view len_sv = line.substr(kPrefix.size());
    int content_len = 0;
    const auto [ptr, ec] =
        std::from_chars(len_sv.data(), len_sv.data() + len_sv.size(), content_len);
    if (ec != std::errc{} || content_len <= 0) {
      // Malformed/absurd length: drop the header line and try to resync; the
      // read-buffer cap remains the backstop if the stream never recovers.
      buf.consume(nl + 1);
      return std::nullopt;
    }
    std::size_t body_start = nl + 1;
    bool header_terminated = false;
    while (body_start < v.size()) {
      const auto nl2 = v.find('\n', body_start);
      if (nl2 == std::string_view::npos) {
        return std::nullopt;
      }
      std::string_view hdr = v.substr(body_start, nl2 - body_start);
      if (!hdr.empty() && hdr.back() == '\r') {
        hdr.remove_suffix(1);
      }
      body_start = nl2 + 1;
      if (hdr.empty()) {
        header_terminated = true;
        break;
      }
    }
    // The loop can also exit because the buffer ran out mid-header-block (a recv
    // split right on a header newline). `body_start` then points at the buffer
    // end, not past the real blank-line terminator; committing the oversized skip
    // below with that body_start would count the unseen terminator bytes as body
    // and desync the stream. Wait for the rest of the header block instead.
    if (!header_terminated) {
      return std::nullopt;
    }
    if (static_cast<std::size_t>(content_len) > kMaxDapMessageBytes) {
      // Too large to buffer: skip the entire frame (headers + body) so the parser
      // resyncs to the next frame instead of reading body bytes as headers.
      buf.consume(body_start);
      skip_body_bytes_ = static_cast<std::size_t>(content_len);
      return std::nullopt;
    }
    if (v.size() - body_start < static_cast<std::size_t>(content_len)) {
      return std::nullopt;
    }
    const std::string_view body = v.substr(body_start, content_len);
    auto parsed = util::ParseJson(body);
    buf.consume(body_start + content_len);
    return parsed;
  }

  bool WaitStdoutReadable(int timeout_ms) {
#if defined(__unix__) || defined(__APPLE__)
    // Re-fetch the stdout fd each poll rather than trusting the once-captured
    // cached_stdout_fd_: a main-thread liveness probe (proc.IsRunning) or the
    // shutdown thread can reap the child and close this fd, whose number another
    // thread may then reuse — polling the cached number would watch an unrelated
    // descriptor. stdout_fd() returns -1 (under lock) once closed, so we fall
    // through to the read path and let proc.Read() observe EOF and tear down.
    const int stdout_fd = proc.stdout_fd();
    if (stdout_fd >= 0) {
      pollfd fds[2] = {};
      int nfds = 0;
      const int out_index = nfds;
      fds[nfds].fd = stdout_fd;
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
        return false;
      }
      if (wake_index >= 0 && (fds[wake_index].revents & POLLIN) != 0) {
        DrainWakePipe();
      }
      const short out_revents = fds[out_index].revents;
      return (out_revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0;
    }
#endif
    return true;
  }

  void ParseBufferedMessages() {
    while (true) {
      const std::size_t buffered_before = io_buf.view().size();
      auto msg_opt = TryParseOneMessage(io_buf);
      if (msg_opt) {
        DispatchMessage(std::move(*msg_opt));
        continue;
      }
      // nullopt is overloaded: either TryParseOneMessage consumed a frame it could
      // not surface (malformed header / oversized / invalid JSON) — in which case
      // well-formed frames already buffered behind it must still be drained now, not
      // ~1s later on the next poll — or no complete frame is available yet, in which
      // case the buffer is unchanged and we wait for more bytes.
      if (io_buf.view().size() == buffered_before) {
        break;
      }
    }
  }

  void IoMain() {
    cached_stdout_fd_ = proc.stdout_fd();
    const int read_timeout = cached_stdout_fd_ >= 0 ? 0 : 50;
    while (!stop_io.load(std::memory_order_acquire)) {
      ParseBufferedMessages();
      DrainOutbound();
      // WaitStdoutReadable wakes at least once per second, so this sweeps timed-out
      // requests within ~1s even when the adapter sends nothing.
      if (!shutting_down.load(std::memory_order_acquire)) {
        FailPendingRequests(/*only_expired=*/true);
      }
      if (stop_io.load(std::memory_order_acquire)) {
        break;
      }
      if (!WaitStdoutReadable(1000)) {
        continue;
      }
      auto chunk = proc.Read(4096, read_timeout);
      if (!chunk) {
        break;
      }
      if (!chunk->empty()) {
        io_buf.append(*chunk);
      }
      if (io_buf.view().size() > kMaxDapReadBufferBytes) {
        break;  // runaway adapter (no valid frame): tear the session down
      }
    }
    ParseBufferedMessages();
    DrainOutbound();
    // The adapter is gone (EOF / exited / killed by the memory cap). When this is an
    // unexpected death rather than a shutdown we requested, fail any still-pending
    // requests so the UI clears instead of waiting forever, and — crucially — wake
    // the main thread even if there were none pending: an idle *stopped* session has
    // no in-flight request, so without this nudge ConsumeDapCallbacks would never run
    // ReapExitedSessions and the dead session would linger (no `terminated` event,
    // stale "paused" UI) until some unrelated event happened to drive a frame.
    if (!shutting_down.load(std::memory_order_acquire)) {
      FailPendingRequests(/*only_expired=*/false);
      main_mailbox.PushWake();
    }
    // The io_thread is exiting (EOF / fatal read / runaway buffer). Signal that no
    // thread is draining outbound anymore so the send path refuses further requests
    // instead of stranding them — covers an adapter that closed stdout but is still
    // alive, where proc.IsRunning() alone would keep accepting sends. DoInitialize
    // resets this to false before spawning a fresh io_thread on restart.
    stop_io.store(true, std::memory_order_release);
  }

  // --- dispatch -------------------------------------------------------------
  void DispatchMessage(util::JsonValue msg) {
    // Single funnel for every inbound message (responses, events, reverse requests).
    util::DebugTrace::Message("recv", msg);
    const std::string& type = msg["type"].AsString();
    if (type == "response") {
      HandleResponse(std::move(msg));
    } else if (type == "event") {
      HandleEvent(std::move(msg));
    } else if (type == "request") {
      HandleReverseRequest(std::move(msg));
    }
  }

  void HandleResponse(util::JsonValue msg) {
    // Correlate only on a numeric request_seq within int range (mirrors the LSP
    // dispatch's ResponseIdInRange). A string-encoded or out-of-int-range seq from a
    // non-conformant/hostile adapter cannot match one of our small monotonic seqs, so
    // skip it rather than narrow-and-collide with a live pending request.
    int request_seq = 0;
    if (!DapResponseSeqInRange(msg["request_seq"], &request_seq)) {
      return;
    }
    if (shutting_down.load(std::memory_order_acquire)) {
      std::lock_guard lock(mutex);
      if (request_seq == shutdown_request_seq) {
        shutdown_response_received = true;
        shutdown_cv.notify_all();
      }
      return;
    }
    std::function<void(util::JsonValue)> cb;
    {
      std::lock_guard lock(mutex);
      auto it = pending_requests.find(request_seq);
      if (it != pending_requests.end()) {
        cb = std::move(it->second.callback);
        pending_requests.erase(it);
      }
    }
    if (!cb) {
      return;
    }
    main_mailbox.Post([cb = std::move(cb), m = std::move(msg)]() mutable { cb(std::move(m)); });
  }

  void HandleEvent(util::JsonValue msg) {
    std::string event = msg["event"].AsString();
    util::JsonValue body = msg["body"];
    EventCallback cb;
    {
      std::lock_guard lock(mutex);
      cb = event_callback;
    }
    if (cb) {
      main_mailbox.Post(
          [cb = std::move(cb), e = std::move(event), b = std::move(body)]() mutable { cb(e, b); });
    }
  }

  // Adapter -> client requests must always get a reply or the adapter may stall.
  // We advertise no client-side capabilities (runInTerminal etc.), so reject any
  // reverse request that does arrive.
  void HandleReverseRequest(util::JsonValue msg) {
    // The adapter's seq is echoed back verbatim as our response's request_seq. If it
    // is out of int range we cannot represent it faithfully, and narrowing it would
    // reply against a different request id — a non-conformant adapter, so drop the
    // reverse request without replying rather than send a mis-correlated response.
    int request_seq = 0;
    if (!DapResponseSeqInRange(msg["seq"], &request_seq)) {
      return;
    }
    const std::string& command = msg["command"].AsString();
    const int seq = GetNextSeq();
    SendMessageAfterInitialize(dap_protocol::MakeResponse(
        seq, request_seq, command, false, "unsupported reverse request: " + command,
        util::JsonValue(nullptr)));
  }

  int RegisterPendingRequest(std::function<void(util::JsonValue)> cb,
                             std::chrono::milliseconds timeout) {
    std::lock_guard lock(mutex);
    const int seq = next_seq++;
    pending_requests[seq] = PendingRequest{
        .callback = std::move(cb),
        .deadline = std::chrono::steady_clock::now() + timeout,
    };
    return seq;
  }

  // Returns true iff this call actually removed a still-pending entry. The
  // send-failure path uses that to avoid double-delivering a callback the I/O
  // thread's EOF sweep (FailPendingRequests) may have already failed and erased in
  // the window between RegisterPendingRequest and a failed SendMessageAfterInitialize.
  bool RemovePendingRequest(int seq) {
    std::lock_guard lock(mutex);
    return pending_requests.erase(seq) != 0;
  }

  // Synthesize failure responses for pending requests so a non-responding adapter
  // never strands a request (and the UI's loading state). `only_expired` fails
  // just those past their deadline (the periodic timeout sweep); otherwise fails
  // all (the adapter has exited and no response can ever arrive). Runs on the I/O
  // thread; pushes the handlers onto the main-thread ready queue and wakes it.
  void FailPendingRequests(bool only_expired) {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(mutex);
    bool any = false;
    for (auto it = pending_requests.begin(); it != pending_requests.end();) {
      if (only_expired && now < it->second.deadline) {
        ++it;
        continue;
      }
      // The runaway/<unavailable> smoking gun: correlate this seq with the
      // matching outbound "send" line to see which request the adapter dropped.
      util::DebugTrace::Note("dap", only_expired ? "request-timeout seq" : "adapter-gone seq",
                             static_cast<long long>(it->first));
      auto cb = std::move(it->second.callback);
      it = pending_requests.erase(it);
      util::JsonValue envelope = dap_protocol::MakeResponse(
          0, 0, std::string{}, false, "debug adapter did not respond", util::JsonValue(nullptr));
      main_mailbox.PostWithoutWake(
          [cb = std::move(cb), envelope = std::move(envelope)]() mutable { cb(std::move(envelope)); });
      any = true;
    }
    if (any) {
      main_mailbox.PushWake();
    }
  }

  // Register the response handler, send the request, and on send failure clean up
  // and report the failure. Returns false when the request cannot be sent.
  bool DispatchRequest(const std::string& command, util::JsonValue arguments,
                       std::function<void(util::JsonValue)> response_handler,
                       std::function<void()> on_send_failure) {
    if (test_stub_mode.load(std::memory_order_acquire)) {
      return DispatchRequestStub(command, std::move(arguments), std::move(response_handler),
                                 std::move(on_send_failure));
    }
    const int seq = RegisterPendingRequest(std::move(response_handler), RequestTimeoutFor(command));
    if (!SendMessageAfterInitialize(dap_protocol::MakeRequest(seq, command, arguments))) {
      // Only report the failure if the entry was still ours to fail. If the EOF
      // sweep already claimed it, it also already invoked the handler — running
      // on_send_failure() again would double-fire the callback.
      if (RemovePendingRequest(seq)) {
        on_send_failure();
      }
      return false;
    }
    return true;
  }

  bool DispatchRequestStub(const std::string& command, util::JsonValue arguments,
                           std::function<void(util::JsonValue)> response_handler,
                           std::function<void()> on_send_failure) {
    std::function<void(const std::string&, const util::JsonValue&, ResponseCallback)> handler;
    {
      std::lock_guard lock(mutex);
      handler = test_request_handler;
    }
    if (!handler) {
      on_send_failure();
      return false;
    }
    // Bridge the user-facing ResponseCallback (DapResponse) back into the raw
    // pending-handler shape by re-encoding into a response envelope.
    ResponseCallback bridge = [this, command,
                               response_handler = std::move(response_handler)](
                                  const dap_protocol::DapResponse& response) {
      util::JsonValue envelope = dap_protocol::MakeResponse(0, 0, command, response.success,
                                                            response.message, response.body);
      auto handler_copy = response_handler;
      main_mailbox.Post(
          [handler_copy = std::move(handler_copy), envelope = std::move(envelope)]() mutable {
            handler_copy(std::move(envelope));
          });
    };
    handler(command, arguments, std::move(bridge));
    return true;
  }

  void DoInitializeBlocking() {
    initializing.store(true, std::memory_order_release);

    using namespace util;
    JsonObject args;
    args["clientID"] = JsonValue("microide");
    args["clientName"] = JsonValue("microide");
    args["adapterID"] = JsonValue(adapter_id);
    args["locale"] = JsonValue("en-US");
    args["linesStartAt1"] = JsonValue(true);
    args["columnsStartAt1"] = JsonValue(true);
    args["pathFormat"] = JsonValue("path");
    args["supportsVariableType"] = JsonValue(true);
    args["supportsVariablePaging"] = JsonValue(true);
    args["supportsRunInTerminalRequest"] = JsonValue(false);
    args["supportsMemoryReferences"] = JsonValue(false);
    args["supportsProgressReporting"] = JsonValue(false);
    args["supportsInvalidatedEvent"] = JsonValue(false);

    const int init_seq = GetNextSeq();
    const auto req = dap_protocol::MakeRequest(init_seq, "initialize", JsonValue(std::move(args)));
    if (!SendMessageImmediate(req)) {
      SetLastError("failed to send initialize request to debug adapter");
      ShutdownProcessOnce();
      // Fail (don't silently drop) any request registered during init so its UI
      // loading state resolves instead of stranding — symmetric with the LSP client.
      FailPendingRequests(false);
      ClearDeferredMessages();
      initializing.store(false, std::memory_order_release);
      return;
    }

    DapReadBuf& buf = io_buf;
    bool got_init = false;
    // Messages that arrive BEFORE the initialize response are buffered here and
    // dispatched only after `capabilities` is stored and `initialized` is set. A
    // non-conformant adapter that emits the `initialized` event before its
    // initialize response would otherwise have that event dispatched to the main
    // thread while capabilities were still default-constructed — so
    // SendConfigurationDone would read supports_configuration_done_request==false
    // and skip configurationDone, hanging a debuggee that needs it. For a
    // spec-compliant adapter (response first) this stays empty and is zero-cost.
    std::vector<util::JsonValue> early_messages;
    // Bound the initialize wait by WALL-CLOCK, not an attempt count. proc.Read returns
    // as soon as any data is available and dispatching a buffered message does not read
    // at all, so an adapter that emits a burst of events (e.g. `output`) before its
    // initialize response would burn an attempt-count budget in milliseconds and get
    // force-killed while healthy. A steady_clock deadline spends the budget only as real
    // time elapses; message supply is bounded per read, so this never spins hot.
    // Symmetric with the LSP client's initialize wait.
    const auto init_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (std::chrono::steady_clock::now() < init_deadline) {
      if (stop_init.load(std::memory_order_acquire)) {
        ShutdownProcessOnce();
        FailPendingRequests(false);
        ClearDeferredMessages();
        initializing.store(false, std::memory_order_release);
        return;
      }
      auto msg_opt = TryParseOneMessage(buf);
      if (!msg_opt) {
        auto chunk = proc.Read(4096, 500);
        if (!chunk) {
          break;
        }
        if (!chunk->empty()) {
          buf.append(*chunk);
        }
        if (buf.view().size() > kMaxDapReadBufferBytes) {
          break;  // runaway adapter
        }
        continue;
      }
      const util::JsonValue& msg = *msg_opt;
      const std::string& type = msg["type"].AsString();
      // Range-check the seq before comparing to init_seq: a wrapped out-of-range
      // request_seq must not be able to forge an initialize match and cause us to
      // trust capabilities from a response that was never the initialize reply.
      int init_response_seq = 0;
      const bool is_init_response =
          type == "response" && DapResponseSeqInRange(msg["request_seq"], &init_response_seq) &&
          init_response_seq == init_seq;
      if (!is_init_response) {
        // Defer adapter-initiated messages that arrive before the initialize
        // response (e.g. an early `output`, or a non-conformant `initialized`)
        // until capabilities are stored, preserving arrival order.
        early_messages.push_back(std::move(*msg_opt));
        // TD-2026-07-17A-098: bound the pre-initialize replay buffer. A conforming
        // adapter emits only a handful of events before its initialize response; an
        // adapter flooding thousands is runaway — fail initialization cleanly instead
        // of retaining an unbounded event backlog for the full timeout window.
        if (early_messages.size() > kMaxDapEarlyMessages) {
          SetLastError("debug adapter emitted too many events before initialize response");
          ShutdownProcessOnce();
          FailPendingRequests(false);
          ClearDeferredMessages();
          initializing.store(false, std::memory_order_release);
          return;
        }
        continue;
      }
      if (!msg["success"].AsBool(false)) {
        const util::JsonValue& message_val = msg["message"];
        const std::string message =
            message_val.IsString() ? message_val.AsString() : "initialize failed";
        SetLastError("debug adapter rejected initialize: " + message);
        ShutdownProcessOnce();
        FailPendingRequests(false);
        ClearDeferredMessages();
        initializing.store(false, std::memory_order_release);
        return;
      }
      {
        std::lock_guard lock(mutex);
        capabilities = dap_protocol::ParseCapabilities(msg["body"]);
      }
      got_init = true;
      break;
    }

    if (!got_init) {
      const std::optional<int> exit_code = proc.exit_code();
      if (exit_code.has_value()) {
        SetLastError("debug adapter exited before initialize response (exit code " +
                     std::to_string(*exit_code) + ")");
      } else {
        SetLastError("timed out waiting for initialize response from debug adapter");
      }
      ShutdownProcessOnce();
      FailPendingRequests(false);
      ClearDeferredMessages();
      initializing.store(false, std::memory_order_release);
      return;
    }

    {
      std::lock_guard lock(send_mutex);
      initialized.store(true, std::memory_order_release);
      for (QueuedMessage& message : deferred_messages) {
        outbound_messages.push_back(std::move(message));
      }
      deferred_messages.clear();
    }

    // Now that capabilities are parsed and `initialized` is set, replay any
    // pre-response messages in arrival order. A buffered `initialized` event
    // therefore reaches the main-thread handshake with correct capabilities.
    for (util::JsonValue& message : early_messages) {
      DispatchMessage(std::move(message));
    }
    early_messages.clear();

    OpenWakePipe();
    stop_io.store(false, std::memory_order_release);
    io_thread = std::thread([this]() { IoMain(); });
    Wake();
    initializing.store(false, std::memory_order_release);
  }

  void DoShutdown() {
    TraceDapLifecycle(adapter_id, proc.pid(), "shutdown-begin");
    shutting_down.store(true, std::memory_order_release);
    stop_init.store(true);

    if (test_stub_mode.load(std::memory_order_acquire)) {
      ClearDeferredMessages();
      ResetProtocolState();
      {
        std::lock_guard lock(mutex);
        test_request_handler = nullptr;
      }
      initialized.store(false, std::memory_order_release);
      initializing.store(false, std::memory_order_release);
      test_stub_mode.store(false, std::memory_order_release);
      shutting_down.store(false, std::memory_order_release);
      shutdown_complete.store(true, std::memory_order_release);
      main_mailbox.PushWake();
      return;
    }

    if (!initialized.load(std::memory_order_acquire)) {
      TraceDapLifecycle(adapter_id, proc.pid(), "preinit-cancel");
      stop_io.store(true, std::memory_order_release);
      Wake();
      ShutdownProcessOnce(0);
      if (init_thread.joinable()) {
        init_thread.join();
      }
      // The init thread's success path may have observed our shutdown request too
      // late: it resets stop_io to false and spawns io_thread after we set stop_io
      // above (a lost-signal window). Re-assert the stop and wake io_thread so it
      // terminates promptly instead of relying on the force-kill making the adapter
      // hit EOF. io_thread is only joinable when init opened the wake pipe.
      if (io_thread.joinable()) {
        stop_io.store(true, std::memory_order_release);
        Wake();
        io_thread.join();
      }
      ClearDeferredMessages();
      CloseWakePipe();
      ResetProtocolState();
      initialized.store(false, std::memory_order_release);
      initializing.store(false, std::memory_order_release);
      shutting_down.store(false, std::memory_order_release);
      shutdown_complete.store(true, std::memory_order_release);
      main_mailbox.PushWake();
      return;
    }

    if (init_thread.joinable()) {
      init_thread.join();
    }
    ClearDeferredMessages();

    using namespace util;
    int disconnect_seq = 0;
    {
      std::lock_guard lock(mutex);
      shutdown_response_received = false;
      shutdown_request_seq = next_seq++;
      disconnect_seq = shutdown_request_seq;
    }
    JsonObject disconnect_args;
    disconnect_args["restart"] = JsonValue(false);
    // Bounded lock acquisition: if the io_thread is stuck in a proc.Write to a
    // wedged-but-alive adapter (holding write_mutex), this returns false quickly
    // rather than blocking teardown forever, and we fall through to the force-kill
    // below — which unblocks that write so io_thread can be joined.
    const bool sent_disconnect = SendMessageImmediate(
        dap_protocol::MakeRequest(disconnect_seq, "disconnect", JsonValue(std::move(disconnect_args))),
        true, std::chrono::milliseconds(1000));
    TraceDapLifecycle(adapter_id, proc.pid(), "disconnect-request",
                      sent_disconnect ? "sent" : "send-failed");
    if (sent_disconnect) {
      std::unique_lock lock(mutex);
      const bool got_response = shutdown_cv.wait_for(lock, std::chrono::milliseconds(750),
                                                     [this]() { return shutdown_response_received; });
      TraceDapLifecycle(adapter_id, proc.pid(), "disconnect-response",
                        got_response ? "received" : "timeout");
    }
    if (proc.IsRunning()) {
      proc.CloseStdin();
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
    while (proc.IsRunning() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    stop_io.store(true, std::memory_order_release);
    Wake();
    if (proc.IsRunning()) {
      TraceDapLifecycle(adapter_id, proc.pid(), "forced-shutdown");
      ShutdownProcessOnce(1000);
    }
    if (io_thread.joinable()) {
      io_thread.join();
    }
    CloseWakePipe();
    ResetProtocolState();
    initialized.store(false, std::memory_order_release);
    initializing.store(false, std::memory_order_release);
    shutting_down.store(false, std::memory_order_release);
    shutdown_complete.store(true, std::memory_order_release);
    TraceDapLifecycle(adapter_id, -1, "shutdown-complete");
    main_mailbox.PushWake();
  }

  void BeginShutdown() {
    bool expected = false;
    if (!shutdown_started.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
      return;
    }
    shutdown_complete.store(false, std::memory_order_release);
    shutdown_thread = std::thread([this]() { DoShutdown(); });
  }

  void WaitForShutdown() {
    BeginShutdown();
    if (shutdown_thread.joinable()) {
      shutdown_thread.join();
    }
  }
};

}  // namespace microide::workspace
