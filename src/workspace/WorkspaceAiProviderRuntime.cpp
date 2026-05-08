#include "workspace/WorkspaceAiProviderRuntime.h"

#include <algorithm>
#include <utility>

namespace microide::workspace {
namespace {

class DisabledAiProviderRuntime final : public AiProviderRuntime {
 public:
  explicit DisabledAiProviderRuntime(std::string provider_id) : provider_id_(std::move(provider_id)) {}

  const std::string& ProviderId() const override { return provider_id_; }
  bool SupportsWorkflow(AiRuntimeWorkflow) const override { return false; }

  bool EnsureReady(const AiRuntimeLaunchContext&, std::string* error_message) override {
    if (error_message != nullptr) {
      *error_message = "AI runtime is retired";
    }
    return false;
  }

  bool StartRequest(const AiRuntimeRequest&, const AiRuntimeLaunchContext&,
                    std::string* error_message) override {
    if (error_message != nullptr) {
      *error_message = "AI runtime is retired";
    }
    return false;
  }

  bool SendToolResult(std::string_view, std::string_view, std::string_view) override {
    return false;
  }
  bool SendToolDenied(std::string_view, std::string_view, std::string_view) override {
    return false;
  }
  void CancelRequest(std::string_view) override {}

  bool RequestModelList(const AiRuntimeLaunchContext&, std::string* error_message) override {
    if (error_message != nullptr) {
      *error_message = "AI runtime is retired";
    }
    return false;
  }
  bool RequestAuthCheck(const AiRuntimeLaunchContext&, std::string* error_message) override {
    if (error_message != nullptr) {
      *error_message = "AI runtime is retired";
    }
    return false;
  }
  ProviderAuthStatus AuthStatus() const override { return ProviderAuthStatus::Unknown; }
  ProviderCapabilities Capabilities() const override { return ProviderCapabilities{}; }
  std::vector<std::string> Models() const override { return {}; }

