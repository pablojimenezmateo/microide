#include "workspace/WorkspaceAiProviderRuntime.h"

#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

#include "util/JsonValue.h"

namespace microide::workspace {
namespace {

using microide::util::JsonArray;
using microide::util::JsonObject;
using microide::util::JsonValue;

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

struct HttpResponse {
  long status_code = 0;
  std::string body;
  std::string error;
};

std::string TrimTrailingSlash(std::string text) {
  while (!text.empty() && text.back() == '/') {
    text.pop_back();
  }
  return text;
}

std::string JoinUrl(std::string base_url, std::string_view path) {
  base_url = TrimTrailingSlash(std::move(base_url));
  if (path.empty()) {
    return base_url;
  }
  if (base_url.empty()) {
    return std::string(path);
  }
  return base_url + std::string(path);
}

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

std::vector<std::string> FallbackModels(std::string_view runtime_kind) {
  if (runtime_kind == "anthropic_messages") {
    return {"claude-sonnet-4-6", "claude-haiku-4-5"};
  }
  if (runtime_kind == "deepseek") {
    return {"deepseek-chat", "deepseek-reasoner"};
  }
  return {"gpt-4.1-mini", "gpt-4o-mini"};
}

std::string ErrorMessageFromJson(const JsonValue& payload) {
  if (payload.IsObject()) {
    const JsonValue& error = payload["error"];
    if (error.IsString()) {
      return error.AsString();
    }
    if (error.IsObject() && error["message"].IsString()) {
      return error["message"].AsString();
    }
    if (payload["message"].IsString()) {
      return payload["message"].AsString();
    }
  }
  return {};
}

size_t CurlWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
  if (userdata == nullptr) {
    return 0;
  }
  const size_t total = size * nmemb;
  auto* out = static_cast<std::string*>(userdata);
  out->append(ptr, total);
  return total;
}

int CurlProgressCallback(void* clientp,
                         curl_off_t,
                         curl_off_t,
                         curl_off_t,
                         curl_off_t) {
  if (clientp == nullptr) {
    return 0;
  }
  auto* cancelled = static_cast<std::atomic<bool>*>(clientp);
  return cancelled->load() ? 1 : 0;
}

HttpResponse PerformRequest(const std::string& method,
                            const std::string& url,
                            const std::vector<std::string>& headers,
                            const std::string& body,
                            std::atomic<bool>* cancelled) {
  HttpResponse response;
  CURL* curl = curl_easy_init();
  if (curl == nullptr) {
    response.error = "Failed to initialize HTTP client";
    return response;
  }

  struct curl_slist* header_list = nullptr;
  for (const auto& header : headers) {
    header_list = curl_slist_append(header_list, header.c_str());
  }

  char error_buffer[CURL_ERROR_SIZE] = {};
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &CurlWriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "microide-ai-runtime/1");
  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);

  if (cancelled != nullptr) {
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &CurlProgressCallback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, cancelled);
  }

  if (method == "POST") {
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  }

  const CURLcode code = curl_easy_perform(curl);
  if (code != CURLE_OK) {
    if (code == CURLE_ABORTED_BY_CALLBACK && cancelled != nullptr && cancelled->load()) {
      response.error = "Cancelled";
    } else {
      response.error = error_buffer[0] != '\0' ? std::string(error_buffer) : curl_easy_strerror(code);
    }
  }
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status_code);

  curl_slist_free_all(header_list);
  curl_easy_cleanup(curl);
  return response;
}

std::vector<std::string> ParseModelIds(const JsonValue& payload) {
  std::vector<std::string> models;
  const JsonValue& data = payload["data"];
  if (!data.IsArray()) {
    return models;
  }

  for (const JsonValue& item : data.AsArray()) {
    if (!item.IsObject()) {
      continue;
    }
    const std::string id = item["id"].AsString();
    if (!id.empty()) {
      models.push_back(id);
    }
  }
  return models;
}

class DirectHttpAiProviderRuntime final : public AiProviderRuntime {
 public:
  struct Config {
    std::string provider_id;
    std::string runtime_kind;
    std::string base_url;
    std::string default_model;
  };

  explicit DirectHttpAiProviderRuntime(Config config,
                                       std::function<void(AiRuntimeEvent)> emit_event)
      : config_(std::move(config)),
        emit_event_(std::move(emit_event)) {
    capabilities_.chat = true;
    capabilities_.streaming = true;
    capabilities_.tool_call = true;
    capabilities_.system_prompt = true;
    capabilities_.model_enumeration = true;
  }

