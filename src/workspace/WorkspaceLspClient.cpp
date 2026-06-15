#include "workspace/WorkspaceLspClient.h"

#include "workspace/WorkspaceLspClientInternal.h"

namespace microide::workspace {

LspClient::LspClient() : impl_(new Impl{}) {}

LspClient::~LspClient() {
  Shutdown();
  delete impl_;
}

void LspClient::SetWakeEventType(Uint32 event_type) {
  impl_->wake_event_type.store(event_type, std::memory_order_release);
}

bool LspClient::Start(const std::vector<std::string>& command, const std::string& root_uri,
                      const std::string& language_id, const std::string& cwd,
                      const util::JsonValue& initialization_options,
                      const util::JsonValue& settings) {
  util::StartupTrace::Scope trace_scope("LspClient::Start");
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
    if (!impl_->proc.Start(command, cwd)) {
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
  std::vector<std::function<void()>> cbs;
  {
    std::lock_guard lock(impl_->mutex);
    cbs.swap(impl_->ready_callbacks);
  }
  for (auto& cb : cbs) {
    cb();
  }
}

bool LspClient::DidOpen(std::string uri, std::string language_id, std::string text) {
  {
    std::lock_guard lock(impl_->mutex);
    impl_->document_versions[uri] = 1;
  }
  return impl_->SendMessageBuilderAfterInitialize(
      [impl = impl_, uri = std::move(uri), language_id = std::move(language_id),
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
      });
}

bool LspClient::DidChange(const std::string& uri, const std::string& text) {
  using namespace util;
  int version = 0;
  {
    std::lock_guard lock(impl_->mutex);
    version = ++impl_->document_versions[uri];
  }
  JsonObject text_doc;
  text_doc["uri"] = JsonValue(uri);
  text_doc["version"] = JsonValue(static_cast<std::int64_t>(version));
  JsonObject change;
  change["text"] = JsonValue(text);
  JsonArray changes;
  changes.push_back(JsonValue(std::move(change)));
  JsonObject params;
  params["textDocument"] = JsonValue(std::move(text_doc));
  params["contentChanges"] = JsonValue(std::move(changes));
  return impl_->SendMessageAfterInitialize(
      impl_->MakeNotification("textDocument/didChange", JsonValue(std::move(params))));
}

bool LspClient::DidChangeIncremental(const std::string& uri,
                                     Range changed_range,
                                     const std::string& new_text) {
  if (!impl_->supports_incremental_sync) {
    return false;
  }
  using namespace util;
  int version = 0;
  {
    std::lock_guard lock(impl_->mutex);
    version = ++impl_->document_versions[uri];
  }
  JsonObject text_doc;
  text_doc["uri"] = JsonValue(uri);
  text_doc["version"] = JsonValue(static_cast<std::int64_t>(version));

  JsonObject change;
  change["range"] = lsp_protocol::MakeRange(changed_range);
  change["text"] = JsonValue(new_text);

  JsonArray changes;
  changes.push_back(JsonValue(std::move(change)));

  JsonObject params;
  params["textDocument"] = JsonValue(std::move(text_doc));
  params["contentChanges"] = JsonValue(std::move(changes));
  return impl_->SendMessageAfterInitialize(
      impl_->MakeNotification("textDocument/didChange", JsonValue(std::move(params))));
}

bool LspClient::DidSave(const std::string& uri) {
  using namespace util;
  JsonObject text_doc;
  text_doc["uri"] = JsonValue(uri);
  JsonObject params;
  params["textDocument"] = JsonValue(std::move(text_doc));
  return impl_->SendMessageAfterInitialize(
      impl_->MakeNotification("textDocument/didSave", JsonValue(std::move(params))));
}

bool LspClient::DidClose(const std::string& uri) {
  using namespace util;
  {
    std::lock_guard lock(impl_->mutex);
    impl_->document_versions.erase(uri);
  }
  JsonObject text_doc;
  text_doc["uri"] = JsonValue(uri);
  JsonObject params;
  params["textDocument"] = JsonValue(std::move(text_doc));
  return impl_->SendMessageAfterInitialize(
      impl_->MakeNotification("textDocument/didClose", JsonValue(std::move(params))));
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
  impl_->test_document_symbol_handler = nullptr;
}

void LspClient::SetTestDocumentSymbolHandler(
    std::function<void(std::string uri, DocumentSymbolCallback cb)> handler) {
  std::lock_guard lock(impl_->mutex);
  impl_->test_document_symbol_handler = std::move(handler);
}

void LspClient::ClearTestDocumentSymbolHandler() {
  std::lock_guard lock(impl_->mutex);
  impl_->test_document_symbol_handler = nullptr;
}

}  // namespace microide::workspace
