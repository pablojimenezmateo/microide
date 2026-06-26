#pragma once

#include "workspace/DapProtocol.h"
#include "workspace/WorkspaceDapClient.h"
#include "util/DebugTrace.h"
#include "util/MainThreadMailbox.h"

#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
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

// OOM backstop for the outbound/deferred queues. The I/O thread drains outbound
// continuously, so this is never approached in normal operation; the queues only grow
// without bound if the adapter stops reading its stdin while staying alive. At that
// point the session is wedged, so we refuse further messages rather than silently DROP
// queued protocol messages or grow until the IDE OOMs. Refused requests fail cleanly
// via the send-failure path; the timeout sweep clears anything already pending.
constexpr std::size_t kMaxQueuedMessages = 50000;

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
  struct QueuedMessage {
    std::string serialized;
    std::function<std::string()> build_serialized;

    std::string TakeSerialized() && {
      if (!serialized.empty()) {
        return std::move(serialized);
      }
      return build_serialized ? build_serialized() : std::string{};
    }
  };

  platform::AsyncSubprocess proc;
  std::mutex mutex;
  std::mutex send_mutex;
  std::mutex write_mutex;

  // Single I/O thread: reads stdout / writes stdin, blocking in poll() over
  // stdout + a self-pipe wake, so an idle adapter makes no fixed-cadence wakeups.
  // io_buf is filled by the initialize handshake and handed to the I/O thread,
  // preserving any events the adapter pushed right after the initialize response.
  DapReadBuf io_buf;
  std::thread io_thread;
  std::atomic<bool> stop_io{false};
  std::mutex wake_mutex;
  int wake_pipe_[2] = {-1, -1};
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
  void OpenWakePipe() {
#if defined(__unix__) || defined(__APPLE__)
    std::lock_guard lock(wake_mutex);
    if (wake_pipe_[0] >= 0) {
      return;
    }
    int fds[2] = {-1, -1};
    if (::pipe(fds) != 0) {
      return;
    }
    ::fcntl(fds[0], F_SETFL, O_NONBLOCK);
    ::fcntl(fds[1], F_SETFL, O_NONBLOCK);
    wake_pipe_[0] = fds[0];
    wake_pipe_[1] = fds[1];
#endif
  }

  void CloseWakePipe() {
#if defined(__unix__) || defined(__APPLE__)
    std::lock_guard lock(wake_mutex);
    if (wake_pipe_[1] >= 0) {
      ::close(wake_pipe_[1]);
      wake_pipe_[1] = -1;
    }
    if (wake_pipe_[0] >= 0) {
      ::close(wake_pipe_[0]);
      wake_pipe_[0] = -1;
    }
#endif
  }

  void Wake() {
#if defined(__unix__) || defined(__APPLE__)
    std::lock_guard lock(wake_mutex);
    if (wake_pipe_[1] < 0) {
      return;
    }
    const char byte = 1;
    ssize_t written = ::write(wake_pipe_[1], &byte, 1);
    (void)written;
#endif
  }

  void DrainWakePipe() {
#if defined(__unix__) || defined(__APPLE__)
    if (wake_pipe_[0] < 0) {
      return;
    }
    char scratch[64];
    while (::read(wake_pipe_[0], scratch, sizeof(scratch)) > 0) {
    }
#endif
  }

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

  bool SendMessageImmediate(const util::JsonValue& msg, bool allow_during_shutdown = false) {
    if (!allow_during_shutdown && shutting_down.load(std::memory_order_acquire)) {
      return false;
    }
    const std::string serialized = SerializeMessage(msg);
    std::lock_guard wlock(write_mutex);
    return proc.Write(serialized);
  }

  bool SendMessageAfterInitialize(const util::JsonValue& msg) {
    return SendMessageBuilderAfterInitialize(
        [serialized = SerializeMessage(msg)]() mutable { return std::move(serialized); });
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
    if (!proc.IsRunning()) {
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
      buf.consume(nl + 1);
      return std::nullopt;
    }
    std::size_t body_start = nl + 1;
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
        break;
      }
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
    if (cached_stdout_fd_ >= 0) {
      pollfd fds[2] = {};
      int nfds = 0;
      const int out_index = nfds;
      fds[nfds].fd = cached_stdout_fd_;
      fds[nfds].events = POLLIN | POLLHUP;
      ++nfds;
      int wake_index = -1;
      if (wake_pipe_[0] >= 0) {
        wake_index = nfds;
        fds[nfds].fd = wake_pipe_[0];
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
      auto msg_opt = TryParseOneMessage(io_buf);
      if (!msg_opt) {
        break;
      }
      DispatchMessage(std::move(*msg_opt));
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
    const int request_seq = static_cast<int>(msg["request_seq"].AsInt());
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
    const int request_seq = static_cast<int>(msg["seq"].AsInt());
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

  void RemovePendingRequest(int seq) {
    std::lock_guard lock(mutex);
    pending_requests.erase(seq);
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
      RemovePendingRequest(seq);
      on_send_failure();
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
      ClearDeferredMessages();
      initializing.store(false, std::memory_order_release);
      return;
    }

    DapReadBuf& buf = io_buf;
    bool got_init = false;
    for (int attempts = 0; attempts < 120; ++attempts) {
      if (stop_init.load(std::memory_order_acquire)) {
        ShutdownProcessOnce();
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
        continue;
      }
      const util::JsonValue& msg = *msg_opt;
      const std::string& type = msg["type"].AsString();
      const bool is_init_response =
          type == "response" && static_cast<int>(msg["request_seq"].AsInt()) == init_seq;
      if (!is_init_response) {
        // Preserve adapter-initiated events that arrive before/around the
        // initialize response (e.g. an early `output`) by dispatching them.
        DispatchMessage(util::JsonValue(msg));
        continue;
      }
      if (!msg["success"].AsBool(false)) {
        const util::JsonValue& message_val = msg["message"];
        const std::string message =
            message_val.IsString() ? message_val.AsString() : "initialize failed";
        SetLastError("debug adapter rejected initialize: " + message);
        ShutdownProcessOnce();
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
      if (io_thread.joinable()) {
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
    const bool sent_disconnect = SendMessageImmediate(
        dap_protocol::MakeRequest(disconnect_seq, "disconnect", JsonValue(std::move(disconnect_args))),
        true);
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
