// LspClient::Impl lifecycle: the blocking initialize handshake (capability
// advertisement + response parsing), graceful shutdown, and the progress-driven
// readiness state machine. Split out of WorkspaceLspClientInternal.h — these are
// the bulk of the former 1332-line transport header.
#include "workspace/WorkspaceLspClientInternal.h"

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "util/StartupTrace.h"

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
      readiness_snapshot.message =
          initialized.load(std::memory_order_acquire) ? "Ready" : "Starting...";
      readiness_snapshot.indexed_count = 0;
    } else {
      readiness_snapshot.state = LspClient::ReadinessSnapshot::State::Indexing;
      readiness_snapshot.message = !message.empty() ? message
                                 : !title.empty()   ? title
                                                    : "Indexing...";
      readiness_snapshot.indexed_count = std::max({ExtractIndexedCount(message),
                                                   ExtractIndexedCount(title), percentage});
    }
  }
  TraceLspLifecycle(language_id, proc.pid(), "progress", kind.empty() ? "report" : kind);
}

void LspClient::Impl::DoInitializeBlocking() {
  util::StartupTrace::Scope trace_scope("LspClient::DoInitializeBlocking");
  TraceLspLifecycle(language_id, proc.pid(), "init-thread-begin");
  initializing.store(true, std::memory_order_release);
  {
    std::lock_guard lock(mutex);
    readiness_snapshot.state = LspClient::ReadinessSnapshot::State::Starting;
    readiness_snapshot.message = "Starting...";
    readiness_snapshot.indexed_count = 0;
  }

  using namespace util;
  JsonObject text_doc_sync;
  text_doc_sync["dynamicRegistration"] = JsonValue(false);
  text_doc_sync["willSave"] = JsonValue(false);
  text_doc_sync["didSave"] = JsonValue(true);

  JsonObject completion_item_caps;
  completion_item_caps["snippetSupport"] = JsonValue(true);
  {
    JsonArray doc_formats;
    doc_formats.push_back(JsonValue("markdown"));
    doc_formats.push_back(JsonValue("plaintext"));
    completion_item_caps["documentationFormat"] = JsonValue(std::move(doc_formats));
  }
  JsonObject completion_caps;
  completion_caps["dynamicRegistration"] = JsonValue(false);
  completion_caps["completionItem"] = JsonValue(std::move(completion_item_caps));

  JsonObject hover_caps;
  hover_caps["dynamicRegistration"] = JsonValue(false);
  {
    JsonArray hover_formats;
    hover_formats.push_back(JsonValue("markdown"));
    hover_formats.push_back(JsonValue("plaintext"));
    hover_caps["contentFormat"] = JsonValue(std::move(hover_formats));
  }

  JsonObject signature_caps;
  signature_caps["dynamicRegistration"] = JsonValue(false);
  {
    JsonObject parameter_info;
    // We resolve `[start, end]` parameter-label offsets against the signature label.
    parameter_info["labelOffsetSupport"] = JsonValue(true);
    JsonObject signature_info;
    signature_info["parameterInformation"] = JsonValue(std::move(parameter_info));
    // Per-signature activeParameter overrides the top-level value (LSP 3.16+).
    signature_info["activeParameterSupport"] = JsonValue(true);
    JsonArray doc_formats;
    doc_formats.push_back(JsonValue("markdown"));
    doc_formats.push_back(JsonValue("plaintext"));
    signature_info["documentationFormat"] = JsonValue(std::move(doc_formats));
    signature_caps["signatureInformation"] = JsonValue(std::move(signature_info));
  }

  JsonObject definition_caps;
  definition_caps["dynamicRegistration"] = JsonValue(false);
  definition_caps["linkSupport"] = JsonValue(true);

  // typeDefinition / implementation / declaration share definition's link support.
  const auto make_nav_caps = []() {
    JsonObject caps;
    caps["dynamicRegistration"] = JsonValue(false);
    caps["linkSupport"] = JsonValue(true);
    return caps;
  };

  JsonObject references_caps;
  references_caps["dynamicRegistration"] = JsonValue(false);

  JsonObject rename_caps;
  rename_caps["dynamicRegistration"] = JsonValue(false);
  rename_caps["prepareSupport"] = JsonValue(true);

  JsonObject code_action_caps;
  code_action_caps["dynamicRegistration"] = JsonValue(false);
  {
    JsonArray kinds;
    for (const char* kind : {"quickfix", "refactor", "refactor.extract", "refactor.inline",
                             "refactor.rewrite", "source", "source.organizeImports"}) {
      kinds.push_back(JsonValue(kind));
    }
    JsonObject kind_obj;
    kind_obj["valueSet"] = JsonValue(std::move(kinds));
    JsonObject literal;
    literal["codeActionKind"] = JsonValue(std::move(kind_obj));
    code_action_caps["codeActionLiteralSupport"] = JsonValue(std::move(literal));
  }

  JsonObject formatting_caps;
  formatting_caps["dynamicRegistration"] = JsonValue(false);

  JsonObject range_formatting_caps;
  range_formatting_caps["dynamicRegistration"] = JsonValue(false);

  JsonObject document_symbol_caps;
  document_symbol_caps["dynamicRegistration"] = JsonValue(false);

  JsonObject semantic_tokens_caps;
  semantic_tokens_caps["dynamicRegistration"] = JsonValue(false);
  {
    JsonObject requests;
    requests["full"] = JsonValue(true);
    semantic_tokens_caps["requests"] = JsonValue(std::move(requests));
    JsonArray formats;
    formats.push_back(JsonValue("relative"));
    semantic_tokens_caps["formats"] = JsonValue(std::move(formats));
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
  text_document_caps["definition"] = JsonValue(std::move(definition_caps));
  text_document_caps["typeDefinition"] = JsonValue(make_nav_caps());
  text_document_caps["implementation"] = JsonValue(make_nav_caps());
  text_document_caps["declaration"] = JsonValue(make_nav_caps());
  text_document_caps["references"] = JsonValue(std::move(references_caps));
  text_document_caps["rename"] = JsonValue(std::move(rename_caps));
  text_document_caps["codeAction"] = JsonValue(std::move(code_action_caps));
  text_document_caps["formatting"] = JsonValue(std::move(formatting_caps));
  text_document_caps["rangeFormatting"] = JsonValue(std::move(range_formatting_caps));
  text_document_caps["documentSymbol"] = JsonValue(std::move(document_symbol_caps));
  text_document_caps["semanticTokens"] = JsonValue(std::move(semantic_tokens_caps));

  JsonObject workspace_caps;
  workspace_caps["configuration"] = JsonValue(true);
  workspace_caps["workspaceFolders"] = JsonValue(true);
  // Server-initiated edits are applied by the host (open buffers in place, closed
  // files silently on disk) via the bound apply-edit handler.
  workspace_caps["applyEdit"] = JsonValue(true);
  {
    JsonObject did_change_config;
    did_change_config["dynamicRegistration"] = JsonValue(false);
    workspace_caps["didChangeConfiguration"] = JsonValue(std::move(did_change_config));
  }

  JsonObject window_caps;
  window_caps["workDoneProgress"] = JsonValue(true);

  // The editor measures columns in UTF-8 bytes, which is exactly LSP's UTF-8
  // position encoding. Advertise it first (then UTF-16 as the mandatory
  // fallback) so a conformant server emits/consumes byte offsets and positions
  // past non-ASCII characters stay correct. Without this the server assumes the
  // UTF-16 default and our byte offsets corrupt every position past any
  // non-ASCII char on the incremental-sync mirror.
  JsonObject general_caps;
  {
    JsonArray encodings;
    encodings.push_back(JsonValue("utf-8"));
    encodings.push_back(JsonValue("utf-16"));
    general_caps["positionEncodings"] = JsonValue(std::move(encodings));
  }

  JsonObject caps;
  caps["textDocument"] = JsonValue(std::move(text_document_caps));
  caps["workspace"] = JsonValue(std::move(workspace_caps));
  caps["window"] = JsonValue(std::move(window_caps));
  caps["general"] = JsonValue(std::move(general_caps));

  JsonObject client_info;
  client_info["name"] = JsonValue("microide");

  JsonObject init_params;
#if defined(__unix__) || defined(__APPLE__)
  init_params["processId"] = JsonValue(static_cast<std::int64_t>(::getpid()));
#else
  init_params["processId"] = JsonValue(static_cast<std::int64_t>(0));
#endif
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
  init_params["capabilities"] = JsonValue(std::move(caps));

  const int init_id = [&]() {
    std::lock_guard lock(mutex);
    return next_id++;
  }();
  const auto req = MakeRequest(init_id, "initialize", JsonValue(std::move(init_params)));
  if (!SendMessageImmediate(req)) {
    SetLastError("failed to send initialize request to language server");
    ShutdownProcessOnce();
    ClearDeferredMessages();
    initializing.store(false, std::memory_order_release);
    return;
  }
  TraceLspLifecycle(language_id, proc.pid(), "initialize-request", "sent");

  bool got_init = false;
  {
    util::StartupTrace::Scope wait_init_scope("LspClient::DoInitializeBlocking::WaitInitializeResponse");
    for (int attempts = 0; attempts < 60; ++attempts) {
      if (stop_init.load(std::memory_order_acquire)) {
        ShutdownProcessOnce();
        ClearDeferredMessages();
        initializing.store(false, std::memory_order_release);
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
      if (!resp.HasKey("id") || resp["id"].AsInt() != init_id) continue;

      if (resp.HasKey("result")) {
        const auto& result = resp["result"];
        if (result.HasKey("capabilities")) {
          const auto& server_caps = result["capabilities"];
          const auto& sync = server_caps["textDocumentSync"];
          int sync_kind = 1;
          if (sync.IsInt()) {
            sync_kind = sync.AsInt();
          } else if (sync.HasKey("change")) {
            sync_kind = sync["change"].AsInt(1);
          }
          supports_incremental_sync.store(sync_kind == 2, std::memory_order_release);

          // Capture the negotiated position encoding. utf-8 means our editor
          // byte offsets are already exact; utf-16/utf-32 are converted per line
          // at every position seam (lsp_encoding::*), and the per-keystroke
          // incremental sync falls back to full-document sync for those so the
          // pre-edit byte range never needs re-encoding.
          {
            std::string negotiated = "utf-16";
            if (server_caps.HasKey("positionEncoding") &&
                server_caps["positionEncoding"].IsString()) {
              negotiated = server_caps["positionEncoding"].AsString();
            }
            {
              std::lock_guard lock(mutex);
              position_encoding = negotiated;
            }
          }

          // Capture the semantic-token legend (type names) so the host can map
          // a response's token-type indices back to names at publish time.
          if (server_caps.HasKey("semanticTokensProvider")) {
            const auto& provider = server_caps["semanticTokensProvider"];
            if (provider.HasKey("legend") && provider["legend"].HasKey("tokenTypes")) {
              const auto& types = provider["legend"]["tokenTypes"];
              if (types.IsArray()) {
                const auto& array = types.AsArray();
                std::vector<std::string> legend;
                legend.reserve(array.size());
                for (const auto& entry : array) {
                  legend.push_back(entry.AsString());
                }
                std::lock_guard lock(mutex);
                semantic_token_types = std::move(legend);
              }
              supports_semantic_tokens.store(true, std::memory_order_release);
            }
          }

          // renameProvider may be a bare bool (plain rename) or an object that can
          // carry prepareProvider (textDocument/prepareRename support).
          if (server_caps.HasKey("renameProvider")) {
            const auto& rename_provider = server_caps["renameProvider"];
            if (rename_provider.HasKey("prepareProvider")) {
              supports_prepare_rename.store(
                  rename_provider["prepareProvider"].AsBool(false), std::memory_order_release);
            }
          }
        }
        got_init = true;
      }
      break;
    }
  }
  TraceLspLifecycle(language_id, proc.pid(), "initialize-response",
                    got_init ? "received" : "missing");

  if (!got_init) {
    (void)proc.IsRunning();
    const std::optional<int> exit_code = proc.exit_code();
    if (exit_code.has_value()) {
      SetLastError("language server exited before initialize response (exit code " +
                   std::to_string(*exit_code) + ")");
    } else {
      SetLastError("timed out waiting for initialize response from language server");
    }
    ShutdownProcessOnce();
    ClearDeferredMessages();
    initializing.store(false, std::memory_order_release);
    return;
  }

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
      ShutdownProcessOnce();
      initializing.store(false, std::memory_order_release);
      return;
    }
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

  OpenWakePipe();
  stop_io.store(false, std::memory_order_release);
  io_thread = std::thread([this]() { IoMain(); });
  Wake();  // flush the queued config/didOpen without waiting for the first poll

  bool is_indexing = false;
  {
    std::lock_guard lock(mutex);
    if (readiness_snapshot.state != LspClient::ReadinessSnapshot::State::Indexing) {
      readiness_snapshot.state = LspClient::ReadinessSnapshot::State::Ready;
      readiness_snapshot.message = "Ready";
      readiness_snapshot.indexed_count = 0;
    }
    is_indexing = readiness_snapshot.state == LspClient::ReadinessSnapshot::State::Indexing;
  }
  initializing.store(false, std::memory_order_release);
  TraceLspLifecycle(language_id, proc.pid(), "initialized", is_indexing ? "indexing" : "ready");
}

void LspClient::Impl::DoShutdown() {
  TraceLspLifecycle(language_id, proc.pid(), "shutdown-begin");
  shutting_down.store(true, std::memory_order_release);
  stop_init.store(true);
  if (test_stub_mode.load(std::memory_order_acquire)) {
    ClearDeferredMessages();
    ResetProtocolState();
    {
      std::lock_guard hook_lock(mutex);
      test_document_symbol_handler = nullptr;
      test_hover_handler = nullptr;
      test_formatting_handler = nullptr;
      test_rename_handler = nullptr;
      test_completion_handler = nullptr;
      test_signature_help_handler = nullptr;
      test_prepare_rename_handler = nullptr;
      apply_edit_handler = nullptr;
    }
    initialized.store(false, std::memory_order_release);
    initializing.store(false, std::memory_order_release);
    supports_incremental_sync.store(false, std::memory_order_release);
    test_stub_mode.store(false, std::memory_order_release);
    shutting_down.store(false, std::memory_order_release);
    shutdown_complete.store(true, std::memory_order_release);
    main_mailbox.PushWake();
    return;
  }
  if (!initialized.load(std::memory_order_acquire)) {
    TraceLspLifecycle(language_id, proc.pid(), "preinit-cancel");
    stop_io.store(true, std::memory_order_release);
    Wake();
    ShutdownProcessOnce(0);
    if (init_thread.joinable()) {
      init_thread.join();
    }
    // The init thread may have just promoted itself to the running state and
    // launched the I/O thread between our check and here; join it if so.
    if (io_thread.joinable()) {
      io_thread.join();
    }
    ClearDeferredMessages();
    CloseWakePipe();
    ResetProtocolState();
    initialized.store(false, std::memory_order_release);
    initializing.store(false, std::memory_order_release);
    supports_incremental_sync.store(false, std::memory_order_release);
    shutting_down.store(false, std::memory_order_release);
    shutdown_complete.store(true, std::memory_order_release);
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
  TraceLspLifecycle(language_id, proc.pid(), "shutdown-request",
                    sent_shutdown ? "sent" : "send-failed");
  if (sent_shutdown) {
    std::unique_lock lock(mutex);
    const bool got_shutdown_response =
        shutdown_cv.wait_for(lock, std::chrono::milliseconds(750), [this]() {
      return shutdown_response_received;
    });
    TraceLspLifecycle(language_id, proc.pid(), "shutdown-response",
                      got_shutdown_response ? "received" : "timeout");
  }
  if (proc.IsRunning()) {
    // Also bounded: never block teardown behind a stuck write. If it times out,
    // CloseStdin + the force-kill below still tear the server down.
    (void)SendMessageImmediate(MakeNotification("exit", JsonValue(JsonObject{})), true,
                               std::chrono::milliseconds(1000));
    TraceLspLifecycle(language_id, proc.pid(), "exit-notification", "sent");
    proc.CloseStdin();
    TraceLspLifecycle(language_id, proc.pid(), "stdin", "closed");
  }

  const auto graceful_shutdown_deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
  while (proc.IsRunning() && std::chrono::steady_clock::now() < graceful_shutdown_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  TraceLspLifecycle(language_id, proc.pid(), "graceful-wait",
                    proc.IsRunning() ? "still-running" : "exited");

  stop_io.store(true, std::memory_order_release);
  Wake();
  if (proc.IsRunning()) {
    TraceLspLifecycle(language_id, proc.pid(), "forced-shutdown");
    ShutdownProcessOnce(1000);
  }

  if (io_thread.joinable()) {
    io_thread.join();
  }
  CloseWakePipe();

  ResetProtocolState();
  initialized.store(false, std::memory_order_release);
  initializing.store(false, std::memory_order_release);
  supports_incremental_sync.store(false, std::memory_order_release);
  shutting_down.store(false, std::memory_order_release);
  shutdown_complete.store(true, std::memory_order_release);
  const std::optional<int> code = proc.exit_code();
  if (code.has_value()) {
    const std::string detail = "exit-code=" + std::to_string(*code);
    TraceLspLifecycle(language_id, -1, "shutdown-complete", detail);
  } else {
    TraceLspLifecycle(language_id, -1, "shutdown-complete");
  }
  main_mailbox.PushWake();
}

void LspClient::Impl::BeginShutdown() {
  bool expected = false;
  if (!shutdown_started.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    return;
  }
  shutdown_complete.store(false, std::memory_order_release);
  shutdown_thread = std::thread([this]() { DoShutdown(); });
}

void LspClient::Impl::WaitForShutdown() {
  BeginShutdown();
  if (shutdown_thread.joinable()) {
    shutdown_thread.join();
  }
}

}  // namespace microide::workspace
