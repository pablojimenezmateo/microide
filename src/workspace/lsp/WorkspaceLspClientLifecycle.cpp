// LspClient::Impl lifecycle: the blocking initialize handshake (capability
// advertisement + response parsing), graceful shutdown, and the progress-driven
// readiness state machine. Split out of WorkspaceLspClientInternal.h — these are
// the bulk of the former 1332-line transport header.
#include "workspace/lsp/WorkspaceLspClientInternal.h"

#include <algorithm>
#include <chrono>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "util/StartupTrace.h"
#include "util/ThreadName.h"

namespace microide::workspace {

void LspClient::Impl::SetProgressReadiness(const util::JsonValue& value) {
  const std::string& kind = value["kind"].AsString();
  const std::string& title = value["title"].AsString();
  const std::string& message = value["message"].AsString();
  const int percentage = value["percentage"].AsInt(0);

  {
    std::lock_guard lock(mutex);
    if (kind == "end") {
      readiness_snapshot.state = initialized.load(std::memory_order_acquire)
                                     ? LspClient::ReadinessSnapshot::State::Ready
                                     : LspClient::ReadinessSnapshot::State::Starting;
      readiness_snapshot.message.clear();
      readiness_snapshot.indexed_count = 0;
    } else {
      readiness_snapshot.state = LspClient::ReadinessSnapshot::State::Indexing;
      // The server's own progress text is genuine extra detail (it names what is
      // being indexed); the bare state name is not, so leave it empty when the
      // server said nothing and let the shared vocabulary supply "Indexing…".
      readiness_snapshot.message = !message.empty() ? message : title;
      readiness_snapshot.indexed_count = std::max({ExtractIndexedCount(message),
                                                   ExtractIndexedCount(title), percentage});
    }
  }
  TraceLifecycle("progress", kind.empty() ? "report" : kind);
}

namespace {

using util::JsonArray;
using util::JsonObject;
using util::JsonValue;

JsonValue StringArray(std::initializer_list<const char*> values) {
  JsonArray array;
  array.reserve(values.size());
  for (const char* value : values) {
    array.push_back(JsonValue(value));
  }
  return JsonValue(std::move(array));
}

// The capability object nearly every feature advertises: statically registered.
JsonObject StaticRegistration() {
  JsonObject caps;
  caps["dynamicRegistration"] = JsonValue(false);
  return caps;
}

// typeDefinition / implementation / declaration share definition's link support.
JsonObject NavigationCapabilities() {
  JsonObject caps = StaticRegistration();
  caps["linkSupport"] = JsonValue(true);
  return caps;
}

JsonObject TextDocumentClientCapabilities() {
  JsonObject text_doc_sync = StaticRegistration();
  text_doc_sync["willSave"] = JsonValue(false);
  text_doc_sync["didSave"] = JsonValue(true);

  JsonObject completion_item_caps;
  completion_item_caps["snippetSupport"] = JsonValue(true);
  completion_item_caps["documentationFormat"] = StringArray({"markdown", "plaintext"});
  JsonObject completion_caps = StaticRegistration();
  completion_caps["completionItem"] = JsonValue(std::move(completion_item_caps));
  // The request carries `context.triggerKind`, so a re-request for an incomplete
  // list is announced as one (TriggerForIncompleteCompletions).
  completion_caps["contextSupport"] = JsonValue(true);

  JsonObject hover_caps = StaticRegistration();
  hover_caps["contentFormat"] = StringArray({"markdown", "plaintext"});

  JsonObject signature_caps = StaticRegistration();
  {
    JsonObject parameter_info;
    // We resolve `[start, end]` parameter-label offsets against the signature label.
    parameter_info["labelOffsetSupport"] = JsonValue(true);
    JsonObject signature_info;
    signature_info["parameterInformation"] = JsonValue(std::move(parameter_info));
    // Per-signature activeParameter overrides the top-level value (LSP 3.16+).
    signature_info["activeParameterSupport"] = JsonValue(true);
    signature_info["documentationFormat"] = StringArray({"markdown", "plaintext"});
    signature_caps["signatureInformation"] = JsonValue(std::move(signature_info));
  }

  JsonObject rename_caps = StaticRegistration();
  rename_caps["prepareSupport"] = JsonValue(true);

  JsonObject code_action_caps = StaticRegistration();
  {
    JsonObject kind_obj;
    kind_obj["valueSet"] =
        StringArray({"quickfix", "refactor", "refactor.extract", "refactor.inline",
                     "refactor.rewrite", "source", "source.organizeImports"});
    JsonObject literal;
    literal["codeActionKind"] = JsonValue(std::move(kind_obj));
    code_action_caps["codeActionLiteralSupport"] = JsonValue(std::move(literal));
  }

  JsonObject semantic_tokens_caps = StaticRegistration();
  {
    JsonObject requests;
    requests["full"] = JsonValue(true);
    semantic_tokens_caps["requests"] = JsonValue(std::move(requests));
    semantic_tokens_caps["formats"] = StringArray({"relative"});
    // The server still picks its own legend; we map its token-type indices by
    // name at publish time, so an empty client tokenTypes/tokenModifiers set is
    // fine (we accept whatever legend the server reports).
    semantic_tokens_caps["tokenTypes"] = JsonValue(JsonArray{});
    semantic_tokens_caps["tokenModifiers"] = JsonValue(JsonArray{});
  }

  JsonObject text_document_caps;
  text_document_caps["synchronization"] = JsonValue(std::move(text_doc_sync));
  text_document_caps["completion"] = JsonValue(std::move(completion_caps));
  text_document_caps["hover"] = JsonValue(std::move(hover_caps));
  text_document_caps["signatureHelp"] = JsonValue(std::move(signature_caps));
  text_document_caps["definition"] = JsonValue(NavigationCapabilities());
  text_document_caps["typeDefinition"] = JsonValue(NavigationCapabilities());
  text_document_caps["implementation"] = JsonValue(NavigationCapabilities());
  text_document_caps["declaration"] = JsonValue(NavigationCapabilities());
  text_document_caps["references"] = JsonValue(StaticRegistration());
  text_document_caps["rename"] = JsonValue(std::move(rename_caps));
  text_document_caps["codeAction"] = JsonValue(std::move(code_action_caps));
  text_document_caps["formatting"] = JsonValue(StaticRegistration());
  text_document_caps["rangeFormatting"] = JsonValue(StaticRegistration());
  text_document_caps["documentSymbol"] = JsonValue(StaticRegistration());
  text_document_caps["semanticTokens"] = JsonValue(std::move(semantic_tokens_caps));
  {
    // Pushed diagnostics. Some servers publish only to a client that says it takes
    // them -- typescript-language-server reported nothing at all without this, while
    // clangd, which pushes regardless, hid the gap. versionSupport: a publish for a
    // superseded document version is dropped by version (DispatchMessage).
    JsonObject publish_diagnostics_caps;
    publish_diagnostics_caps["versionSupport"] = JsonValue(true);
    text_document_caps["publishDiagnostics"] = JsonValue(std::move(publish_diagnostics_caps));
  }
  text_document_caps["inlayHint"] = JsonValue(StaticRegistration());
  text_document_caps["documentHighlight"] = JsonValue(StaticRegistration());
  text_document_caps["codeLens"] = JsonValue(StaticRegistration());
  text_document_caps["callHierarchy"] = JsonValue(StaticRegistration());
  return text_document_caps;
}

JsonObject WorkspaceClientCapabilities() {
  JsonObject workspace_caps;
  workspace_caps["configuration"] = JsonValue(true);
  workspace_caps["workspaceFolders"] = JsonValue(true);
  workspace_caps["symbol"] = JsonValue(StaticRegistration());
  // Activating a code lens runs its command through workspace/executeCommand.
  workspace_caps["executeCommand"] = JsonValue(StaticRegistration());
  // Server-initiated edits are applied by the host (open buffers in place, closed
  // files silently on disk) via the bound apply-edit handler.
  workspace_caps["applyEdit"] = JsonValue(true);
  {
    // WorkspaceEdit support: the ordered `documentChanges` shape, file
    // create/rename/delete resource ops (validated up front, applied with
    // rollback-safe staging), and versioned TextDocumentEdits (a stale expected
    // version fails the whole edit). Failure handling matches VSCode: text-only
    // edits are transactional; an edit containing resource ops aborts at the
    // first failure (our applier additionally rolls back completed ops).
    JsonObject workspace_edit_caps;
    workspace_edit_caps["documentChanges"] = JsonValue(true);
    workspace_edit_caps["resourceOperations"] = StringArray({"create", "rename", "delete"});
    workspace_edit_caps["failureHandling"] = JsonValue("textOnlyTransactional");
    workspace_caps["workspaceEdit"] = JsonValue(std::move(workspace_edit_caps));
  }
  workspace_caps["didChangeConfiguration"] = JsonValue(StaticRegistration());
  {
    // On-disk changes the editor did not make (a `git switch`, a `pull`, a
    // generated header) reach the server only through this notification. Without
    // it every file the user has not opened keeps its pre-change index, so
    // go-to-definition lands on stale lines and diagnostics name deleted symbols
    // until the file happens to be opened.
    //
    // dynamicRegistration is true because that is the only way a server can tell
    // us which globs it cares about, and a branch switch in a large repository is
    // thousands of paths — filtering by the server's own registration is what
    // keeps the fan-out cheap. relativePatternSupport lets a server anchor a
    // pattern to a base URI instead of spelling an absolute glob.
    JsonObject did_change_watched_files;
    did_change_watched_files["dynamicRegistration"] = JsonValue(true);
    did_change_watched_files["relativePatternSupport"] = JsonValue(true);
    workspace_caps["didChangeWatchedFiles"] = JsonValue(std::move(did_change_watched_files));
  }
  return workspace_caps;
}

JsonObject ClientCapabilities() {
  JsonObject window_caps;
  window_caps["workDoneProgress"] = JsonValue(true);

  // The editor measures columns in UTF-8 bytes, which is exactly LSP's UTF-8
  // position encoding. Advertise it first (then UTF-16 as the mandatory
  // fallback) so a conformant server emits/consumes byte offsets and positions
  // past non-ASCII characters stay correct. Without this the server assumes the
  // UTF-16 default and our byte offsets corrupt every position past any
  // non-ASCII char on the incremental-sync mirror.
  JsonObject general_caps;
  general_caps["positionEncodings"] = StringArray({"utf-8", "utf-16"});

  JsonObject caps;
  caps["textDocument"] = JsonValue(TextDocumentClientCapabilities());
  caps["workspace"] = JsonValue(WorkspaceClientCapabilities());
  caps["window"] = JsonValue(std::move(window_caps));
  caps["general"] = JsonValue(std::move(general_caps));
  return caps;
}

JsonValue InitializeParams(const std::string& root_uri, const JsonValue& initialization_options) {
  JsonObject client_info;
  client_info["name"] = JsonValue("microide");

  JsonObject init_params;
  init_params["processId"] = JsonValue(static_cast<std::int64_t>(::getpid()));
  init_params["clientInfo"] = JsonValue(std::move(client_info));
  init_params["rootUri"] = JsonValue(root_uri);
  if (!root_uri.empty()) {
    JsonObject folder;
    folder["uri"] = JsonValue(root_uri);
    const auto slash = root_uri.find_last_of('/');
    folder["name"] =
        JsonValue(slash == std::string::npos ? root_uri : root_uri.substr(slash + 1));
    JsonArray folders;
    folders.push_back(JsonValue(std::move(folder)));
    init_params["workspaceFolders"] = JsonValue(std::move(folders));
  }
  if (!initialization_options.IsNull()) {
    init_params["initializationOptions"] = initialization_options;
  }
  init_params["capabilities"] = JsonValue(ClientCapabilities());
  return JsonValue(std::move(init_params));
}

// A `*Provider` server capability that may be a bare bool or an options object;
// either truthy shape means the server serves the request.
bool ProviderAdvertised(const JsonValue& provider) {
  return provider.IsObject() || provider.AsBool(false);
}

}  // namespace

void LspClient::Impl::ApplyServerCapabilities(const util::JsonValue& server_caps) {
  const auto& sync = server_caps["textDocumentSync"];
  int sync_kind = 1;
  if (sync.IsInt()) {
    sync_kind = sync.AsInt();
  } else if (sync.HasKey("change")) {
    sync_kind = sync["change"].AsInt(1);
  }
  supports_incremental_sync.store(sync_kind == 2, std::memory_order_release);

  // Completion trigger characters (TD-2026-10-07-321) and signature-help
  // trigger characters (TD-2026-10-20-369): typing one opens the list or
  // the parameter hints, as VS Code does.
  completion_triggers.Store(server_caps["completionProvider"]["triggerCharacters"]);
  signature_help_triggers.Store(server_caps["signatureHelpProvider"]["triggerCharacters"]);

  // Capture the negotiated position encoding. utf-8 means our editor
  // byte offsets are already exact; utf-16/utf-32 are converted per line
  // at every position seam (lsp_encoding::*), and the per-keystroke
  // incremental sync falls back to full-document sync for those so the
  // pre-edit byte range never needs re-encoding.
  {
    std::string negotiated = "utf-16";
    if (server_caps.HasKey("positionEncoding") && server_caps["positionEncoding"].IsString()) {
      negotiated = server_caps["positionEncoding"].AsString();
    }
    std::lock_guard lock(mutex);
    position_encoding = negotiated;
  }

  // Capture the semantic-token legend (type names) so the host can map
  // a response's token-type indices back to names at publish time.
  if (server_caps.HasKey("semanticTokensProvider")) {
    const auto& provider = server_caps["semanticTokensProvider"];
    if (provider.HasKey("legend") && provider["legend"].HasKey("tokenTypes")) {
      const auto& types = provider["legend"]["tokenTypes"];
      if (types.IsArray()) {
        const auto& array = types.AsArray();
        // Cap the harvested legend like the steady-state result parsers: a
        // buggy/hostile server can advertise a huge tokenTypes array in the
        // initialize response, which would be duplicated into a long-lived
        // vector (and copied by value on every SemanticTokenLegend() call) for
        // a feature that only maps a small set of token-type indices to names.
        // Token types past the cap are not useful for rendering. (TD-32.)
        constexpr std::size_t kMaxSemanticTokenLegendEntries = 1024;
        const std::size_t harvested = std::min(array.size(), kMaxSemanticTokenLegendEntries);
        std::vector<std::string> legend;
        legend.reserve(harvested);
        for (std::size_t i = 0; i < harvested; ++i) {
          legend.push_back(array[i].AsString());
        }
        std::lock_guard lock(mutex);
        semantic_token_types = std::move(legend);
      }
      supports_semantic_tokens.store(true, std::memory_order_release);
    }
  }

  // inlayHintProvider: bare bool or an object (resolveProvider).
  if (server_caps.HasKey("inlayHintProvider")) {
    supports_inlay_hints.store(ProviderAdvertised(server_caps["inlayHintProvider"]),
                               std::memory_order_release);
  }
  // documentHighlightProvider: bare bool or a WorkDoneProgressOptions object.
  if (server_caps.HasKey("documentHighlightProvider")) {
    supports_document_highlight.store(
        ProviderAdvertised(server_caps["documentHighlightProvider"]), std::memory_order_release);
  }
  // callHierarchyProvider: bare bool or a (Static)RegistrationOptions object.
  if (server_caps.HasKey("callHierarchyProvider")) {
    supports_call_hierarchy.store(ProviderAdvertised(server_caps["callHierarchyProvider"]),
                                  std::memory_order_release);
  }
  // codeLensProvider is always an object when present, and its optional
  // `resolveProvider` decides whether range-only lenses can be filled in.
  if (server_caps.HasKey("codeLensProvider")) {
    const auto& provider = server_caps["codeLensProvider"];
    const bool provided = ProviderAdvertised(provider);
    supports_code_lens.store(provided, std::memory_order_release);
    supports_code_lens_resolve.store(provided && provider["resolveProvider"].AsBool(false),
                                     std::memory_order_release);
  }
  // renameProvider may be a bare bool (plain rename) or an object that can
  // carry prepareProvider (textDocument/prepareRename support).
  if (server_caps.HasKey("renameProvider")) {
    const auto& rename_provider = server_caps["renameProvider"];
    if (rename_provider.HasKey("prepareProvider")) {
      supports_prepare_rename.store(rename_provider["prepareProvider"].AsBool(false),
                                    std::memory_order_release);
    }
  }
}

void LspClient::Impl::DoInitializeBlocking() {
  util::StartupTrace::Scope trace_scope("LspClient::DoInitializeBlocking");
  TraceLifecycle("init-thread-begin");
  initializing.store(true, std::memory_order_release);
  {
    std::lock_guard lock(mutex);
    readiness_snapshot.state = LspClient::ReadinessSnapshot::State::Starting;
    readiness_snapshot.message.clear();
    readiness_snapshot.indexed_count = 0;
  }

  const int init_id = GetNextId();
  const auto req = MakeRequest(init_id, "initialize", InitializeParams(root_uri, initialization_options));
  if (!SendMessageImmediate(req)) {
    SetLastError("failed to send initialize request to language server");
    AbandonInitialize();
    return;
  }
  TraceLifecycle("initialize-request", "sent");

  bool got_init = false;
  std::string init_error;  // the server's message when it answered initialize with an error
  {
    util::StartupTrace::Scope wait_init_scope("LspClient::DoInitializeBlocking::WaitInitializeResponse");
    // Bound the initialize wait by WALL-CLOCK, not an attempt count. The old
    // `for (attempts < 60)` treated each loop turn as ~500 ms, but proc.Read returns
    // as soon as any data is available and dispatching a buffered frame does not read
    // at all — so a chatty server emitting a burst of notifications (window/logMessage,
    // $/progress) before its initialize response burned dozens of turns in milliseconds
    // and got force-killed while perfectly healthy. A steady_clock deadline spends the
    // budget only as real time elapses; frame supply is bounded per read, so this never
    // spins hot.
    const auto init_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < init_deadline) {
      if (stop_init.load(std::memory_order_acquire)) {
        AbandonInitialize();
        return;
      }
      auto resp_opt = framer_.Next();
      if (!resp_opt) {
        auto chunk = proc.Read(4096, 500);
        if (!chunk) break;
        if (!chunk->empty()) framer_.Append(*chunk);
        if (framer_.BufferedBytes() > kMaxLspReadBufferBytes) break;  // runaway server
        continue;
      }
      const auto& resp = *resp_opt;
      // Accept the id echoed as a JSON float (e.g. 1.0) as well as an integer:
      // some servers round-trip our integer id through a float. AsInt() truncates
      // 1.0 -> 1. Mirrors the steady-state gate in WorkspaceLspClientDispatch.cpp;
      // without it a float-echoing server's initialize response is never matched
      // and startup times out (the server is then killed — no LSP for that language).
      const bool is_init_response =
          resp.HasKey("id") && !resp.HasKey("method") &&
          (resp["id"].IsInt() || resp["id"].IsDouble()) && resp["id"].AsInt() == init_id;
      if (!is_init_response) {
        // Not the initialize response. A server may push notifications
        // (window/logMessage, $/progress) or requests
        // (window/workDoneProgress/create) before or around the initialize
        // response. `framer_.Next()` has already consumed this frame's bytes, so
        // a bare `continue` would silently drop it — losing early logs/progress
        // and leaving a server request unanswered (a request-blocking server then
        // hangs forever). Dispatch it instead; any reply queues in
        // deferred_messages and flushes once `initialized` is sent. Mirrors the
        // DAP client's pre-initialize dispatch (WorkspaceDapClientInternal.h).
        DispatchMessage(std::move(*resp_opt));
        continue;
      }
      // The server answered, with an error: say what it said. Read as "missing",
      // this reported "exited before initialize response" for a server that
      // explained itself (typescript-language-server on a TypeScript with no
      // tsserver.js), and the status tooltip -- the one place the reason shows --
      // sent the user looking for a crash.
      if (!resp.HasKey("result") && resp.HasKey("error")) {
        const std::string message = resp["error"]["message"].AsString();
        init_error = message.empty() ? std::string("no message") : message;
        break;
      }
      if (resp.HasKey("result")) {
        const auto& result = resp["result"];
        if (result.HasKey("capabilities")) {
          ApplyServerCapabilities(result["capabilities"]);
        }
        got_init = true;
      }
      break;
    }
  }
  TraceLifecycle("initialize-response", got_init ? "received" : "missing");

