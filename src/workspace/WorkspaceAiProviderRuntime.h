#pragma once

#include <SDL3/SDL.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "workspace/WorkspaceAiProvider.h"
#include "workspace/WorkspaceExternalAgent.h"
#include "workspace/WorkspaceProviderBridge.h"

namespace microide::workspace {

enum class AiRuntimeWorkflow {
  Chat,
  InlineCompletion,
};

struct AiRuntimeMessage {
  std::string role;
  std::string content;
};

struct AiRuntimeToolSpec {
  std::string id;
  std::string display_name;
  std::string description;
  std::string input_schema;
};

struct AiRuntimeLaunchContext {
  std::filesystem::path cwd;
  std::optional<std::string> secret;
};

struct AiRuntimeRequest {
  AiRuntimeWorkflow workflow = AiRuntimeWorkflow::Chat;
  std::string provider_id;
  std::string request_id;
  std::vector<AiRuntimeMessage> messages;
  std::string model_id;
  std::string system_prompt;
  std::string tool_mode;
  std::vector<AiRuntimeToolSpec> tools;
};

struct AiRuntimeEvent {
  enum class Kind {
    Chunk,
    ToolCall,
    Completed,
  };

  Kind kind = Kind::Chunk;
  std::string provider_id;
  std::string request_id;
  std::string chunk;
  std::string status_text;
  std::string terminal_status;
  std::string tool_call_id;
  std::string tool_id;
  std::string display_name;
  std::string arguments_json;
  std::string arguments_summary;
  std::string capability_scope;
};

class AiProviderRuntime {
 public:
  virtual ~AiProviderRuntime() = default;

  virtual const std::string& ProviderId() const = 0;
  virtual bool SupportsWorkflow(AiRuntimeWorkflow workflow) const = 0;
  virtual bool EnsureReady(const AiRuntimeLaunchContext& context, std::string* error_message) = 0;
  virtual bool StartRequest(const AiRuntimeRequest& request,
                            const AiRuntimeLaunchContext& context,
                            std::string* error_message) = 0;
  virtual bool SendToolResult(std::string_view request_id,
                              std::string_view tool_call_id,
                              std::string_view output_json) = 0;
  virtual bool SendToolDenied(std::string_view request_id,
                              std::string_view tool_call_id,
                              std::string_view error_message) = 0;
  virtual void CancelRequest(std::string_view request_id) = 0;
  virtual bool RequestModelList(const AiRuntimeLaunchContext& context,
                                std::string* error_message) = 0;
  virtual bool RequestAuthCheck(const AiRuntimeLaunchContext& context,
                                std::string* error_message) = 0;
  virtual ProviderAuthStatus AuthStatus() const = 0;
  virtual ProviderCapabilities Capabilities() const = 0;
  virtual std::vector<std::string> Models() const = 0;
};

class AiProviderRuntimeService {
 public:
  AiProviderRuntimeService();
  ~AiProviderRuntimeService();
  AiProviderRuntimeService(const AiProviderRuntimeService&) = delete;
  AiProviderRuntimeService& operator=(const AiProviderRuntimeService&) = delete;

  void Initialize();
  void Shutdown();
  bool HandlesEvent(Uint32 type) const;

  void ClearRuntimes();
  void RegisterRuntime(std::unique_ptr<AiProviderRuntime> runtime);
  void RegisterSidecarRuntime(const AiProviderSpec& provider, const ExternalAgentSpec& agent);

  std::vector<std::string> ProviderIdsForWorkflow(AiRuntimeWorkflow workflow) const;
  bool ProviderSupportsWorkflow(std::string_view provider_id, AiRuntimeWorkflow workflow) const;

  bool StartRequest(const AiRuntimeRequest& request,
                    const AiRuntimeLaunchContext& context,
                    std::string* error_message);
  bool SendToolResult(std::string_view provider_id,
                      std::string_view request_id,
                      std::string_view tool_call_id,
                      std::string_view output_json);
  bool SendToolDenied(std::string_view provider_id,
                      std::string_view request_id,
                      std::string_view tool_call_id,
                      std::string_view error_message);
  void CancelRequest(std::string_view provider_id, std::string_view request_id);
  bool RequestModelList(std::string_view provider_id,
                        const AiRuntimeLaunchContext& context,
                        std::string* error_message);
  bool RequestAuthCheck(std::string_view provider_id,
                        const AiRuntimeLaunchContext& context,
                        std::string* error_message);
  void StopRuntime(std::string_view provider_id);

  ProviderAuthStatus GetAuthStatus(std::string_view provider_id) const;
  ProviderCapabilities GetCapabilities(std::string_view provider_id) const;
  std::vector<std::string> GetModels(std::string_view provider_id) const;

  std::optional<AiRuntimeEvent> ConsumeEvent();

 private:
  WorkspaceProviderBridgeManager bridge_manager_;
  std::vector<std::string> runtime_order_;
  std::unordered_map<std::string, std::unique_ptr<AiProviderRuntime>> runtimes_;
};

}  // namespace microide::workspace
