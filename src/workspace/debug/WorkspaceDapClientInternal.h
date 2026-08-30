#pragma once

#include "workspace/debug/DapProtocol.h"
#include "workspace/JsonRpcMessageFraming.h"
#include "workspace/debug/WorkspaceDapClient.h"
#include "workspace/ProtocolNumeric.h"
#include "workspace/StdioClientQueue.h"
#include "workspace/StdioJsonRpcClientTransport.h"
#include "util/DebugTrace.h"
#include "util/MainThreadMailbox.h"
#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"
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
// Aggregate approximate byte budget for queued outbound payloads (the count cap
// above does not bound retained BYTES — large evaluate/setExpression/source-content
// bodies capture their text by value until sent). TD-2026-07-17A-071.
constexpr std::size_t kMaxQueuedBytes = 512u * 1024 * 1024;  // 512 MiB

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

// The transport half (I/O thread, outbound queues, framing, caps) lives in the
// shared StdioJsonRpcClientTransport mixin — one implementation for the LSP and
// DAP clients, which used to carry drifting copies of it. Impl supplies the
// protocol half the mixin calls back into: DispatchMessage, FailPendingRequests,
// SetLastError, TraceSend, WakeMainThread.
struct DapClient::Impl : StdioJsonRpcClientTransport<DapClient::Impl> {
  Impl()
      : StdioJsonRpcClientTransport(StdioTransportConfig{
            .messages_sent = util::PerfCounterId::DapMessagesSent,
            .bytes_sent = util::PerfCounterId::DapBytesSent,
            .bytes_received = util::PerfCounterId::DapBytesReceived,
            .max_message_bytes = kMaxDapMessageBytes,
            .max_read_buffer_bytes = kMaxDapReadBufferBytes,
            .max_queued_messages = kMaxQueuedMessages,
            .max_queued_bytes = kMaxQueuedBytes,
            .peer_label = "dap",
            .send_error = "failed to send message to debug adapter",
        }) {}

  void TraceSend(const util::JsonValue& msg) const { util::DebugTrace::Message("send", msg); }
  void WakeMainThread() { main_mailbox.PushWake(); }

  std::mutex mutex;

  std::thread init_thread;
  std::atomic<bool> initializing{false};
  std::atomic<bool> stop_init{false};
  std::thread shutdown_thread;
  std::atomic<bool> shutdown_started{false};
  std::atomic<bool> shutdown_complete{false};
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

  std::function<void(const std::string&, const util::JsonValue&, ResponseCallback)>
      test_request_handler;

  EventCallback event_callback;

  int next_seq = 1;
  std::string adapter_id;
  std::string last_error;
  std::string last_error_snapshot;
  dap_protocol::DapCapabilities capabilities;

  void SetLastError(std::string message) {
    std::lock_guard lock(mutex);
    last_error = std::move(message);
  }

  int GetNextSeq() {
    std::lock_guard lock(mutex);
    return next_seq++;
  }

  void ResetProtocolState() {
    std::lock_guard lock(mutex);
    pending_requests.clear();
    main_mailbox.Clear();
    shutdown_response_received = false;
    shutdown_request_seq = 0;
  }

  // --- dispatch -------------------------------------------------------------
  void DispatchMessage(util::JsonValue msg) {
    // Single funnel for every inbound message (responses, events, reverse requests).
    // Mirrors lsp::DispatchMessage -- these two transports duplicate each other,
    // so instrumenting one and not the other leaves half the picture.
    util::PerformanceTrace::Scope perf_scope("dap::DispatchMessage");
    util::AddPerformanceCounter(util::PerfCounterId::DapMessagesReceived);
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
    if (!SendMessageAfterInitialize(
            dap_protocol::MakeRequest(seq, command, std::move(arguments)))) {
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
      auto msg_opt = framer_.Next();
      if (!msg_opt) {
        auto chunk = proc.Read(4096, 500);
        if (!chunk) {
          break;
        }
        if (!chunk->empty()) {
          framer_.Append(*chunk);
        }
        if (framer_.BufferedBytes() > kMaxDapReadBufferBytes) {
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