  if (!got_init) {
    (void)proc.IsRunning();
    const std::optional<int> exit_code = proc.exit_code();
    if (!init_error.empty()) {
      SetLastError("language server refused to initialize: " + init_error);
    } else if (exit_code.has_value()) {
      SetLastError("language server exited before initialize response (exit code " +
                   std::to_string(*exit_code) + ")");
    } else {
      SetLastError("timed out waiting for initialize response from language server");
    }
    AbandonInitialize();
    return;
  }

  bool initialized_send_failed = false;
  {
    std::lock_guard lock(send_mutex);
    // No I/O thread is running yet, so this is the only writer — write directly.
    bool sent_initialized = false;
    {
      std::lock_guard wlock(write_mutex);
      sent_initialized = proc.Write(
          SerializeMessage(MakeNotification("initialized", JsonValue(JsonObject{}))));
    }
    if (!sent_initialized) {
      SetLastError("failed to send initialized notification to language server");
      deferred_messages.clear();
      // Pre-init, all queued messages live in deferred_messages (outbound is empty),
      // so dropping them releases the whole charged byte budget. (TD-2026-07-17A-071)
      outbound_queued_bytes_ = 0;
      initialized_send_failed = true;
    } else {
      initialized.store(true, std::memory_order_release);
      // Push configuration before any queued didOpen so servers that read
      // settings (clangd, OmniSharp/Roslyn) see them before touching documents.
      if (settings.IsObject()) {
        JsonObject config_params;
        config_params["settings"] = settings;
        outbound_messages.push_back(QueuedMessage{
            .serialized = SerializeMessage(MakeNotification(
                "workspace/didChangeConfiguration", JsonValue(std::move(config_params)))),
            .build_serialized = {},
        });
      }
      for (QueuedMessage& message : deferred_messages) {
        outbound_messages.push_back(std::move(message));
      }
      deferred_messages.clear();
    }
  }

