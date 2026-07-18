#include "workspace/WorkspaceLspClient.h"

#include "workspace/WorkspaceLspClientInternal.h"

namespace microide::workspace {

LspClient::LspClient() : impl_(new Impl{}) {}

LspClient::~LspClient() {
  Shutdown();
  delete impl_;
}

void LspClient::SetWakeEventType(Uint32 event_type) {
  impl_->main_mailbox.SetWakeEventType(event_type);
}

bool LspClient::Start(const std::vector<std::string>& command, const std::string& root_uri,
                      const std::string& language_id, const std::string& cwd,
                      const util::JsonValue& initialization_options,
                      const util::JsonValue& settings,
                      const platform::SubprocessSandbox& sandbox) {
  util::StartupTrace::Scope trace_scope("LspClient::Start");
  TraceLspLifecycle(language_id, -1, "start-request", command.empty() ? "" : command.front());
  impl_->last_error.clear();
  impl_->initialization_options = initialization_options;
  impl_->settings = settings;
  {
    std::lock_guard lock(impl_->mutex);
    impl_->readiness_snapshot.state = ReadinessSnapshot::State::Starting;
    impl_->readiness_snapshot.message = "Starting...";
    impl_->readiness_snapshot.indexed_count = 0;
  }

  {
    util::StartupTrace::Scope start_proc_scope("LspClient::Start::StartProcess");
    if (!impl_->proc.Start(command, cwd, sandbox)) {
      impl_->last_error = "failed to start language server process";
      {
        std::lock_guard lock(impl_->mutex);
        impl_->readiness_snapshot.state = ReadinessSnapshot::State::Failed;
        impl_->readiness_snapshot.message = impl_->last_error;
        impl_->readiness_snapshot.indexed_count = 0;
      }
      return false;
    }
  }
  TraceLspLifecycle(language_id, impl_->proc.pid(), "process-started");

  impl_->root_uri = root_uri;
  impl_->language_id = language_id;
  impl_->shutdown_started.store(false, std::memory_order_release);
  impl_->shutdown_complete.store(false, std::memory_order_release);
  impl_->shutting_down.store(false, std::memory_order_release);
  impl_->process_shutdown_started.store(false, std::memory_order_release);

  impl_->stop_init.store(false);
  Impl* const init_impl = impl_;
  impl_->init_thread = std::thread([init_impl]() { init_impl->DoInitializeBlocking(); });
  return true;
}

bool LspClient::IsRunning() const {
  if (impl_->test_stub_mode.load(std::memory_order_acquire)) {
    return true;
  }
  return impl_->proc.IsRunning();
}

bool LspClient::IsInitializing() const {
  return impl_->initializing.load(std::memory_order_acquire);
}

bool LspClient::IsInitialized() const {
  return impl_->initialized.load(std::memory_order_acquire);
}

bool LspClient::SupportsIncrementalSync() const {
  return impl_->supports_incremental_sync.load(std::memory_order_acquire);
}

std::string LspClient::ServerPositionEncoding() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->position_encoding;
}

bool LspClient::HasOpenDocument(const std::string& uri) const {
  std::lock_guard lock(impl_->mutex);
  return impl_->document_versions.contains(uri);
}

const std::string& LspClient::LastError() const {
  std::lock_guard lock(impl_->mutex);
  impl_->last_error_snapshot = impl_->last_error;
  return impl_->last_error_snapshot;
}

LspClient::ReadinessSnapshot LspClient::GetReadinessSnapshot() const {
  std::lock_guard lock(impl_->mutex);
  LspClient::ReadinessSnapshot snapshot = impl_->readiness_snapshot;
  if (!impl_->last_error.empty()) {
    snapshot.state = LspClient::ReadinessSnapshot::State::Failed;
    snapshot.message = impl_->last_error;
    snapshot.indexed_count = 0;
    return snapshot;
  }
  if (snapshot.state == LspClient::ReadinessSnapshot::State::Indexing) {
    if (snapshot.message.empty()) {
      snapshot.message = "Indexing...";
    }
    return snapshot;
  }
  if (impl_->initializing.load(std::memory_order_acquire)) {
    snapshot.state = LspClient::ReadinessSnapshot::State::Starting;
    if (snapshot.message.empty()) {
      snapshot.message = "Starting...";
    }
    snapshot.indexed_count = 0;
    return snapshot;
  }
  if (impl_->initialized.load(std::memory_order_acquire)) {
    snapshot.state = LspClient::ReadinessSnapshot::State::Ready;
    if (snapshot.message.empty()) {
      snapshot.message = "Ready";
    }
    snapshot.indexed_count = 0;
    return snapshot;
  }
  if (impl_->proc.IsRunning()) {
    snapshot.state = LspClient::ReadinessSnapshot::State::Starting;
    if (snapshot.message.empty()) {
      snapshot.message = "Starting...";
    }
    snapshot.indexed_count = 0;
    return snapshot;
  }
  if (!snapshot.message.empty()) {
    return snapshot;
  }
  snapshot.state = LspClient::ReadinessSnapshot::State::Idle;
  snapshot.message = "Idle";
  snapshot.indexed_count = 0;
  return snapshot;
}

