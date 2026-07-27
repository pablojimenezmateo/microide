#pragma once

#include "workspace/WorkspaceLspClient.h"
#include "workspace/LspProtocol.h"
#include "workspace/StdioClientQueue.h"
#include "util/MainThreadMailbox.h"
#include "util/WakePipe.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

#include "util/StartupTrace.h"
#include "workspace/LspClientTrace.h"
#include "workspace/JsonRpcMessageFraming.h"
#include "util/StringUtil.h"

namespace microide::workspace {

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------
struct LspClient::Impl {
  using QueuedMessage = StdioQueuedMessage;

  platform::AsyncSubprocess proc;
  std::mutex mutex;
  std::mutex send_mutex;   // guards the outbound/deferred queues
  // Serializes every proc.Write so stdin is never written by two threads. The
  // shutdown path bounds its acquisition (see SendMessageImmediate): the io_thread
  // can hold this across a proc.Write that blocks indefinitely on a wedged-but-alive
  // server, and the shutdown's force-kill (which unblocks that write) must not be
  // gated behind acquiring this lock. A plain std::mutex with a try_lock poll rather
  // than std::timed_mutex::try_lock_for, which ThreadSanitizer mis-models (spurious
  // "unlock of an unlocked mutex"). Mirrors the DAP client.
  std::mutex write_mutex;

  // Single I/O thread state. One thread per server reads stdout and writes stdin;
  // it blocks in poll() over stdout + a self-pipe wakeup, so it makes no
  // fixed-cadence idle wakeups and reacts immediately to data or new outbound.
  // framer_ is filled first by the initialize handshake and then handed to the I/O
  // thread; keeping it a member preserves any bytes the server pushed right after
  // the initialize response (e.g. clangd's early registerCapability / progress /
  // configuration requests) across that handoff, along with any partial frame.
  JsonRpcMessageFramer framer_{.max_message_bytes = kMaxLspMessageBytes};
  std::thread io_thread;
  std::atomic<bool> stop_io{false};
  util::WakePipe wake_pipe_;  // self-pipe that breaks the I/O thread's poll() on demand
  int cached_stdout_fd_ = -1;

  // Initialization thread state
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
  int shutdown_request_id = 0;

  // Per-request pending callbacks (keyed by request id), guarded by mutex. Each
  // carries a deadline so a silent server can never strand a request (and its UI
  // loading state) forever: the I/O loop sweeps expired requests and synthesizes an
  // empty response, which every handler treats as "no result". Mirrors the DAP
  // client's timeout sweep.
  // The callback receives the transport-level outcome plus the response envelope.
  // The outcome is kOk when a real frame arrived (DispatchResultRequest then refines
  // it to kOk/kEmpty/kProtocolError by inspecting the payload); kTimeout / kUnavailable
  // when the sweep synthesizes a failure (the envelope is then an empty `{}`).
  struct PendingRequest {
    std::function<void(LspRequestOutcome, util::JsonValue)> callback;
    std::chrono::steady_clock::time_point deadline;
  };
  std::unordered_map<int, PendingRequest> pending_requests;

  // Per-request timeout; kLspRequestTimeout in production, lowered by tests so the
  // deadline sweep can be exercised without a 30s wait.
  std::chrono::milliseconds request_timeout_ = kLspRequestTimeout;

  // Callbacks marshalled to the main thread and drained once per frame. Carries
  // its own mutex + SDL wake event, so it is the innermost lock: producers may
  // post while holding `mutex`, and DrainCallbacks() never takes `mutex`.
  util::MainThreadMailbox main_mailbox;

  // When true, the client behaves as a connected server for unit tests (no subprocess).
  std::atomic<bool> test_stub_mode{false};
  // Grouped so teardown is a single Reset() and setters/clearers cannot drift out of
  // sync with the teardown list (a stale hand-maintained list once forgot one).
  struct TestHandlers {
    std::function<void(std::string, DocumentSymbolCallback)> document_symbol;
    std::function<void(std::string, SemanticTokensCallback)> semantic_tokens;
    std::function<void(std::string, HoverCallback)> hover;
    std::function<void(std::string, FormattingCallback)> formatting;
    std::function<void(std::string, std::string, RenameCallback)> rename;
    std::function<void(std::string, Position, CompletionCallback)> completion;
    std::function<void(std::string, Position, SignatureHelpCallback)> signature_help;
    std::function<void(std::string, Position, PrepareRenameCallback)> prepare_rename;
    std::function<void(std::string, WorkspaceSymbolCallback)> workspace_symbol;
    std::function<void(std::string, Range, InlayHintCallback)> inlay_hint;
    std::function<void(std::string, Position, DocumentHighlightCallback)> document_highlight;
    void Reset() { *this = TestHandlers{}; }
  };
  TestHandlers test_handlers;