  if (initialized_send_failed) {
    // send_mutex is released above; FailPendingRequests takes the protocol mutex,
    // so failing pending requests here avoids nesting send_mutex -> mutex.
    AbandonInitialize();
    return;
  }

  OpenWakePipe();
  stop_io.store(false, std::memory_order_release);
  io_thread = std::thread([this]() {
    util::NameCurrentThread("lsp-io");
    IoMain();
  });
  Wake();  // flush the queued config/didOpen without waiting for the first poll

  bool is_indexing = false;
  {
    std::lock_guard lock(mutex);
    if (readiness_snapshot.state != LspClient::ReadinessSnapshot::State::Indexing) {
      readiness_snapshot.state = LspClient::ReadinessSnapshot::State::Ready;
      readiness_snapshot.message.clear();
      readiness_snapshot.indexed_count = 0;
    }
    is_indexing = readiness_snapshot.state == LspClient::ReadinessSnapshot::State::Indexing;
  }
  initializing.store(false, std::memory_order_release);
  TraceLifecycle("initialized", is_indexing ? "indexing" : "ready");
}

void LspClient::Impl::DoShutdown() {
  TraceLifecycle("shutdown-begin");
  shutting_down.store(true, std::memory_order_release);
  stop_init.store(true);
  if (test_stub_mode.load(std::memory_order_acquire)) {
    ClearDeferredMessages();
    {
      std::lock_guard hook_lock(mutex);
      test_handlers.Reset();
      apply_edit_handler = nullptr;
    }
    test_stub_mode.store(false, std::memory_order_release);
    MarkSessionEnded();
    main_mailbox.PushWake();
    return;
  }
  if (!initialized.load(std::memory_order_acquire)) {
    CancelBeforeInitialized();
    main_mailbox.PushWake();
    return;
  }

  if (init_thread.joinable()) {
    init_thread.join();
  }
  // Keep the I/O thread running through the handshake below so it can receive
  // the shutdown response; its writes are serialized with ours via write_mutex.
  ClearDeferredMessages();

  using namespace util;
  int shutdown_id = 0;
  {
    std::lock_guard lock(mutex);
    shutdown_response_received = false;
    shutdown_request_id = next_id++;
    shutdown_id = shutdown_request_id;
  }
  // Bounded lock acquisition: if the io_thread is stuck in a proc.Write to a
  // wedged-but-alive server (holding write_mutex), this returns false quickly
  // rather than blocking teardown forever, and we fall through to the force-kill
  // below — which unblocks that write so io_thread can be joined.
  const bool sent_shutdown =
      SendMessageImmediate(MakeRequest(shutdown_id, "shutdown", JsonValue(JsonObject{})), true,
                           std::chrono::milliseconds(1000));
  TraceLifecycle("shutdown-request", sent_shutdown ? "sent" : "send-failed");
  if (sent_shutdown) {
    std::unique_lock lock(mutex);
    const bool got_shutdown_response =
        shutdown_cv.wait_for(lock, std::chrono::milliseconds(750), [this]() {
      return shutdown_response_received;
    });
    TraceLifecycle("shutdown-response", got_shutdown_response ? "received" : "timeout");
  }
  if (proc.IsRunning()) {
    // Also bounded: never block teardown behind a stuck write. If it times out,
    // CloseStdin + the force-kill below still tear the server down.
    (void)SendMessageImmediate(MakeNotification("exit", JsonValue(JsonObject{})), true,
                               std::chrono::milliseconds(1000));
    TraceLifecycle("exit-notification", "sent");
    proc.CloseStdin();
    TraceLifecycle("stdin", "closed");
  }
  StopProcessAfterGoodbye(std::chrono::milliseconds(3000));
  MarkSessionEnded();
  const std::optional<int> code = proc.exit_code();
  if (code.has_value()) {
    const std::string detail = "exit-code=" + std::to_string(*code);
    TraceLspLifecycle(language_id, -1, "shutdown-complete", detail);
  } else {
    TraceLspLifecycle(language_id, -1, "shutdown-complete");
  }
  main_mailbox.PushWake();
}

}  // namespace microide::workspace