 private:
  std::string provider_id_;
};

}  // namespace

AiProviderRuntimeService::AiProviderRuntimeService() = default;
AiProviderRuntimeService::~AiProviderRuntimeService() {
  Shutdown();
}

void AiProviderRuntimeService::Initialize() {
  bridge_manager_.Initialize();
}

void AiProviderRuntimeService::Shutdown() {
  ClearRuntimes();
  bridge_manager_.Shutdown();
}

bool AiProviderRuntimeService::HandlesEvent(Uint32 type) const {
  return bridge_manager_.HandlesEvent(type);
}

void AiProviderRuntimeService::ClearRuntimes() {
  runtimes_.clear();
  runtime_order_.clear();
  std::lock_guard lock(event_mutex_);
  pending_events_ = {};
}

void AiProviderRuntimeService::RegisterRuntime(std::unique_ptr<AiProviderRuntime> runtime) {
  if (runtime == nullptr) {
    return;
  }
  const std::string provider_id = runtime->ProviderId();
  if (!runtimes_.contains(provider_id)) {
    runtime_order_.push_back(provider_id);
  }
  runtimes_[provider_id] = std::move(runtime);
}

void AiProviderRuntimeService::RegisterSidecarRuntime(const AiProviderSpec& provider,
                                                      const ExternalAgentSpec&) {
  RegisterRuntime(std::make_unique<DisabledAiProviderRuntime>(provider.id));
}

void AiProviderRuntimeService::RegisterDirectRuntime(const AiProviderSpec& provider) {
  RegisterRuntime(std::make_unique<DisabledAiProviderRuntime>(provider.id));
}

std::vector<std::string> AiProviderRuntimeService::ProviderIdsForWorkflow(
    AiRuntimeWorkflow workflow) const {
  std::vector<std::string> provider_ids;
  for (const auto& provider_id : runtime_order_) {
    auto it = runtimes_.find(provider_id);
    if (it != runtimes_.end() && it->second->SupportsWorkflow(workflow)) {
      provider_ids.push_back(provider_id);
    }
  }
  return provider_ids;
}

bool AiProviderRuntimeService::ProviderSupportsWorkflow(std::string_view provider_id,
                                                        AiRuntimeWorkflow workflow) const {
  auto it = runtimes_.find(std::string(provider_id));
  return it != runtimes_.end() && it->second->SupportsWorkflow(workflow);
}

bool AiProviderRuntimeService::StartRequest(const AiRuntimeRequest& request,
                                            const AiRuntimeLaunchContext& context,
                                            std::string* error_message) {
  auto it = runtimes_.find(request.provider_id);
  if (it == runtimes_.end()) {
    if (error_message != nullptr) {
      *error_message = "AI runtime is retired";
    }
    return false;
  }
  return it->second->StartRequest(request, context, error_message);
}

bool AiProviderRuntimeService::SendToolResult(std::string_view provider_id,
                                              std::string_view request_id,
                                              std::string_view tool_call_id,
                                              std::string_view output_json) {
  auto it = runtimes_.find(std::string(provider_id));
  return it != runtimes_.end() && it->second->SendToolResult(request_id, tool_call_id, output_json);
}

bool AiProviderRuntimeService::SendToolDenied(std::string_view provider_id,
                                              std::string_view request_id,
                                              std::string_view tool_call_id,
                                              std::string_view error_message) {
  auto it = runtimes_.find(std::string(provider_id));
  return it != runtimes_.end() && it->second->SendToolDenied(request_id, tool_call_id, error_message);
}

void AiProviderRuntimeService::CancelRequest(std::string_view provider_id,
                                             std::string_view request_id) {
  auto it = runtimes_.find(std::string(provider_id));
  if (it != runtimes_.end()) {
    it->second->CancelRequest(request_id);
  }
}

bool AiProviderRuntimeService::RequestModelList(std::string_view provider_id,
                                                const AiRuntimeLaunchContext& context,
                                                std::string* error_message) {
  auto it = runtimes_.find(std::string(provider_id));
  if (it == runtimes_.end()) {
    if (error_message != nullptr) {
      *error_message = "AI runtime is retired";
    }
    return false;
  }
  return it->second->RequestModelList(context, error_message);
}

bool AiProviderRuntimeService::RequestAuthCheck(std::string_view provider_id,
                                                const AiRuntimeLaunchContext& context,
                                                std::string* error_message) {
  auto it = runtimes_.find(std::string(provider_id));
  if (it == runtimes_.end()) {
    if (error_message != nullptr) {
      *error_message = "AI runtime is retired";
    }
    return false;
  }
  return it->second->RequestAuthCheck(context, error_message);
}

void AiProviderRuntimeService::StopRuntime(std::string_view provider_id) {
  bridge_manager_.StopBridge(std::string(provider_id));
  auto it = runtimes_.find(std::string(provider_id));
  if (it != runtimes_.end()) {
    it->second->CancelRequest("*");
  }
}

ProviderAuthStatus AiProviderRuntimeService::GetAuthStatus(std::string_view provider_id) const {
  auto it = runtimes_.find(std::string(provider_id));
  return it == runtimes_.end() ? ProviderAuthStatus::Unknown : it->second->AuthStatus();
}

ProviderCapabilities AiProviderRuntimeService::GetCapabilities(std::string_view provider_id) const {
  auto it = runtimes_.find(std::string(provider_id));
  return it == runtimes_.end() ? ProviderCapabilities{} : it->second->Capabilities();
}

std::vector<std::string> AiProviderRuntimeService::GetModels(std::string_view provider_id) const {
  auto it = runtimes_.find(std::string(provider_id));
  return it == runtimes_.end() ? std::vector<std::string>{} : it->second->Models();
}

std::optional<AiRuntimeEvent> AiProviderRuntimeService::ConsumeEvent() {
  std::lock_guard lock(event_mutex_);
  if (pending_events_.empty()) {
    return std::nullopt;
  }
  AiRuntimeEvent event = std::move(pending_events_.front());
  pending_events_.pop();
  return event;
}

}  // namespace microide::workspace