  ~DirectHttpAiProviderRuntime() override {
    {
      std::lock_guard lock(requests_mutex_);
      for (auto& [_, request] : active_requests_) {
        request->cancelled.store(true);
      }
    }
    for (auto& worker : workers_) {
      if (worker.joinable()) {
        worker.join();
      }
    }
  }

  const std::string& ProviderId() const override { return config_.provider_id; }

  bool SupportsWorkflow(AiRuntimeWorkflow workflow) const override {
    return workflow == AiRuntimeWorkflow::Chat || workflow == AiRuntimeWorkflow::InlineCompletion;
  }

  bool EnsureReady(const AiRuntimeLaunchContext&, std::string*) override {
    return true;
  }

  bool StartRequest(const AiRuntimeRequest& request,
                    const AiRuntimeLaunchContext& context,
                    std::string* error_message) override {
    if (context.secret.value_or("").empty()) {
      if (error_message != nullptr) {
        *error_message = "Missing API key";
      }
      return false;
    }
    auto active_request = std::make_shared<ActiveRequest>();
    {
      std::lock_guard lock(requests_mutex_);
      active_requests_[request.request_id] = active_request;
    }
    workers_.emplace_back([this, request, context, active_request]() mutable {
      RunRequest(request, context, std::move(active_request));
      std::lock_guard lock(requests_mutex_);
      active_requests_.erase(request.request_id);
    });
    return true;
  }

  bool SendToolResult(std::string_view request_id,
                      std::string_view tool_call_id,
                      std::string_view output_json) override {
    auto request = FindRequest(request_id);
    if (request == nullptr) {
      return false;
    }
    {
      std::lock_guard lock(request->mutex);
      request->tool_responses[std::string(tool_call_id)] = ToolResponse{
          .denied = false,
          .output_json = std::string(output_json),
      };
    }
    request->cv.notify_all();
    return true;
  }

  bool SendToolDenied(std::string_view request_id,
                      std::string_view tool_call_id,
                      std::string_view error_message) override {
    auto request = FindRequest(request_id);
    if (request == nullptr) {
      return false;
    }
    {
      std::lock_guard lock(request->mutex);
      request->tool_responses[std::string(tool_call_id)] = ToolResponse{
          .denied = true,
          .error = std::string(error_message),
      };
    }
    request->cv.notify_all();
    return true;
  }

  void CancelRequest(std::string_view request_id) override {
    if (auto request = FindRequest(request_id); request != nullptr) {
      request->cancelled.store(true);
      request->cv.notify_all();
    }
  }

  bool RequestModelList(const AiRuntimeLaunchContext& context,
                        std::string* error_message) override {
    if (context.secret.value_or("").empty()) {
      auth_status_ = ProviderAuthStatus::KeyMissing;
      if (error_message != nullptr) {
        *error_message = "Missing API key";
      }
      return false;
    }

    const std::vector<std::string> headers = BuildHeaders(*context.secret);
    const HttpResponse response = PerformRequest(
        "GET", JoinUrl(config_.base_url, "/v1/models"), headers, {}, nullptr);
    if (!response.error.empty()) {
      if (error_message != nullptr) {
        *error_message = response.error;
      }
      return false;
    }
    if (response.status_code == 401 || response.status_code == 403) {
      auth_status_ = ProviderAuthStatus::KeyInvalid;
      if (error_message != nullptr) {
        *error_message = "Invalid API key";
      }
      return false;
    }
    if (response.status_code < 200 || response.status_code >= 300) {
      if (error_message != nullptr) {
        *error_message = "Model enumeration failed with HTTP " + std::to_string(response.status_code);
      }
      return false;
    }
    const auto payload = util::ParseJson(response.body);
    if (!payload.has_value()) {
      if (error_message != nullptr) {
        *error_message = "Provider returned malformed JSON";
      }
      return false;
    }
    auto models = ParseModelIds(*payload);
    if (models.empty()) {
      models = FallbackModels(config_.runtime_kind);
    }
    models_ = std::move(models);
    auth_status_ = ProviderAuthStatus::KeyValid;
    return true;
  }

