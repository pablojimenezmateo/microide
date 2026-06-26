#include "workspace/WorkspaceDapClient.h"

#include "workspace/WorkspaceDapClientInternal.h"

namespace microide::workspace {

DapClient::DapClient() : impl_(new Impl{}) {}

DapClient::~DapClient() {
  Shutdown();
  delete impl_;
}

void DapClient::SetWakeEventType(Uint32 event_type) {
  impl_->main_mailbox.SetWakeEventType(event_type);
}

void DapClient::SetEventCallback(EventCallback callback) {
  std::lock_guard lock(impl_->mutex);
  impl_->event_callback = std::move(callback);
}

bool DapClient::Start(const std::vector<std::string>& command, const std::string& adapter_id,
                      const std::string& cwd, const platform::SubprocessSandbox& sandbox) {
  impl_->last_error.clear();
  impl_->adapter_id = adapter_id;

  if (!impl_->proc.Start(command, cwd, sandbox)) {
    impl_->last_error = "failed to start debug adapter process";
    return false;
  }

  impl_->shutdown_started.store(false, std::memory_order_release);
  impl_->shutdown_complete.store(false, std::memory_order_release);
  impl_->shutting_down.store(false, std::memory_order_release);
  impl_->process_shutdown_started.store(false, std::memory_order_release);
  impl_->initialized.store(false, std::memory_order_release);
  impl_->stop_init.store(false);

  Impl* const init_impl = impl_;
  impl_->init_thread = std::thread([init_impl]() { init_impl->DoInitializeBlocking(); });
  return true;
}

bool DapClient::IsRunning() const {
  if (impl_->test_stub_mode.load(std::memory_order_acquire)) {
    return true;
  }
  return impl_->proc.IsRunning();
}

bool DapClient::IsInitializing() const {
  return impl_->initializing.load(std::memory_order_acquire);
}

bool DapClient::IsInitialized() const {
  return impl_->initialized.load(std::memory_order_acquire);
}

dap_protocol::DapCapabilities DapClient::Capabilities() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->capabilities;
}

void DapClient::ApplyCapabilitiesUpdate(const util::JsonValue& capabilities_body) {
  std::lock_guard lock(impl_->mutex);
  dap_protocol::MergeCapabilities(impl_->capabilities, capabilities_body);
}

const std::string& DapClient::LastError() const {
  std::lock_guard lock(impl_->mutex);
  impl_->last_error_snapshot = impl_->last_error;
  return impl_->last_error_snapshot;
}

void DapClient::DrainCallbacks() { impl_->main_mailbox.Drain(); }

bool DapClient::HasPendingRequests() const {
  std::lock_guard lock(impl_->mutex);
  return !impl_->pending_requests.empty() || impl_->main_mailbox.PendingCount() > 0;
}

bool DapClient::SendRequestAsync(const std::string& command, util::JsonValue arguments,
                                 ResponseCallback callback) {
  if (!callback) {
    return impl_->DispatchRequest(
        command, std::move(arguments), [](util::JsonValue) {}, []() {});
  }
  ResponseCallback failure = callback;
  return impl_->DispatchRequest(
      command, std::move(arguments),
      [cb = std::move(callback)](util::JsonValue resp) {
        cb(dap_protocol::ParseResponse(resp));
      },
      [failure = std::move(failure)]() {
        dap_protocol::DapResponse response;
        response.success = false;
        response.command.clear();
        response.message = "failed to send request to debug adapter";
        failure(response);
      });
}

void DapClient::EnableTestStubMode() {
  std::lock_guard lock(impl_->mutex);
  impl_->test_stub_mode.store(true, std::memory_order_release);
  impl_->initialized.store(true, std::memory_order_release);
  impl_->initializing.store(false, std::memory_order_release);
}

void DapClient::DisableTestStubMode() {
  std::lock_guard lock(impl_->mutex);
  impl_->test_stub_mode.store(false, std::memory_order_release);
  impl_->test_request_handler = nullptr;
}

void DapClient::SetTestRequestHandler(
    std::function<void(const std::string& command, const util::JsonValue& arguments,
                       ResponseCallback)> handler) {
  std::lock_guard lock(impl_->mutex);
  impl_->test_request_handler = std::move(handler);
}

void DapClient::InjectTestEvent(const std::string& event, util::JsonValue body) {
  EventCallback cb;
  {
    std::lock_guard lock(impl_->mutex);
    cb = impl_->event_callback;
  }
  if (!cb) {
    return;
  }
  impl_->main_mailbox.Post(
      [cb = std::move(cb), event, body = std::move(body)]() mutable { cb(event, body); });
}

void DapClient::BeginShutdown() {
  impl_->BeginShutdown();
}

void DapClient::Shutdown() {
  impl_->WaitForShutdown();
}

bool DapClient::IsShuttingDown() const {
  return impl_->shutting_down.load(std::memory_order_acquire);
}

bool DapClient::IsShutdownComplete() const {
  return impl_->shutdown_started.load(std::memory_order_acquire) &&
         impl_->shutdown_complete.load(std::memory_order_acquire);
}

}  // namespace microide::workspace