void LspClient::SetDiagnosticsCallback(OnPublishDiagnostics callback) {
  std::lock_guard lock(impl_->mutex);
  impl_->diagnostics_callback = std::move(callback);
}

bool LspClient::HasDiagnosticsCallback() const {
  std::lock_guard lock(impl_->mutex);
  return static_cast<bool>(impl_->diagnostics_callback);
}

void LspClient::DrainCallbacks() {
  util::StartupTrace::Scope trace_scope("LspClient::DrainCallbacks");
  impl_->main_mailbox.Drain();
}

bool LspClient::DidOpen(std::string uri, std::string language_id, std::string text) {
  TraceLspLifecycle(language_id, impl_->proc.pid(), "didOpen", uri);
  // Commit the open state (version 1) only after a successful enqueue, matching
  // DidChange. Committing before the enqueue (the old operator[] = 1) would make
  // the host believe the document is open even when the queue was full / the I/O
  // thread stopped / shutdown rejected the frame, so later version-gated
  // diagnostics would be checked against a document the server never opened.
  // `uri` is captured by value so it stays valid for the post-enqueue commit.
  const std::size_t approx_bytes = text.size();
  const bool queued = impl_->SendMessageBuilderAfterInitialize(
      [impl = impl_, uri, language_id = std::move(language_id),
       text = std::move(text)]() mutable {
        using namespace util;
        JsonObject text_doc;
        text_doc["uri"] = JsonValue(uri);
        text_doc["languageId"] = JsonValue(language_id);
        text_doc["version"] = JsonValue(static_cast<std::int64_t>(1));
        text_doc["text"] = JsonValue(text);
        JsonObject params;
        params["textDocument"] = JsonValue(std::move(text_doc));
        return impl->SerializeMessage(
            impl->MakeNotification("textDocument/didOpen", JsonValue(std::move(params))));
      },
      approx_bytes);
  if (queued) {
    std::lock_guard lock(impl_->mutex);
    impl_->document_versions[uri] = 1;
  }
  return queued;
}

bool LspClient::DidChange(const std::string& uri, const std::string& text) {
  int version = 0;
  {
    std::lock_guard lock(impl_->mutex);
    // Require an open document. Using operator[] here would insert a phantom
    // version entry for a URI that was never opened (or was already closed),
    // making the client version-gate future diagnostics for a document the
    // server does not consider open.
    const auto it = impl_->document_versions.find(uri);
    if (it == impl_->document_versions.end()) {
      return false;
    }
    // Compute the NEXT version but do not commit it yet: if the enqueue below fails
    // (queue at capacity / stopped), the server never receives this version, so
    // advancing document_versions[uri] would make the staleness gate drop every
    // later publishDiagnostics (the server reports against the last version it saw).
    version = it->second + 1;
  }
  // Defer to the I/O thread: the whole-document copy into the JsonValue and the
  // serialization (both proportional to document size) run off the UI thread.
  // `text` must be captured by value — the caller owns the buffer only for this call.
  const bool queued = impl_->SendMessageBuilderAfterInitialize(
      [impl = impl_, uri, text, version]() mutable {
        using namespace util;
        JsonObject text_doc;
        text_doc["uri"] = JsonValue(uri);
        text_doc["version"] = JsonValue(static_cast<std::int64_t>(version));
        JsonObject change;
        change["text"] = JsonValue(std::move(text));
        JsonArray changes;
        changes.push_back(JsonValue(std::move(change)));
        JsonObject params;
        params["textDocument"] = JsonValue(std::move(text_doc));
        params["contentChanges"] = JsonValue(std::move(changes));
        return impl->SerializeMessage(
            impl->MakeNotification("textDocument/didChange", JsonValue(std::move(params))));
      },
      text.size());
  if (queued) {
    std::lock_guard lock(impl_->mutex);
    impl_->document_versions[uri] = std::max(impl_->document_versions[uri], version);
  }
  return queued;
}