  bool RequestAuthCheck(const AiRuntimeLaunchContext& context,
                        std::string* error_message) override {
    if (context.secret.value_or("").empty()) {
      auth_status_ = ProviderAuthStatus::KeyMissing;
      if (error_message != nullptr) {
        *error_message = "Missing API key";
      }
      return false;
    }
    std::string model_error;
    const bool ok = RequestModelList(context, &model_error);
    if (!ok && auth_status_ == ProviderAuthStatus::Unknown) {
      auth_status_ = ProviderAuthStatus::KeyPresent;
    }
    if (!ok && error_message != nullptr) {
      *error_message = model_error;
    }
    return ok;
  }

  ProviderAuthStatus AuthStatus() const override { return auth_status_; }

  ProviderCapabilities Capabilities() const override { return capabilities_; }

  std::vector<std::string> Models() const override {
    return models_.empty() ? FallbackModels(config_.runtime_kind) : models_;
  }

 private:
  struct ToolResponse {
    bool denied = false;
    std::string output_json;
    std::string error;
  };

  struct ActiveRequest {
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<bool> cancelled{false};
    std::unordered_map<std::string, ToolResponse> tool_responses;
  };

  std::shared_ptr<ActiveRequest> FindRequest(std::string_view request_id) {
    std::lock_guard lock(requests_mutex_);
    auto it = active_requests_.find(std::string(request_id));
    return it == active_requests_.end() ? nullptr : it->second;
  }

  std::vector<std::string> BuildHeaders(const std::string& api_key) const {
    std::vector<std::string> headers;
    headers.emplace_back("Content-Type: application/json");
    if (config_.runtime_kind == "anthropic_messages") {
      headers.emplace_back("x-api-key: " + api_key);
      headers.emplace_back("anthropic-version: 2023-06-01");
    } else {
      headers.emplace_back("Authorization: Bearer " + api_key);
    }
    return headers;
  }

  std::string ResolveModel(const AiRuntimeRequest& request) const {
    if (!request.model_id.empty()) {
      return request.model_id;
    }
    if (!config_.default_model.empty()) {
      return config_.default_model;
    }
    if (!models_.empty()) {
      return models_.front();
    }
    const auto fallback = FallbackModels(config_.runtime_kind);
    return fallback.empty() ? std::string{} : fallback.front();
  }

  JsonArray BuildOpenAiMessages(const AiRuntimeRequest& request) const {
    JsonArray result;
    if (!request.system_prompt.empty()) {
      JsonObject system_message;
      system_message["role"] = "system";
      system_message["content"] = request.system_prompt;
      result.push_back(JsonValue(std::move(system_message)));
    }
    for (const auto& message : request.messages) {
      JsonObject entry;
      entry["role"] = message.role;
      entry["content"] = message.content;
      result.push_back(JsonValue(std::move(entry)));
    }
    return result;
  }

  JsonArray BuildAnthropicMessages(const AiRuntimeRequest& request) const {
    JsonArray result;
    for (const auto& message : request.messages) {
      JsonObject entry;
      entry["role"] = message.role;
      entry["content"] = message.content;
      result.push_back(JsonValue(std::move(entry)));
    }
    return result;
  }

  JsonArray BuildOpenAiToolPayload(const AiRuntimeRequest& request) const {
    JsonArray result;
    for (const auto& tool : request.tools) {
      JsonObject function;
      function["name"] = tool.id;
      function["description"] = tool.description;
      if (const auto schema = util::ParseJson(tool.input_schema); schema.has_value()) {
        function["parameters"] = *schema;
      } else {
        function["parameters"] = JsonObject{};
      }
      JsonObject item;
      item["type"] = "function";
      item["function"] = JsonValue(std::move(function));
      result.push_back(JsonValue(std::move(item)));
    }
    return result;
  }

  JsonArray BuildAnthropicToolPayload(const AiRuntimeRequest& request) const {
    JsonArray result;
    for (const auto& tool : request.tools) {
      JsonObject item;
      item["name"] = tool.id;
      item["description"] = tool.description;
      if (const auto schema = util::ParseJson(tool.input_schema); schema.has_value()) {
        item["input_schema"] = *schema;
      } else {
        item["input_schema"] = JsonObject{};
      }
      result.push_back(JsonValue(std::move(item)));
    }
    return result;
  }

  std::optional<ToolResponse> WaitForToolResponse(const std::shared_ptr<ActiveRequest>& request,
                                                  std::string_view tool_call_id) {
    std::unique_lock lock(request->mutex);
    request->cv.wait(lock, [&]() {
      return request->cancelled.load() ||
             request->tool_responses.find(std::string(tool_call_id)) != request->tool_responses.end();
    });
    if (request->cancelled.load()) {
      return std::nullopt;
    }
    auto it = request->tool_responses.find(std::string(tool_call_id));
    if (it == request->tool_responses.end()) {
      return std::nullopt;
    }
    ToolResponse response = std::move(it->second);
    request->tool_responses.erase(it);
    return response;
  }

