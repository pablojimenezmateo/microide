#pragma once

#include "workspace/lsp/WorkspaceLspClient.h"
#include "workspace/lsp/LspFileWatchRegistry.h"
#include "workspace/lsp/LspProtocol.h"
#include "workspace/StdioClientQueue.h"
#include "workspace/StdioJsonRpcClientTransport.h"
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
#include <filesystem>
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
#include "workspace/lsp/LspClientTrace.h"
#include "workspace/JsonRpcMessageFraming.h"
#include "util/StringUtil.h"

namespace microide::workspace {

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------
// The transport half (I/O thread, outbound queues, framing, caps) lives in the
// shared StdioJsonRpcClientTransport mixin — one implementation for the LSP and
// DAP clients, which used to carry drifting copies of it. Impl supplies the
// protocol half the mixin calls back into: DispatchMessage, FailPendingRequests,
// SetLastError, TraceSend, WakeMainThread.
struct LspClient::Impl : StdioJsonRpcClientTransport<LspClient::Impl> {
  Impl()
      : StdioJsonRpcClientTransport(StdioTransportConfig{
            .messages_sent = util::PerfCounterId::LspMessagesSent,
            .bytes_sent = util::PerfCounterId::LspBytesSent,
            .bytes_received = util::PerfCounterId::LspBytesReceived,
            .max_message_bytes = kMaxLspMessageBytes,
            .max_read_buffer_bytes = kMaxLspReadBufferBytes,
            .max_queued_messages = kMaxQueuedMessages,
            .max_queued_bytes = kMaxQueuedBytes,
            .peer_label = "lsp",
            .send_error = "failed to send message to language server",
        }) {}

  // No per-message send trace; LSP tracing is lifecycle-level (LspClientTrace).
  void TraceSend(const util::JsonValue&) const {}
  void WakeMainThread() { main_mailbox.PushWake(); }

  std::mutex mutex;

  // Initialization thread state
  std::thread init_thread;
  std::atomic<bool> initializing{false};
  std::atomic<bool> stop_init{false};
  std::thread shutdown_thread;
  std::atomic<bool> shutdown_started{false};
  std::atomic<bool> shutdown_complete{false};
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
    // For round-trip attribution. `deadline` cannot stand in for this: tests
    // shorten request_timeout_, so deadline-minus-timeout would drift.
    std::chrono::steady_clock::time_point sent_at;
  };
  std::unordered_map<int, PendingRequest> pending_requests;

  // Per-request timeout; kLspRequestTimeout in production, lowered by tests so the
  // deadline sweep can be exercised without a 30s wait.
  std::chrono::milliseconds request_timeout_ = kLspRequestTimeout;

  // Callbacks marshalled to the main thread and drained once per frame. Carries
  // its own mutex + SDL wake event, so it is the innermost lock: producers may
  // post while holding `mutex`, and DrainCallbacks() never takes `mutex`.
  util::MainThreadMailbox main_mailbox;

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
    std::function<void(std::string, CodeLensCallback)> code_lens;
    std::function<void(util::JsonValue, ResolveCodeLensCallback)> resolve_code_lens;
    std::function<void(std::string, std::vector<util::JsonValue>, ExecuteCommandCallback)>
        execute_command;
    std::function<void(std::string, Position, PrepareCallHierarchyCallback)>
        prepare_call_hierarchy;
    // One handler for both directions; `incoming` says which was asked for.
    std::function<void(bool, util::JsonValue, CallHierarchyCallsCallback)> call_hierarchy_calls;
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
  // Server advertised a codeLensProvider, and whether it carries resolveProvider
  // (i.e. whether range-only lenses can be filled in via codeLens/resolve).
  std::atomic<bool> supports_code_lens{false};
  std::atomic<bool> supports_code_lens_resolve{false};
  // Server advertised a callHierarchyProvider (captured at initialize).
  std::atomic<bool> supports_call_hierarchy{false};

  // File watchers the server registered via client/registerCapability for
  // workspace/didChangeWatchedFiles. Written on the I/O thread (registrations
  // arrive as server requests), read on the shell thread (one query per changed
  // file). Guarded by `mutex`; `has_file_watchers` is the lock-free gate so the
  // overwhelmingly common "server registered nothing" case costs one relaxed load
  // per batch instead of a lock per file.
  LspFileWatchRegistry file_watch_registry;
  std::atomic<bool> has_file_watchers{false};

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
  // Highest version each URI reached before it was last closed. A reopen resumes
  // from here + 1 instead of restarting at 1, which is what makes the staleness
  // gate survive a close -> reopen: a publishDiagnostics still in flight from the
  // previous open carries a version from the OLD numbering, and against a
  // restarted-at-1 document `old_version < 1` is false, so it was accepted and
  // painted on the freshly reopened buffer until the next republish. Nothing in
  // the protocol requires didOpen to start at 1 — `TextDocumentItem.version` is
  // just "the version number of this document" — so keeping it monotonic per URI
  // costs nothing on the wire and removes the whole window.
  //
  // Entries are only added on close, so this holds at most one int per file the
  // session has opened and closed; a document that is still open lives in
  // document_versions.
  std::unordered_map<std::string, int> retired_document_versions;
  int next_id = 1;
  std::string root_uri;
  // root_uri decoded once at Start(), so the didChangeWatchedFiles registration
  // path can resolve RelativePattern base URIs without re-parsing per call.
  std::filesystem::path root_path;
  std::string language_id;
  util::JsonValue initialization_options;  // LSP initializationOptions (object or Null)
  util::JsonValue settings;                // answers workspace/configuration (object or Null)
  std::string last_error;
  std::string last_error_snapshot;
  LspClient::ReadinessSnapshot readiness_snapshot;
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

  void ResetProtocolState() {
    std::lock_guard lock(mutex);
    pending_requests.clear();
    main_mailbox.Clear();
    document_versions.clear();
    // A restarted server has no memory of the old numbering, so neither should we.
    retired_document_versions.clear();
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
    const auto now = std::chrono::steady_clock::now();
    pending_requests[id] = PendingRequest{
        .callback = std::move(cb),
        .deadline = now + request_timeout_,
        .sent_at = now,
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