  // Server semantic-token legend (index -> type name), captured at initialize.
  // Guarded by `mutex`. Empty when the server advertises no semanticTokens provider.
  std::vector<std::string> semantic_token_types;
  std::atomic<bool> supports_semantic_tokens{false};
  // Server advertised renameProvider.prepareProvider (captured at initialize).
  std::atomic<bool> supports_prepare_rename{false};
  // Server advertised an inlayHintProvider (captured at initialize).
  std::atomic<bool> supports_inlay_hints{false};
  // Server advertised a documentHighlightProvider (captured at initialize).
  std::atomic<bool> supports_document_highlight{false};

  // Negotiated position encoding (LSP `capabilities.positionEncoding`), captured at
  // initialize. Guarded by `mutex`. We advertise utf-8 first, so a conformant server
  // that supports it reports "utf-8" and our editor byte offsets are then exact LSP
  // positions with zero conversion. Per spec the default when unreported is "utf-16".
  std::string position_encoding = "utf-16";

  // Diagnostics callback — set from main thread, called on main thread via main_mailbox.
  OnPublishDiagnostics diagnostics_callback;
  // Server-initiated workspace/applyEdit handler — set from the main thread and
  // invoked on the main thread (it mutates buffers / writes files); returns
  // whether the edit was applied. Null => reply applied:false.
  std::function<bool(WorkspaceEdit)> apply_edit_handler;

  std::unordered_map<std::string, int> document_versions;
  std::deque<QueuedMessage> deferred_messages;
  std::deque<QueuedMessage> outbound_messages;
  // Aggregate approximate bytes across deferred_messages + outbound_messages, guarded
  // by send_mutex. Charged at enqueue, released when a batch drains / the queues are
  // cleared. Bounds retained payload under the message-count cap (TD-2026-07-17A-071).
  std::size_t outbound_queued_bytes_ = 0;
  // Effective byte budget (kMaxQueuedBytes in production; lowered in tests).
  std::size_t max_queued_bytes_ = kMaxQueuedBytes;
  // One-shot guard so the wedged-server overflow warning logs once, not per message.
  bool outbound_overflow_logged_ = false;
  int next_id = 1;
  std::string root_uri;
  std::string language_id;
  util::JsonValue initialization_options;  // LSP initializationOptions (object or Null)
  util::JsonValue settings;                // answers workspace/configuration (object or Null)
  std::string last_error;
  std::string last_error_snapshot;
  LspClient::ReadinessSnapshot readiness_snapshot;
  std::atomic<bool> initialized{false};
  std::atomic<bool> supports_incremental_sync{false};

  void SetLastError(std::string message) {
    std::lock_guard lock(mutex);
    last_error = std::move(message);
    readiness_snapshot.state = LspClient::ReadinessSnapshot::State::Failed;
    readiness_snapshot.message = last_error;
    readiness_snapshot.indexed_count = 0;
  }

  std::string GetLastErrorCopy() {
    std::lock_guard lock(mutex);
    return last_error;
  }

  int GetNextId() {
    std::lock_guard lock(mutex);
    return next_id++;
  }

  util::JsonValue MakeRequest(int id, const std::string& method, const util::JsonValue& params) {
    using namespace util;
    JsonObject req;
    req["jsonrpc"] = JsonValue("2.0");
    req["id"] = JsonValue(static_cast<std::int64_t>(id));
    req["method"] = JsonValue(method);
    req["params"] = params;
    return JsonValue(std::move(req));
  }

  util::JsonValue MakeNotification(const std::string& method, const util::JsonValue& params) {
    using namespace util;
    JsonObject msg;
    msg["jsonrpc"] = JsonValue("2.0");
    msg["method"] = JsonValue(method);
    msg["params"] = params;
    return JsonValue(std::move(msg));
  }

  // Seed for a server-request reply envelope; callers add "result" or "error".
  // `id` is echoed verbatim (int or string per JSON-RPC).
  util::JsonValue MakeResponse(const util::JsonValue& id) {
    using namespace util;
    JsonObject msg;
    msg["jsonrpc"] = JsonValue("2.0");
    msg["id"] = id;
    return JsonValue(std::move(msg));
  }