  void EmitCompleted(std::string_view request_id,
                     std::string status,
                     std::string status_text,
                     std::string chunk = {}) {
    emit_event_(AiRuntimeEvent{
        .kind = AiRuntimeEvent::Kind::Completed,
        .provider_id = config_.provider_id,
        .request_id = std::string(request_id),
        .chunk = std::move(chunk),
        .status_text = std::move(status_text),
        .terminal_status = std::move(status),
    });
  }

  bool HandleOpenAiToolCalls(JsonValue* parsed,
                             JsonArray* messages,
                             const AiRuntimeRequest& request,
                             const std::shared_ptr<ActiveRequest>& active_request,
                             std::string_view request_id) {
    const JsonValue& choices = (*parsed)["choices"];
    if (!choices.IsArray() || choices.AsArray().empty()) {
      return false;
    }
    const JsonValue& tool_calls = choices[0]["message"]["tool_calls"];
    if (!tool_calls.IsArray() || tool_calls.AsArray().empty()) {
      return false;
    }

    for (const JsonValue& tool_call : tool_calls.AsArray()) {
      const std::string tool_call_id = tool_call["id"].AsString();
      const std::string tool_id = tool_call["function"]["name"].AsString();
      const std::string arguments_json = tool_call["function"]["arguments"].AsString();
      if (tool_call_id.empty() || tool_id.empty()) {
        continue;
      }

      std::string display_name = tool_id;
      for (const auto& tool : request.tools) {
        if (tool.id == tool_id) {
          display_name = tool.display_name.empty() ? tool.id : tool.display_name;
          break;
        }
      }
      emit_event_(AiRuntimeEvent{
          .kind = AiRuntimeEvent::Kind::ToolCall,
          .provider_id = config_.provider_id,
          .request_id = std::string(request_id),
          .tool_call_id = tool_call_id,
          .tool_id = tool_id,
          .display_name = display_name,
          .arguments_json = arguments_json,
          .arguments_summary = arguments_json,
          .capability_scope = tool_id,
      });

      const auto response = WaitForToolResponse(active_request, tool_call_id);
      if (!response.has_value()) {
        EmitCompleted(request_id, "cancelled", "Cancelled");
        return true;
      }
      if (response->denied) {
        EmitCompleted(request_id, "failed", response->error.empty() ? "Tool call denied" : response->error);
        return true;
      }

      JsonObject tool_message;
      tool_message["role"] = "tool";
      tool_message["tool_call_id"] = tool_call_id;
      tool_message["content"] = response->output_json;
      messages->push_back(JsonValue(std::move(tool_message)));
    }
    return true;
  }

  bool HandleAnthropicToolCalls(JsonValue* parsed,
                                JsonArray* messages,
                                const AiRuntimeRequest& request,
                                const std::shared_ptr<ActiveRequest>& active_request,
                                std::string_view request_id) {
    const JsonValue& content = (*parsed)["content"];
    if (!content.IsArray()) {
      return false;
    }

    JsonArray tool_results;
    bool saw_tool = false;
    for (const JsonValue& part : content.AsArray()) {
      if (!part.IsObject() || part["type"].AsString() != "tool_use") {
        continue;
      }
      saw_tool = true;
      const std::string tool_call_id = part["id"].AsString();
      const std::string tool_id = part["name"].AsString();
      const std::string arguments_json = util::SerializeJson(part["input"]);
      if (tool_call_id.empty() || tool_id.empty()) {
        continue;
      }

      std::string display_name = tool_id;
      for (const auto& tool : request.tools) {
        if (tool.id == tool_id) {
          display_name = tool.display_name.empty() ? tool.id : tool.display_name;
          break;
        }
      }
      emit_event_(AiRuntimeEvent{
          .kind = AiRuntimeEvent::Kind::ToolCall,
          .provider_id = config_.provider_id,
          .request_id = std::string(request_id),
          .tool_call_id = tool_call_id,
          .tool_id = tool_id,
          .display_name = display_name,
          .arguments_json = arguments_json,
          .arguments_summary = arguments_json,
          .capability_scope = tool_id,
      });

      const auto response = WaitForToolResponse(active_request, tool_call_id);
      if (!response.has_value()) {
        EmitCompleted(request_id, "cancelled", "Cancelled");
        return true;
      }
      if (response->denied) {
        EmitCompleted(request_id, "failed", response->error.empty() ? "Tool call denied" : response->error);
        return true;
      }

      JsonObject tool_result;
      tool_result["type"] = "tool_result";
      tool_result["tool_use_id"] = tool_call_id;
      tool_result["content"] = response->output_json;
      tool_results.push_back(JsonValue(std::move(tool_result)));
    }

    if (!saw_tool) {
      return false;
    }

    JsonObject assistant_message;
    assistant_message["role"] = "assistant";
    assistant_message["content"] = content;
    messages->push_back(JsonValue(std::move(assistant_message)));

    JsonObject result_message;
    result_message["role"] = "user";
    result_message["content"] = JsonValue(std::move(tool_results));
    messages->push_back(JsonValue(std::move(result_message)));
    return true;
  }