bool LspClient::DidChangeIncremental(const std::string& uri,
                                     Range changed_range,
                                     const std::string& new_text) {
  if (!impl_->supports_incremental_sync) {
    return false;
  }
  int version = 0;
  {
    std::lock_guard lock(impl_->mutex);
    // See DidChange: require an open document (no phantom version entry) and
    // commit the next version only on a successful enqueue.
    const auto it = impl_->document_versions.find(uri);
    if (it == impl_->document_versions.end()) {
      return false;
    }
    version = it->second + 1;
  }
  const bool queued = impl_->SendMessageBuilderAfterInitialize(
      [impl = impl_, uri, changed_range, new_text, version]() mutable {
        using namespace util;
        JsonObject text_doc;
        text_doc["uri"] = JsonValue(uri);
        text_doc["version"] = JsonValue(static_cast<std::int64_t>(version));
        JsonObject change;
        change["range"] = lsp_protocol::MakeRange(changed_range);
        change["text"] = JsonValue(std::move(new_text));
        JsonArray changes;
        changes.push_back(JsonValue(std::move(change)));
        JsonObject params;
        params["textDocument"] = JsonValue(std::move(text_doc));
        params["contentChanges"] = JsonValue(std::move(changes));
        return impl->SerializeMessage(
            impl->MakeNotification("textDocument/didChange", JsonValue(std::move(params))));
      },
      new_text.size());
  if (queued) {
    std::lock_guard lock(impl_->mutex);
    impl_->document_versions[uri] = std::max(impl_->document_versions[uri], version);
  }
  return queued;
}

bool LspClient::DidSave(const std::string& uri) {
  using namespace util;
  // Only notify for an open document. A save for an unopened/closed URI can make
  // strict servers report a protocol error or double-run file watchers.
  {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->document_versions.contains(uri)) {
      return false;
    }
  }
  JsonObject text_doc;
  text_doc["uri"] = JsonValue(uri);
  JsonObject params;
  params["textDocument"] = JsonValue(std::move(text_doc));
  return impl_->SendMessageAfterInitialize(
      impl_->MakeNotification("textDocument/didSave", JsonValue(std::move(params))));
}

bool LspClient::DidClose(const std::string& uri) {
  using namespace util;
  JsonObject text_doc;
  text_doc["uri"] = JsonValue(uri);
  JsonObject params;
  params["textDocument"] = JsonValue(std::move(text_doc));
  const bool queued = impl_->SendMessageAfterInitialize(
      impl_->MakeNotification("textDocument/didClose", JsonValue(std::move(params))));
  if (queued) {
    // Erase the open state only after the didClose notification is enqueued.
    // Erasing first (the old unconditional erase) would let the host treat the
    // document as closed while the server still has it open, so a later
    // didChange would be dropped by the missing-version guard and a re-open
    // could violate LSP ordering against a version the server never released.
    std::lock_guard lock(impl_->mutex);
    impl_->document_versions.erase(uri);
  }
  return queued;
}

void LspClient::BeginShutdown() {
  impl_->BeginShutdown();
}

void LspClient::Shutdown() {
  impl_->WaitForShutdown();
}

bool LspClient::IsShuttingDown() const {
  return impl_->shutting_down.load(std::memory_order_acquire);
}

bool LspClient::IsShutdownComplete() const {
  return impl_->shutdown_started.load(std::memory_order_acquire) &&
         impl_->shutdown_complete.load(std::memory_order_acquire);
}

void LspClient::SetMaxQueuedBytesForTesting(std::size_t bytes) {
  std::lock_guard lock(impl_->send_mutex);
  impl_->max_queued_bytes_ = bytes;
}

void LspClient::EnableTestStubMode() {
  std::lock_guard lock(impl_->mutex);
  impl_->test_stub_mode.store(true, std::memory_order_release);
  impl_->initialized.store(true, std::memory_order_release);
  impl_->initializing.store(false, std::memory_order_release);
  impl_->readiness_snapshot.state = ReadinessSnapshot::State::Ready;
  impl_->readiness_snapshot.message = "Ready";
  impl_->readiness_snapshot.indexed_count = 0;
}

void LspClient::DisableTestStubMode() {
  std::lock_guard lock(impl_->mutex);
  impl_->test_stub_mode.store(false, std::memory_order_release);
  impl_->test_handlers.document_symbol = nullptr;
}

void LspClient::SetTestDocumentSymbolHandler(
    std::function<void(std::string uri, DocumentSymbolCallback cb)> handler) {
  std::lock_guard lock(impl_->mutex);
  impl_->test_handlers.document_symbol = std::move(handler);
}

void LspClient::ClearTestDocumentSymbolHandler() {
  std::lock_guard lock(impl_->mutex);
  impl_->test_handlers.document_symbol = nullptr;
}

void LspClient::SetTestHoverHandler(std::function<void(std::string uri, HoverCallback cb)> handler) {
  std::lock_guard lock(impl_->mutex);
  impl_->test_handlers.hover = std::move(handler);
}

void LspClient::ClearTestHoverHandler() {
  std::lock_guard lock(impl_->mutex);
  impl_->test_handlers.hover = nullptr;
}