  std::string SerializeMessage(const util::JsonValue& msg) const {
    const std::string json = util::SerializeJson(msg);
    std::string rfc;
    rfc.reserve(32 + json.size());
    rfc += "Content-Length: ";
    rfc += std::to_string(json.size());
    rfc += "\r\n\r\n";
    rfc += json;
    return rfc;
  }

  // Self-pipe wakeup: the I/O thread blocks in poll() over stdout + wake_pipe_.read_fd();
  // enqueuing outbound work calls Wake() to break the poll immediately. See util::WakePipe.
  void OpenWakePipe() { wake_pipe_.Open(); }
  void CloseWakePipe() { wake_pipe_.Close(); }
  void Wake() { wake_pipe_.Wake(); }

  // Flush every queued outbound message. Runs only on the I/O thread; holds
  // write_mutex across the proc.Write calls so it never races a shutdown-time
  // SendMessageImmediate on stdin. Defined in WorkspaceLspClientTransport.cpp.
  void DrainOutbound();

  // lock_timeout > 0 bounds how long we wait for write_mutex before giving up:
  // used only by the shutdown path so a stuck io_thread write cannot wedge
  // teardown. Returns false on lock timeout (as well as on a failed write); the
  // shutdown caller treats both as "graceful send failed" and force-kills.
  // Defined in WorkspaceLspClientTransport.cpp (SendMessageImmediate keeps this
  // exact qualified name — tsan.supp targets it by mangled symbol).
  bool SendMessageImmediate(const util::JsonValue& msg, bool allow_during_shutdown = false,
                            std::chrono::milliseconds lock_timeout = std::chrono::milliseconds::zero());

  // Defer serialization to the I/O thread: move the message into the outbound
  // builder so SerializeMessage (const, no shared state) runs during DrainOutbound
  // rather than on the calling (often UI) thread. Ordering is fixed at enqueue time
  // under send_mutex, so lazy serialization never reorders. The shutdown/initialize
  // paths use SendMessageImmediate instead and stay eager.
  bool SendMessageAfterInitialize(util::JsonValue msg) {
    return SendMessageBuilderAfterInitialize(
        [this, msg = std::move(msg)]() { return SerializeMessage(msg); });
  }

  // Queue a message for the I/O thread (deferred until initialized). Refuses on a
  // wedged/dead server rather than growing unbounded. `approx_bytes` is the payload's
  // approximate size, charged against the aggregate outbound byte budget (0 for small
  // control frames). Defined in WorkspaceLspClientTransport.cpp.
  bool SendMessageBuilderAfterInitialize(std::function<std::string()> build_serialized,
                                         std::size_t approx_bytes = 0);

  void ClearDeferredMessages() {
    std::lock_guard lock(send_mutex);
    deferred_messages.clear();
    outbound_messages.clear();
    outbound_queued_bytes_ = 0;
  }

  void ResetProtocolState() {
    std::lock_guard lock(mutex);
    pending_requests.clear();
    main_mailbox.Clear();
    document_versions.clear();
    shutdown_response_received = false;
    shutdown_request_id = 0;
    readiness_snapshot = LspClient::ReadinessSnapshot{};
  }

  static int ExtractIndexedCount(std::string_view text) {
    for (std::size_t i = 0; i < text.size(); ++i) {
      if (!util::IsAsciiDigit(static_cast<unsigned char>(text[i]))) {
        continue;
      }
      // Accumulate in 64-bit and saturate: the digit run comes from
      // server-controlled $/progress text, so a long run would overflow a plain
      // int (signed-overflow UB). Clamp to INT_MAX — the exact count past that is
      // meaningless for a readiness heuristic.
      std::int64_t value = 0;
      std::size_t cursor = i;
      while (cursor < text.size() &&
             util::IsAsciiDigit(static_cast<unsigned char>(text[cursor]))) {
        value = value * 10 + (text[cursor] - '0');
        if (value > std::numeric_limits<int>::max()) {
          value = std::numeric_limits<int>::max();
        }
        ++cursor;
      }
      return static_cast<int>(value);
    }
    return 0;
  }

  // Update the readiness snapshot from a $/progress notification's value. Defined
  // in WorkspaceLspClientLifecycle.cpp.
  void SetProgressReadiness(const util::JsonValue& value);

