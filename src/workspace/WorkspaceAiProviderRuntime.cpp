#include "workspace/WorkspaceAiProviderRuntime.h"

#include <algorithm>
#include <utility>

namespace microide::workspace {

namespace {

class SidecarAiProviderRuntime final : public AiProviderRuntime {
 public:
  SidecarAiProviderRuntime(WorkspaceProviderBridgeManager& bridge_manager,
                           std::string provider_id,
                           std::vector<std::string> command,
                           std::vector<AiRuntimeWorkflow> workflows)
      : bridge_manager_(bridge_manager),
        provider_id_(std::move(provider_id)),
        command_(std::move(command)),
        workflows_(std::move(workflows)) {}

  const std::string& ProviderId() const override { return provider_id_; }

  bool SupportsWorkflow(AiRuntimeWorkflow workflow) const override {
    return std::find(workflows_.begin(), workflows_.end(), workflow) != workflows_.end();
  }

  bool EnsureReady(const AiRuntimeLaunchContext& context, std::string* error_message) override {
    if (command_.empty()) {
      if (error_message != nullptr) {
        *error_message = "Runtime command is empty";
      }
      return false;
    }
    if (bridge_manager_.IsBridgeRunning(provider_id_)) {
      return true;
    }
    if (!bridge_manager_.StartBridge(provider_id_, command_, context.secret.value_or(""), context.cwd)) {
      if (error_message != nullptr && error_message->empty()) {
        *error_message = "Failed to start provider runtime";
      }
      return false;
    }
    return true;
  }

  bool StartRequest(const AiRuntimeRequest& request,
                    const AiRuntimeLaunchContext& context,
                    std::string* error_message) override {
    if (!EnsureReady(context, error_message)) {
      return false;
    }
    std::vector<std::pair<std::string, std::string>> messages;
    messages.reserve(request.messages.size());
    for (const auto& message : request.messages) {
      messages.emplace_back(message.role, message.content);
    }
    std::vector<WorkspaceProviderBridgeManager::ToolSpec> tools;
    tools.reserve(request.tools.size());
    for (const auto& tool : request.tools) {
      tools.push_back(WorkspaceProviderBridgeManager::ToolSpec{
          .id = tool.id,
          .display_name = tool.display_name,
          .description = tool.description,
          .input_schema = tool.input_schema,
      });
    }
    if (!bridge_manager_.SendChat(provider_id_, request.request_id, messages, request.model_id,
                                  request.system_prompt, request.tool_mode, tools)) {
      if (error_message != nullptr && error_message->empty()) {
        *error_message = "Failed to send runtime request";
      }
      return false;
    }
    return true;
  }

  bool SendToolResult(std::string_view request_id,
                      std::string_view tool_call_id,
                      std::string_view output_json) override {
    return bridge_manager_.SendToolResult(provider_id_, std::string(request_id),
                                          std::string(tool_call_id), std::string(output_json));
  }

  bool SendToolDenied(std::string_view request_id,
                      std::string_view tool_call_id,
                      std::string_view error_message) override {
    return bridge_manager_.SendToolDenied(provider_id_, std::string(request_id),
                                          std::string(tool_call_id), std::string(error_message));
  }

  void CancelRequest(std::string_view request_id) override {
    bridge_manager_.CancelRequest(provider_id_, std::string(request_id));
  }

  bool RequestModelList(const AiRuntimeLaunchContext& context,
                        std::string* error_message) override {
    if (!EnsureReady(context, error_message)) {
      return false;
    }
    bridge_manager_.RequestModelList(provider_id_);
    return true;
  }

  bool RequestAuthCheck(const AiRuntimeLaunchContext& context,
                        std::string* error_message) override {
    if (!EnsureReady(context, error_message)) {
      return false;
    }
    bridge_manager_.RequestAuthCheck(provider_id_);
    return true;
  }

  ProviderAuthStatus AuthStatus() const override {
    return bridge_manager_.GetAuthStatus(provider_id_);
  }

  ProviderCapabilities Capabilities() const override {
    return bridge_manager_.GetCapabilities(provider_id_);
  }

  std::vector<std::string> Models() const override {
    return bridge_manager_.GetModels(provider_id_);
  }

 private:
  WorkspaceProviderBridgeManager& bridge_manager_;
  std::string provider_id_;
  std::vector<std::string> command_;
  std::vector<AiRuntimeWorkflow> workflows_;
};

std::vector<AiRuntimeWorkflow> WorkflowsForAgent(const ExternalAgentSpec& agent) {
  std::vector<AiRuntimeWorkflow> workflows;
  for (const auto& capability : agent.capabilities) {
    if (capability == "chat") {
      workflows.push_back(AiRuntimeWorkflow::Chat);
    } else if (capability == "inline-completion") {
      workflows.push_back(AiRuntimeWorkflow::InlineCompletion);
    }
  }
  return workflows;
}

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
                                                      const ExternalAgentSpec& agent) {
  if (provider.id != agent.id || agent.protocol != "stdio") {
    return;
  }
  auto workflows = WorkflowsForAgent(agent);
  if (workflows.empty()) {
    return;
  }
  RegisterRuntime(std::make_unique<SidecarAiProviderRuntime>(
      bridge_manager_, provider.id, agent.command,
      std::move(workflows)));
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
      *error_message = "No AI provider runtime is registered for " + request.provider_id;
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
  return it != runtimes_.end() &&
         it->second->SendToolResult(request_id, tool_call_id, output_json);
}

bool AiProviderRuntimeService::SendToolDenied(std::string_view provider_id,
                                              std::string_view request_id,
                                              std::string_view tool_call_id,
                                              std::string_view error_message) {
  auto it = runtimes_.find(std::string(provider_id));
  return it != runtimes_.end() &&
         it->second->SendToolDenied(request_id, tool_call_id, error_message);
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
      *error_message = "No AI provider runtime is registered for " + std::string(provider_id);
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
      *error_message = "No AI provider runtime is registered for " + std::string(provider_id);
    }
    return false;
  }
  return it->second->RequestAuthCheck(context, error_message);
}

void AiProviderRuntimeService::StopRuntime(std::string_view provider_id) {
  bridge_manager_.StopBridge(std::string(provider_id));
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
  const auto update = bridge_manager_.ConsumeChatUpdate();
  if (!update.has_value()) {
    return std::nullopt;
  }
  AiRuntimeEvent event;
  event.provider_id = update->agent_id;
  event.request_id = update->request_id;
  event.chunk = update->chunk;
  event.status_text = update->status_text;
  event.terminal_status = update->terminal_status;
  event.tool_call_id = update->tool_call_id;
  event.tool_id = update->tool_id;
  event.display_name = update->display_name;
  event.arguments_json = update->arguments_json;
  event.arguments_summary = update->arguments_summary;
  event.capability_scope = update->capability_scope;
  switch (update->kind) {
    case WorkspaceProviderBridgeManager::ChatUpdate::Kind::Chunk:
      event.kind = AiRuntimeEvent::Kind::Chunk;
      break;
    case WorkspaceProviderBridgeManager::ChatUpdate::Kind::ToolCall:
      event.kind = AiRuntimeEvent::Kind::ToolCall;
      break;
    case WorkspaceProviderBridgeManager::ChatUpdate::Kind::Done:
      event.kind = AiRuntimeEvent::Kind::Completed;
      break;
  }
  return event;
}

}  // namespace microide::workspace