void LspClient::SetTestFormattingHandler(
    std::function<void(std::string uri, FormattingCallback cb)> handler) {
  std::lock_guard lock(impl_->mutex);
  impl_->test_handlers.formatting = std::move(handler);
}

void LspClient::ClearTestFormattingHandler() {
  std::lock_guard lock(impl_->mutex);
  impl_->test_handlers.formatting = nullptr;
}

void LspClient::SetTestRenameHandler(
    std::function<void(std::string uri, std::string new_name, RenameCallback cb)> handler) {
  std::lock_guard lock(impl_->mutex);
  impl_->test_handlers.rename = std::move(handler);
}

void LspClient::ClearTestRenameHandler() {
  std::lock_guard lock(impl_->mutex);
  impl_->test_handlers.rename = nullptr;
}

void LspClient::SetTestCompletionHandler(
    std::function<void(std::string uri, Position pos, CompletionCallback cb)> handler) {
  std::lock_guard lock(impl_->mutex);
  impl_->test_handlers.completion = std::move(handler);
}

void LspClient::ClearTestCompletionHandler() {
  std::lock_guard lock(impl_->mutex);
  impl_->test_handlers.completion = nullptr;
}

void LspClient::SetTestSignatureHelpHandler(
    std::function<void(std::string uri, Position pos, SignatureHelpCallback cb)> handler) {
  std::lock_guard lock(impl_->mutex);
  impl_->test_handlers.signature_help = std::move(handler);
}

void LspClient::ClearTestSignatureHelpHandler() {
  std::lock_guard lock(impl_->mutex);
  impl_->test_handlers.signature_help = nullptr;
}

void LspClient::SetApplyEditHandler(std::function<bool(WorkspaceEdit)> handler) {
  std::lock_guard lock(impl_->mutex);
  impl_->apply_edit_handler = std::move(handler);
}

void LspClient::SimulateServerRequestForTesting(const std::string& method, util::JsonValue params,
                                                util::JsonValue id) {
  impl_->HandleServerRequest(id, method, params);
}

bool LspClient::HasApplyEditHandler() const {
  std::lock_guard lock(impl_->mutex);
  return static_cast<bool>(impl_->apply_edit_handler);
}

std::vector<std::string> LspClient::SemanticTokenLegend() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->semantic_token_types;
}

bool LspClient::SupportsSemanticTokens() const {
  return impl_->supports_semantic_tokens.load(std::memory_order_acquire);
}

bool LspClient::SupportsPrepareRename() const {
  return impl_->supports_prepare_rename.load(std::memory_order_acquire);
}

bool LspClient::SupportsInlayHints() const {
  return impl_->supports_inlay_hints.load(std::memory_order_acquire);
}

void LspClient::SetTestInlayHintHandler(
    std::function<void(std::string uri, Range range, InlayHintCallback cb)> handler) {
  std::lock_guard lock(impl_->mutex);
  impl_->test_handlers.inlay_hint = std::move(handler);
  impl_->supports_inlay_hints.store(true, std::memory_order_release);
}

void LspClient::ClearTestInlayHintHandler() {
  std::lock_guard lock(impl_->mutex);
  impl_->test_handlers.inlay_hint = nullptr;
}

void LspClient::SetTestPrepareRenameHandler(
    std::function<void(std::string uri, Position pos, PrepareRenameCallback cb)> handler) {
  std::lock_guard lock(impl_->mutex);
  impl_->test_handlers.prepare_rename = std::move(handler);
  impl_->supports_prepare_rename.store(true, std::memory_order_release);
}

void LspClient::ClearTestPrepareRenameHandler() {
  std::lock_guard lock(impl_->mutex);
  impl_->test_handlers.prepare_rename = nullptr;
}

void LspClient::SetTestWorkspaceSymbolHandler(
    std::function<void(std::string query, WorkspaceSymbolCallback cb)> handler) {
  std::lock_guard lock(impl_->mutex);
  impl_->test_handlers.workspace_symbol = std::move(handler);
}

void LspClient::ClearTestWorkspaceSymbolHandler() {
  std::lock_guard lock(impl_->mutex);
  impl_->test_handlers.workspace_symbol = nullptr;
}

void LspClient::SetTestSemanticTokensHandler(
    std::function<void(std::string uri, SemanticTokensCallback cb)> handler) {
  std::lock_guard lock(impl_->mutex);
  impl_->test_handlers.semantic_tokens = std::move(handler);
  impl_->supports_semantic_tokens.store(true, std::memory_order_release);
}

void LspClient::SetTestSemanticTokenLegend(std::vector<std::string> legend) {
  std::lock_guard lock(impl_->mutex);
  impl_->semantic_token_types = std::move(legend);
  impl_->supports_semantic_tokens.store(true, std::memory_order_release);
}

}  // namespace microide::workspace