  void ShutdownProcessOnce(int timeout_ms = 3000) {
    bool expected = false;
    if (!process_shutdown_started.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
      return;
    }
    proc.Shutdown(timeout_ms);
  }

  // Wait until stdout has data/EOF or an outbound wake arrives. Returns true when
  // stdout should be read. On POSIX this is a single poll() over stdout + the wake
  // pipe, so an idle server makes no fixed-cadence wakeups; elsewhere it degrades
  // to a short read timeout. Defined in WorkspaceLspClientTransport.cpp.
  bool WaitStdoutReadable(int timeout_ms);

  void ParseBufferedMessages() {
    while (true) {
      const std::size_t buffered_before = framer_.BufferedBytes();
      auto msg_opt = framer_.Next();
      if (msg_opt) {
        DispatchMessage(std::move(*msg_opt));
        continue;
      }
      // nullopt is overloaded: either the framer consumed a frame it could not
      // surface (malformed header / oversized / invalid JSON) — in which case any
      // well-formed frames already buffered behind it must still be drained now,
      // not ~1s later on the next poll — or no complete frame is available yet, in
      // which case the buffer is unchanged and we wait for more bytes.
      if (framer_.BufferedBytes() == buffered_before) break;
    }
  }

  // The single I/O thread body: parse buffered frames, drain outbound, sweep
  // timed-out requests, then poll stdout. Defined in
  // WorkspaceLspClientTransport.cpp.
  void IoMain();

  // Reply to a server-initiated request. `id` is echoed verbatim (it may be an
  // int or a string per JSON-RPC). Defined in WorkspaceLspClientDispatch.cpp.
  void SendResponseResult(const util::JsonValue& id, util::JsonValue result);
  void SendResponseError(const util::JsonValue& id, int code, std::string message);

  // Server -> client requests must always get a reply, or chatty servers
  // (clangd, Roslyn/OmniSharp) log errors or stall. Defined in
  // WorkspaceLspClientDispatch.cpp.
  void HandleServerRequest(const util::JsonValue& id, const std::string& method,
                           const util::JsonValue& params);

  // Route one inbound frame: response callback, server request, diagnostics, or
  // progress. Defined in WorkspaceLspClientDispatch.cpp.
  void DispatchMessage(util::JsonValue msg);

  int RegisterPendingRequest(std::function<void(LspRequestOutcome, util::JsonValue)> cb) {
    std::lock_guard lock(mutex);
    const int id = next_id++;
    pending_requests[id] = PendingRequest{
        .callback = std::move(cb),
        .deadline = std::chrono::steady_clock::now() + request_timeout_,
    };
    return id;
  }

  // Returns true iff this call actually removed a still-pending entry. The
  // send-failure path uses that to avoid double-delivering a callback the I/O
  // thread's EOF sweep (FailPendingRequests) may have already failed and erased in
  // the window between RegisterPendingRequest and a failed SendMessageAfterInitialize.
  bool RemovePendingRequest(int id) {
    std::lock_guard lock(mutex);
    return pending_requests.erase(id) != 0;
  }

  // Synthesize empty responses for pending requests so a non-responding server never
  // strands a request (and its UI loading state). `only_expired` fails just those
  // past their deadline (the periodic sweep); otherwise fails all (the server exited
  // and no response can ever arrive). Runs on the I/O thread: handlers are posted to
  // the main-thread mailbox. The empty `{}` envelope has no "result" key, so every
  // response handler degrades to its no-result path.
  void FailPendingRequests(bool only_expired);

  // Test-stub fast path shared by every interactive request. Returns true when the
  // client is in stub mode (the caller then returns): posts a main-thread task that
  // calls the installed handler with the request's args, or delivers std::nullopt
  // when no handler is installed. Collapses the per-request lock + test_stub_mode
  // check + PostWithoutWake boilerplate. `args` are forwarding references and are
  // consumed ONLY on the stub branch, so `callback`/`uri` remain intact for the real
  // request path when this returns false.
  template <typename Handler, typename Callback, typename... Args>
  bool DispatchTestStub(const Handler& handler_member, Callback& callback, Args&&... args) {
    std::lock_guard lock(mutex);
    if (!test_stub_mode.load(std::memory_order_acquire)) {
      return false;
    }
    Handler handler = handler_member;  // copy under lock, as the per-method code did
    main_mailbox.PostWithoutWake(
        [handler = std::move(handler), cb = std::move(callback),
         ... args = std::forward<Args>(args)]() mutable {
          if (handler) {
            handler(std::move(args)..., std::move(cb));
          } else {
            cb(std::nullopt);
          }
        });
    return true;
  }