  void RunRequest(const AiRuntimeRequest& request,
                  const AiRuntimeLaunchContext& context,
                  const std::shared_ptr<ActiveRequest>& active_request) {
    if (context.secret.value_or("").empty()) {
      EmitCompleted(request.request_id, "failed", "Missing API key");
      return;
    }

    const std::vector<std::string> headers = BuildHeaders(*context.secret);
    JsonArray openai_messages = BuildOpenAiMessages(request);
    JsonArray anthropic_messages = BuildAnthropicMessages(request);

    while (true) {
      if (active_request->cancelled.load()) {
        EmitCompleted(request.request_id, "cancelled", "Cancelled");
        return;
      }

      JsonObject payload;
      payload["model"] = ResolveModel(request);

      std::string path;
      if (config_.runtime_kind == "anthropic_messages") {
        path = "/v1/messages";
        payload["messages"] = anthropic_messages;
        if (!request.system_prompt.empty()) {
          payload["system"] = request.system_prompt;
        }
        payload["stream"] = false;
        if (!request.tools.empty()) {
          payload["tools"] = BuildAnthropicToolPayload(request);
        }
      } else {
        path = "/v1/chat/completions";
        payload["messages"] = openai_messages;
        payload["stream"] = false;
        if (!request.tools.empty()) {
          payload["tools"] = BuildOpenAiToolPayload(request);
        }
      }

      const HttpResponse response = PerformRequest("POST", JoinUrl(config_.base_url, path), headers,
                                                   util::SerializeJson(JsonValue(payload)),
                                                   &active_request->cancelled);
      if (!response.error.empty()) {
        if (response.error == "Cancelled") {
          EmitCompleted(request.request_id, "cancelled", "Cancelled");
        } else {
          EmitCompleted(request.request_id, "failed", response.error);
        }
        return;
      }

      if (response.status_code == 401 || response.status_code == 403) {
        auth_status_ = ProviderAuthStatus::KeyInvalid;
      } else {
        auth_status_ = ProviderAuthStatus::KeyValid;
      }

      const auto parsed = util::ParseJson(response.body);
      if (!parsed.has_value()) {
        EmitCompleted(request.request_id, "failed", "Provider returned malformed JSON");
        return;
      }

      if (response.status_code < 200 || response.status_code >= 300) {
        const std::string message = ErrorMessageFromJson(*parsed);
        EmitCompleted(request.request_id, "failed",
                      message.empty() ? "Provider request failed with HTTP " + std::to_string(response.status_code)
                                      : message);
        return;
      }

      JsonValue parsed_value = *parsed;
      const bool handled_tool_calls =
          config_.runtime_kind == "anthropic_messages"
              ? HandleAnthropicToolCalls(&parsed_value, &anthropic_messages, request, active_request,
                                         request.request_id)
              : HandleOpenAiToolCalls(&parsed_value, &openai_messages, request, active_request,
                                      request.request_id);
      if (handled_tool_calls) {
        if (active_request->cancelled.load()) {
          EmitCompleted(request.request_id, "cancelled", "Cancelled");
          return;
        }
        if (config_.runtime_kind == "anthropic_messages") {
          bool has_tool_use = false;
          if ((*parsed)["content"].IsArray()) {
            for (const JsonValue& part : (*parsed)["content"].AsArray()) {
              if (part.IsObject() && part["type"].AsString() == "tool_use") {
                has_tool_use = true;
                break;
              }
            }
          }
          if (has_tool_use) {
            continue;
          }
        } else if ((*parsed)["choices"][0]["message"]["tool_calls"].IsArray() &&
                   !(*parsed)["choices"][0]["message"]["tool_calls"].AsArray().empty()) {
          continue;
        }
      }

      std::string chunk;
      if (config_.runtime_kind == "anthropic_messages") {
        const JsonValue& content = (*parsed)["content"];
        if (content.IsArray()) {
          for (const JsonValue& part : content.AsArray()) {
            if (part.IsObject() && part["type"].AsString() == "text") {
              chunk += part["text"].AsString();
            }
          }
        }
      } else {
        const JsonValue& choices = (*parsed)["choices"];
        if (choices.IsArray() && !choices.AsArray().empty()) {
          chunk = choices[0]["message"]["content"].AsString();
        }
      }
      emit_event_(AiRuntimeEvent{
          .kind = AiRuntimeEvent::Kind::Chunk,
          .provider_id = config_.provider_id,
          .request_id = request.request_id,
          .chunk = chunk,
      });
      EmitCompleted(request.request_id, "succeeded", "OK", chunk);
      return;
    }
  }

