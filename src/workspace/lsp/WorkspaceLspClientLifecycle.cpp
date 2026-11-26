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

JsonObject StaticRegistration() {
  JsonObject caps;
  caps["dynamicRegistration"] = JsonValue(false);
  return caps;
}

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
  completion_caps["contextSupport"] = JsonValue(true);

  JsonObject hover_caps = StaticRegistration();
  hover_caps["contentFormat"] = StringArray({"markdown", "plaintext"});

  JsonObject signature_caps = StaticRegistration();
  {
    JsonObject parameter_info;
    parameter_info["labelOffsetSupport"] = JsonValue(true);
    JsonObject signature_info;
    signature_info["parameterInformation"] = JsonValue(std::move(parameter_info));
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
  workspace_caps["executeCommand"] = JsonValue(StaticRegistration());
  workspace_caps["applyEdit"] = JsonValue(true);
  {
    JsonObject workspace_edit_caps;
    workspace_edit_caps["documentChanges"] = JsonValue(true);
    workspace_edit_caps["resourceOperations"] = StringArray({"create", "rename", "delete"});
    workspace_edit_caps["failureHandling"] = JsonValue("textOnlyTransactional");
    workspace_caps["workspaceEdit"] = JsonValue(std::move(workspace_edit_caps));
  }
  workspace_caps["didChangeConfiguration"] = JsonValue(StaticRegistration());
  {
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

bool ProviderAdvertised(const JsonValue& provider) {
  return provider.IsObject() || provider.AsBool(false);
}

}

void LspClient::Impl::ApplyServerCapabilities(const util::JsonValue& server_caps) {
  const auto& sync = server_caps["textDocumentSync"];
  int sync_kind = 1;
  if (sync.IsInt()) {
    sync_kind = sync.AsInt();
  } else if (sync.HasKey("change")) {
    sync_kind = sync["change"].AsInt(1);
  }
  supports_incremental_sync.store(sync_kind == 2, std::memory_order_release);

  completion_triggers.Store(server_caps["completionProvider"]["triggerCharacters"]);
  signature_help_triggers.Store(server_caps["signatureHelpProvider"]["triggerCharacters"]);

  {
    std::string negotiated = "utf-16";
    if (server_caps.HasKey("positionEncoding") && server_caps["positionEncoding"].IsString()) {
      negotiated = server_caps["positionEncoding"].AsString();
    }
    std::lock_guard lock(mutex);
    position_encoding = negotiated;
  }

  if (server_caps.HasKey("semanticTokensProvider")) {
    const auto& provider = server_caps["semanticTokensProvider"];
    if (provider.HasKey("legend") && provider["legend"].HasKey("tokenTypes")) {
      const auto& types = provider["legend"]["tokenTypes"];
      if (types.IsArray()) {
        const auto& array = types.AsArray();
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

  if (server_caps.HasKey("inlayHintProvider")) {
    supports_inlay_hints.store(ProviderAdvertised(server_caps["inlayHintProvider"]),
                               std::memory_order_release);
  }
  if (server_caps.HasKey("documentHighlightProvider")) {
    supports_document_highlight.store(
        ProviderAdvertised(server_caps["documentHighlightProvider"]), std::memory_order_release);
  }
  if (server_caps.HasKey("callHierarchyProvider")) {
    supports_call_hierarchy.store(ProviderAdvertised(server_caps["callHierarchyProvider"]),
                                  std::memory_order_release);
  }
  if (server_caps.HasKey("codeLensProvider")) {
    const auto& provider = server_caps["codeLensProvider"];
    const bool provided = ProviderAdvertised(provider);
    supports_code_lens.store(provided, std::memory_order_release);
    supports_code_lens_resolve.store(provided && provider["resolveProvider"].AsBool(false),
                                     std::memory_order_release);
  }
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
  std::string init_error;
  {
    util::StartupTrace::Scope wait_init_scope("LspClient::DoInitializeBlocking::WaitInitializeResponse");
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
        if (framer_.BufferedBytes() > kMaxLspReadBufferBytes) break;
        continue;
      }
      const auto& resp = *resp_opt;
      const bool is_init_response =
          resp.HasKey("id") && !resp.HasKey("method") &&
          (resp["id"].IsInt() || resp["id"].IsDouble()) && resp["id"].AsInt() == init_id;
      if (!is_init_response) {
        DispatchMessage(std::move(*resp_opt));
        continue;
      }
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
    std::lock_guard wlock(write_mutex);
    std::lock_guard lock(send_mutex);
    const bool sent_initialized =
        proc.Write(SerializeMessage(MakeNotification("initialized", JsonValue(JsonObject{}))));
    if (!sent_initialized) {
      SetLastError("failed to send initialized notification to language server");
      deferred_messages.clear();
      outbound_queued_bytes_ = 0;
      initialized_send_failed = true;
    } else {
      initialized.store(true, std::memory_order_release);
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
    AbandonInitialize();
    return;
  }

  OpenWakePipe();
  stop_io.store(false, std::memory_order_release);
  io_thread = std::thread([this]() {
    util::NameCurrentThread("lsp-io");
    IoMain();
  });
  Wake();

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
  ClearDeferredMessages();

  using namespace util;
  int shutdown_id = 0;
  {
    std::lock_guard lock(mutex);
    shutdown_response_received = false;
    shutdown_request_id = next_id++;
    shutdown_id = shutdown_request_id;
  }
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

}