  // Shared scaffolding for every async request: register the response handler,
  // send the request, and on send failure clean up and invoke the failure path.
  // Keeps the per-method code to "build params" + "parse result".
  void DispatchRequest(const std::string& method, util::JsonValue params,
                       std::function<void(LspRequestOutcome, util::JsonValue)> response_handler,
                       std::function<void()> on_send_failure) {
    const int id = RegisterPendingRequest(std::move(response_handler));
    if (!SendMessageAfterInitialize(MakeRequest(id, method, std::move(params)))) {
      // Only report the failure if the entry was still ours to fail. If the EOF
      // sweep already claimed it, it also already invoked the handler — running
      // on_send_failure() again would double-fire the callback.
      if (RemovePendingRequest(id)) {
        on_send_failure();
      }
    }
  }

  // Result-shaped request helper: owns the boilerplate every feature request
  // repeats — the empty-`callback` guard, the "no `result` key" -> nullopt path,
  // and the send-failure -> nullopt path — so a request method reduces to "build
  // params + parse result". `parse_result` maps the response's `result` value to
  // the callback's argument (an `std::optional<...>`); the absent/failure cases
  // deliver a default-constructed argument (i.e. `std::nullopt`).
  template <typename Callback, typename Parser>
  void DispatchResultRequest(const std::string& method, util::JsonValue params, Callback callback,
                             Parser parse_result) {
    if (!callback) {
      return;
    }
    // T is the payload the parser yields (parse_result returns std::optional<T>);
    // the callback takes LspResult<T>.
    using OptT = std::decay_t<std::invoke_result_t<Parser&, util::JsonValue&>>;
    using ResultT = LspResult<typename OptT::value_type>;
    Callback failure = callback;
    DispatchRequest(
        method, std::move(params),
        [cb = std::move(callback), parse_result = std::move(parse_result)](
            LspRequestOutcome transport, util::JsonValue resp) mutable {
          // A synthesized transport failure (timeout / server-gone) carries no
          // payload: deliver the reason so the caller does not mistake it for an
          // authoritative empty answer.
          if (transport != LspRequestOutcome::kOk) {
            cb(ResultT(transport, std::nullopt));
            return;
          }
          // A real frame arrived. A JSON-RPC error response, or a frame with neither
          // `result` nor `error`, is a protocol error — again NOT an empty answer.
          if (util::JsonValue* error = resp.MutableAt("error"); error != nullptr && !error->IsNull()) {
            cb(ResultT(LspRequestOutcome::kProtocolError, std::nullopt));
            return;
          }
          // The response is owned here, so hand the `result` subtree to the parser
          // as a mutable lvalue: hot parsers (completion/code-action) move their
          // strings out instead of copying. MutableAt is non-null iff the key is
          // present (matches the old HasKey guard, incl. a null `result`).
          util::JsonValue* result = resp.MutableAt("result");
          if (result == nullptr) {
            cb(ResultT(LspRequestOutcome::kProtocolError, std::nullopt));
            return;
          }
          OptT parsed = parse_result(*result);
          const LspRequestOutcome outcome =
              parsed.has_value() ? LspRequestOutcome::kOk : LspRequestOutcome::kEmpty;
          cb(ResultT(outcome, std::move(parsed)));
        },
        [failure = std::move(failure)]() mutable {
          failure(ResultT(LspRequestOutcome::kUnavailable, std::nullopt));
        });
  }

  // The blocking initialize handshake: advertise client capabilities, read and
  // parse the server's initialize response (sync kind, position encoding,
  // semantic-token legend), send `initialized`, then flush queued config/didOpen
  // and spawn the I/O thread. Runs on init_thread. Defined in
  // WorkspaceLspClientLifecycle.cpp.
  void DoInitializeBlocking();

  // Graceful shutdown: shutdown/exit handshake with bounded waits, then join the
  // I/O thread and force-kill if the server does not exit. Runs on
  // shutdown_thread. Defined in WorkspaceLspClientLifecycle.cpp.
  void DoShutdown();

  void BeginShutdown();
  void WaitForShutdown();
};

}  // namespace microide::workspace