  Config config_;
  std::function<void(AiRuntimeEvent)> emit_event_;
  mutable std::mutex requests_mutex_;
  std::unordered_map<std::string, std::shared_ptr<ActiveRequest>> active_requests_;
  std::vector<std::thread> workers_;
  std::atomic<ProviderAuthStatus> auth_status_{ProviderAuthStatus::Unknown};
  std::vector<std::string> models_;
  ProviderCapabilities capabilities_{};
};

std::string DefaultBaseUrl(std::string_view runtime_kind) {
  if (runtime_kind == "anthropic_messages") {
    return "https://api.anthropic.com";
  }
  if (runtime_kind == "deepseek") {
    return "https://api.deepseek.com";
  }
  return "https://api.openai.com";
}

std::string NormalizeRuntimeKind(const AiProviderSpec& provider) {
  if (!provider.runtime.empty()) {
    return provider.runtime;
  }
  if (provider.id.find("anthropic") != std::string::npos) {
    return "anthropic_messages";
  }
  if (provider.id.find("deepseek") != std::string::npos) {
    return "deepseek";
  }
  return "openai_compat";
}

}  // namespace

AiProviderRuntimeService::AiProviderRuntimeService() = default;
AiProviderRuntimeService::~AiProviderRuntimeService() {
  Shutdown();
}

void AiProviderRuntimeService::Initialize() {
  bridge_manager_.Initialize();
  if (direct_runtime_event_type_ == 0) {
    direct_runtime_event_type_ = SDL_RegisterEvents(1);
  }
}

void AiProviderRuntimeService::Shutdown() {
  ClearRuntimes();
  bridge_manager_.Shutdown();
}

bool AiProviderRuntimeService::HandlesEvent(Uint32 type) const {
  return bridge_manager_.HandlesEvent(type) ||
         (direct_runtime_event_type_ != 0 && type == direct_runtime_event_type_);
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
                                                      const ExternalAgentSpec& agent) {
  if (provider.id != agent.id || agent.protocol != "stdio") {
    return;
  }
  auto workflows = WorkflowsForAgent(agent);
  if (workflows.empty()) {
    return;
  }
  RegisterRuntime(std::make_unique<SidecarAiProviderRuntime>(bridge_manager_, provider.id,
                                                              agent.command, std::move(workflows)));
}

void AiProviderRuntimeService::RegisterDirectRuntime(const AiProviderSpec& provider) {
  const std::string runtime_kind = NormalizeRuntimeKind(provider);
  if (runtime_kind == "sidecar") {
    return;
  }

  RegisterRuntime(std::make_unique<DirectHttpAiProviderRuntime>(
      DirectHttpAiProviderRuntime::Config{
          .provider_id = provider.id,
          .runtime_kind = runtime_kind,
          .base_url = provider.base_url.empty() ? DefaultBaseUrl(runtime_kind) : provider.base_url,
          .default_model = provider.default_model,
      },
      [this](AiRuntimeEvent event) {
        {
          std::lock_guard lock(event_mutex_);
          pending_events_.push(std::move(event));
        }
        if (direct_runtime_event_type_ != 0) {
          SDL_Event wake{};
          wake.type = direct_runtime_event_type_;
          SDL_PushEvent(&wake);
        }
      }));
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
  {
    std::lock_guard lock(event_mutex_);
    if (!pending_events_.empty()) {
      AiRuntimeEvent event = std::move(pending_events_.front());
      pending_events_.pop();
      return event;
    }
  }

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
