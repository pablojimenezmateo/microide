#include "provider/CloudProviderBridge.h"

#include <curl/curl.h>

#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "util/JsonValue.h"

namespace microide::provider {
namespace {

using microide::util::JsonArray;
using microide::util::JsonObject;
using microide::util::JsonValue;

struct HttpResponse {
  long status_code = 0;
  std::string body;
  std::string error;
};

struct ProviderState {
  struct ToolResponse {
    bool denied = false;
    std::string output_json;
    std::string error;
  };

  struct ActiveRequest {
    std::mutex mutex;
    std::condition_variable cv;
    bool cancelled = false;
    std::unordered_map<std::string, ToolResponse> tool_responses;
  };

  BridgeOptions options;
  std::string api_key;
  std::vector<std::string> cached_models;
  std::mutex write_mutex;
  std::mutex requests_mutex;
  std::unordered_map<std::string, std::shared_ptr<ActiveRequest>> active_requests;
  std::vector<std::thread> worker_threads;
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

std::vector<std::string> FallbackModels(std::string_view provider) {
  if (provider == "anthropic") {
    return {"claude-sonnet-4-6", "claude-haiku-4-5"};
  }
  return {"gpt-4.1-mini", "gpt-4o-mini"};
}

std::string FallbackModel(std::string_view provider) {
  const auto models = FallbackModels(provider);
  return models.empty() ? std::string{} : models.front();
}

JsonObject BuildCapabilities() {
  JsonObject capabilities;
  capabilities["chat"] = true;
  capabilities["streaming"] = false;
  capabilities["tool_call"] = true;
  capabilities["system_prompt"] = true;
  capabilities["model_enumeration"] = true;
  capabilities["structured_output"] = false;
  capabilities["image_attachment"] = false;
  return capabilities;
}

void WriteJsonLine(ProviderState* state, const JsonValue& value) {
  std::lock_guard lock(state->write_mutex);
  std::cout << util::SerializeJson(value) << '\n';
  std::cout.flush();
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

HttpResponse PerformRequest(const std::string& method,
                            const std::string& url,
                            const std::vector<std::string>& headers,
                            const std::string& body) {
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
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "microide-provider-bridge/1");
  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);

  if (method == "POST") {
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  }

  const CURLcode code = curl_easy_perform(curl);
  if (code != CURLE_OK) {
    response.error = error_buffer[0] != '\0' ? std::string(error_buffer) : curl_easy_strerror(code);
  }
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status_code);

  curl_slist_free_all(header_list);
  curl_easy_cleanup(curl);
  return response;
}

std::vector<std::string> ParseModelIds(std::string_view provider, const JsonValue& payload) {
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
    if (id.empty()) {
      continue;
    }
    if (provider == "openai") {
      models.push_back(id);
      continue;
    }

    const std::string type = item["type"].AsString();
    if (type.empty() || type == "model") {
      models.push_back(id);
    }
  }
  return models;
}

std::string ParseOpenAiContent(const JsonValue& payload) {
  const JsonValue& choices = payload["choices"];
  if (!choices.IsArray() || choices.AsArray().empty()) {
    return {};
  }
  const JsonValue& message = choices[0]["message"];
  const JsonValue& content = message["content"];
  if (content.IsString()) {
    return content.AsString();
  }
  if (!content.IsArray()) {
    return {};
  }

  std::string text;
  for (const JsonValue& part : content.AsArray()) {
    if (!part.IsObject()) {
      continue;
    }
    if (part["type"].AsString() != "text") {
      continue;
    }
    text += part["text"].AsString();
  }
  return text;
}

std::string ParseAnthropicContent(const JsonValue& payload) {
  const JsonValue& content = payload["content"];
  if (!content.IsArray()) {
    return {};
  }

  std::string text;
  for (const JsonValue& part : content.AsArray()) {
    if (!part.IsObject()) {
      continue;
    }
    if (part["type"].AsString() != "text") {
      continue;
    }
    text += part["text"].AsString();
  }
  return text;
}

std::vector<std::string> FetchModels(ProviderState* state,
                                     std::string* auth_status,
                                     std::string* error_message) {
  if (auth_status != nullptr) {
    *auth_status = "missing";
  }
  if (error_message != nullptr) {
    error_message->clear();
  }
  if (state == nullptr || state->api_key.empty()) {
    return state != nullptr ? FallbackModels(state->options.provider) : std::vector<std::string>{};
  }

  std::vector<std::string> headers;
  if (state->options.provider == "anthropic") {
    headers.emplace_back("x-api-key: " + state->api_key);
    headers.emplace_back("anthropic-version: 2023-06-01");
  } else {
    headers.emplace_back("Authorization: Bearer " + state->api_key);
  }

  const HttpResponse response = PerformRequest(
      "GET",
      JoinUrl(state->options.base_url, "/v1/models"),
      headers,
      {});
  if (!response.error.empty()) {
    if (auth_status != nullptr) {
      *auth_status = "present";
    }
    if (error_message != nullptr) {
      *error_message = response.error;
    }
    return FallbackModels(state->options.provider);
  }
  if (response.status_code == 401 || response.status_code == 403) {
    if (auth_status != nullptr) {
      *auth_status = "invalid";
    }
    if (const auto payload = util::ParseJson(response.body); payload.has_value()) {
      if (error_message != nullptr) {
        *error_message = ErrorMessageFromJson(*payload);
      }
    }
    return FallbackModels(state->options.provider);
  }
  if (response.status_code < 200 || response.status_code >= 300) {
    if (auth_status != nullptr) {
      *auth_status = "present";
    }
    if (const auto payload = util::ParseJson(response.body); payload.has_value()) {
      if (error_message != nullptr) {
        *error_message = ErrorMessageFromJson(*payload);
      }
    }
    if (error_message != nullptr && error_message->empty()) {
      *error_message = "Model enumeration failed with HTTP " + std::to_string(response.status_code);
    }
    return FallbackModels(state->options.provider);
  }

  const auto payload = util::ParseJson(response.body);
  if (!payload.has_value()) {
    if (auth_status != nullptr) {
      *auth_status = "present";
    }
    if (error_message != nullptr) {
      *error_message = "Provider returned malformed JSON";
    }
    return FallbackModels(state->options.provider);
  }

  std::vector<std::string> models = ParseModelIds(state->options.provider, *payload);
  if (models.empty()) {
    models = FallbackModels(state->options.provider);
  }
  if (auth_status != nullptr) {
    *auth_status = "valid";
  }
  state->cached_models = models;
  return models;
}

std::string ResolveModel(const ProviderState& state, const JsonValue& request) {
  const std::string model = request["model"].AsString();
  if (!model.empty()) {
    return model;
  }
  if (!state.options.default_model.empty()) {
    return state.options.default_model;
  }
  if (!state.cached_models.empty()) {
    return state.cached_models.front();
  }
  return FallbackModel(state.options.provider);
}

JsonArray BuildOpenAiMessages(const JsonValue& request) {
  JsonArray result;
  const std::string system_prompt = request["system_prompt"].AsString();
  if (!system_prompt.empty()) {
    JsonObject system_message;
    system_message["role"] = "system";
    system_message["content"] = system_prompt;
    result.push_back(JsonValue(std::move(system_message)));
  }

  const JsonValue& messages = request["messages"];
  if (!messages.IsArray()) {
    return result;
  }
  for (const JsonValue& entry : messages.AsArray()) {
    if (!entry.IsObject()) {
      continue;
    }
    JsonObject message;
    message["role"] = entry["role"].AsString();
    message["content"] = entry["content"].AsString();
    result.push_back(JsonValue(std::move(message)));
  }
  return result;
}

JsonArray BuildAnthropicMessages(const JsonValue& request) {
  JsonArray result;
  const JsonValue& messages = request["messages"];
  if (!messages.IsArray()) {
    return result;
  }
  for (const JsonValue& entry : messages.AsArray()) {
    if (!entry.IsObject()) {
      continue;
    }
    JsonObject message;
    message["role"] = entry["role"].AsString();
    message["content"] = entry["content"].AsString();
    result.push_back(JsonValue(std::move(message)));
  }
  return result;
}

struct ProviderToolSpec {
  std::string id;
  std::string display_name;
  std::string description;
  JsonValue input_schema = JsonObject{};
};

std::vector<ProviderToolSpec> ParseToolSpecs(const JsonValue& request) {
  std::vector<ProviderToolSpec> tools;
  const JsonValue& values = request["tools"];
  if (!values.IsArray()) {
    return tools;
  }
  for (const JsonValue& entry : values.AsArray()) {
    if (!entry.IsObject()) {
      continue;
    }
    ProviderToolSpec tool;
    tool.id = entry["id"].AsString();
    tool.display_name = entry["display_name"].AsString();
    tool.description = entry["description"].AsString();
    if (const auto schema = util::ParseJson(entry["input_schema"].AsString());
        schema.has_value() && schema->IsObject()) {
      tool.input_schema = *schema;
    } else {
      tool.input_schema = JsonObject{};
    }
    if (!tool.id.empty()) {
      tools.push_back(std::move(tool));
    }
  }
  return tools;
}

std::string CollapseWhitespace(std::string_view text) {
  std::string out;
  bool in_space = false;
  for (char ch : text) {
    const bool whitespace = ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
    if (whitespace) {
      if (!in_space && !out.empty()) {
        out.push_back(' ');
      }
      in_space = true;
      continue;
    }
    in_space = false;
    out.push_back(ch);
  }
  return out;
}

std::string SummarizeArguments(std::string_view arguments_json) {
  std::string summary = CollapseWhitespace(arguments_json);
  constexpr std::size_t kMaxSummaryLength = 160;
  if (summary.size() > kMaxSummaryLength) {
    summary.resize(kMaxSummaryLength - 3);
    summary += "...";
  }
  return summary;
}

std::shared_ptr<ProviderState::ActiveRequest> FindActiveRequest(ProviderState* state,
                                                                std::string_view request_id) {
  if (state == nullptr || request_id.empty()) {
    return nullptr;
  }
  std::lock_guard lock(state->requests_mutex);
  const auto it = state->active_requests.find(std::string(request_id));
  return it != state->active_requests.end() ? it->second : nullptr;
}

std::optional<ProviderState::ToolResponse> WaitForToolResponse(
    const std::shared_ptr<ProviderState::ActiveRequest>& request,
    std::string_view tool_call_id) {
  if (request == nullptr || tool_call_id.empty()) {
    return std::nullopt;
  }

  std::unique_lock lock(request->mutex);
  request->cv.wait(lock, [&]() {
    return request->cancelled ||
           request->tool_responses.find(std::string(tool_call_id)) != request->tool_responses.end();
  });
  if (request->cancelled) {
    return std::nullopt;
  }
  auto it = request->tool_responses.find(std::string(tool_call_id));
  if (it == request->tool_responses.end()) {
    return std::nullopt;
  }
  ProviderState::ToolResponse response = std::move(it->second);
  request->tool_responses.erase(it);
  return response;
}

bool EmitToolCall(ProviderState* state,
                  std::string_view request_id,
                  const ProviderToolSpec& tool,
                  std::string_view tool_call_id,
                  std::string arguments_json) {
  if (state == nullptr || request_id.empty() || tool_call_id.empty() || tool.id.empty()) {
    return false;
  }

  JsonObject payload;
  payload["type"] = "tool_call";
  payload["request_id"] = std::string(request_id);
  payload["tool_call_id"] = std::string(tool_call_id);
  payload["tool_id"] = tool.id;
  payload["display_name"] = !tool.display_name.empty() ? tool.display_name : tool.id;
  payload["arguments_json"] = arguments_json;
  payload["arguments_summary"] = SummarizeArguments(arguments_json);
  payload["capability_scope"] = tool.id;
  WriteJsonLine(state, JsonValue(std::move(payload)));
  return true;
}

std::string ParseDoneError(const JsonValue& payload, long status_code) {
  std::string error = ErrorMessageFromJson(payload);
  if (error.empty()) {
    error = "Provider request failed with HTTP " + std::to_string(status_code);
  }
  return error;
}

JsonArray BuildOpenAiToolPayload(const std::vector<ProviderToolSpec>& tools) {
  JsonArray result;
  result.reserve(tools.size());
  for (const auto& tool : tools) {
    JsonObject function;
    function["name"] = tool.id;
    function["description"] = tool.description;
    function["parameters"] = tool.input_schema;

    JsonObject entry;
    entry["type"] = "function";
    entry["function"] = JsonValue(std::move(function));
    result.push_back(JsonValue(std::move(entry)));
  }
  return result;
}

JsonArray BuildAnthropicToolPayload(const std::vector<ProviderToolSpec>& tools) {
  JsonArray result;
  result.reserve(tools.size());
  for (const auto& tool : tools) {
    JsonObject entry;
    entry["name"] = tool.id;
    entry["description"] = tool.description;
    entry["input_schema"] = tool.input_schema;
    result.push_back(JsonValue(std::move(entry)));
  }
  return result;
}

const ProviderToolSpec* FindToolSpec(const std::vector<ProviderToolSpec>& tools,
                                     std::string_view tool_id) {
  const auto it = std::find_if(tools.begin(), tools.end(), [&](const ProviderToolSpec& tool) {
    return tool.id == tool_id;
  });
  return it != tools.end() ? &*it : nullptr;
}

void HandleInitialize(ProviderState* state, const JsonValue&) {
  JsonObject response;
  response["type"] = "initialized";
  response["capabilities"] = JsonValue(BuildCapabilities());

  std::string auth_status;
  std::string error_message;
  std::vector<std::string> models = FetchModels(state, &auth_status, &error_message);
  JsonArray models_json;
  for (const auto& model : models) {
    models_json.push_back(model);
  }
  response["models"] = JsonValue(std::move(models_json));
  WriteJsonLine(state, JsonValue(std::move(response)));
}

void HandleAuthCheck(ProviderState* state) {
  JsonObject response;
  response["type"] = "auth_status";

  std::string auth_status;
  std::string error_message;
  FetchModels(state, &auth_status, &error_message);
  response["status"] = auth_status.empty() ? "missing" : auth_status;
  if (!error_message.empty()) {
    response["error"] = error_message;
  }
  WriteJsonLine(state, JsonValue(std::move(response)));
}

void HandleModelList(ProviderState* state) {
  JsonObject response;
  response["type"] = "model_list";

  std::string auth_status;
  std::string error_message;
  std::vector<std::string> models = FetchModels(state, &auth_status, &error_message);
  JsonArray models_json;
  for (const auto& model : models) {
    models_json.push_back(model);
  }
  response["models"] = JsonValue(std::move(models_json));
  if (!error_message.empty()) {
    response["error"] = error_message;
  }
  WriteJsonLine(state, JsonValue(std::move(response)));
}

void WriteDoneResponse(ProviderState* state,
                       std::string_view request_id,
                       std::string_view status,
                       std::string content,
                       std::string error) {
  JsonObject response;
  response["type"] = "done";
  response["request_id"] = std::string(request_id);
  response["status"] = std::string(status);
  response["success"] = std::string_view(status) == "succeeded";
  response["content"] = std::move(content);
  response["error"] = std::move(error);
  WriteJsonLine(state, JsonValue(std::move(response)));
}

bool RequestCancelled(const std::shared_ptr<ProviderState::ActiveRequest>& request) {
  if (request == nullptr) {
    return true;
  }
  std::lock_guard lock(request->mutex);
  return request->cancelled;
}

bool HandleOpenAiToolCalls(ProviderState* state,
                           const JsonValue& response,
                           const std::vector<ProviderToolSpec>& tools,
                           JsonArray* messages,
                           const std::shared_ptr<ProviderState::ActiveRequest>& active_request,
                           std::string_view request_id) {
  const JsonValue& choices = response["choices"];
  if (!choices.IsArray() || choices.AsArray().empty()) {
    return false;
  }
  const JsonValue& message = choices[0]["message"];
  const JsonValue& tool_calls = message["tool_calls"];
  if (!tool_calls.IsArray() || tool_calls.AsArray().empty()) {
    return false;
  }

  if (messages != nullptr) {
    messages->push_back(message);
  }

  for (const JsonValue& tool_call : tool_calls.AsArray()) {
    if (!tool_call.IsObject()) {
      continue;
    }
    const std::string tool_call_id = tool_call["id"].AsString();
    const std::string tool_id = tool_call["function"]["name"].AsString();
    const std::string arguments_json = tool_call["function"]["arguments"].AsString();
    const ProviderToolSpec fallback_tool{
        .id = tool_id,
        .display_name = tool_id,
        .description = {},
        .input_schema = JsonObject{},
    };
    const ProviderToolSpec* tool = FindToolSpec(tools, tool_id);
    EmitToolCall(state, request_id, tool != nullptr ? *tool : fallback_tool, tool_call_id,
                 arguments_json);
    const auto tool_response = WaitForToolResponse(active_request, tool_call_id);
    if (!tool_response.has_value()) {
      WriteDoneResponse(state, request_id, "cancelled", {}, "Cancelled");
      return true;
    }
    if (tool_response->denied) {
      WriteDoneResponse(state, request_id, "failed", {}, tool_response->error);
      return true;
    }

    JsonObject tool_message;
    tool_message["role"] = "tool";
    tool_message["tool_call_id"] = tool_call_id;
    tool_message["content"] = tool_response->output_json;
    if (messages != nullptr) {
      messages->push_back(JsonValue(std::move(tool_message)));
    }
  }

  return true;
}

bool HandleAnthropicToolCalls(ProviderState* state,
                              const JsonValue& response,
                              const std::vector<ProviderToolSpec>& tools,
                              JsonArray* messages,
                              const std::shared_ptr<ProviderState::ActiveRequest>& active_request,
                              std::string_view request_id) {
  const JsonValue& content = response["content"];
  if (!content.IsArray()) {
    return false;
  }

  JsonArray tool_results;
  bool saw_tool_call = false;
  for (const JsonValue& part : content.AsArray()) {
    if (!part.IsObject() || part["type"].AsString() != "tool_use") {
      continue;
    }
    saw_tool_call = true;
    const std::string tool_call_id = part["id"].AsString();
    const std::string tool_id = part["name"].AsString();
    const std::string arguments_json = util::SerializeJson(part["input"]);
    const ProviderToolSpec fallback_tool{
        .id = tool_id,
        .display_name = tool_id,
        .description = {},
        .input_schema = JsonObject{},
    };
    const ProviderToolSpec* tool = FindToolSpec(tools, tool_id);
    EmitToolCall(state, request_id, tool != nullptr ? *tool : fallback_tool, tool_call_id,
                 arguments_json);
    const auto tool_response = WaitForToolResponse(active_request, tool_call_id);
    if (!tool_response.has_value()) {
      WriteDoneResponse(state, request_id, "cancelled", {}, "Cancelled");
      return true;
    }
    if (tool_response->denied) {
      WriteDoneResponse(state, request_id, "failed", {}, tool_response->error);
      return true;
    }

    JsonObject tool_result;
    tool_result["type"] = "tool_result";
    tool_result["tool_use_id"] = tool_call_id;
    tool_result["content"] = tool_response->output_json;
    tool_results.push_back(JsonValue(std::move(tool_result)));
  }

  if (!saw_tool_call) {
    return false;
  }

  if (messages != nullptr) {
    JsonObject assistant_message;
    assistant_message["role"] = "assistant";
    assistant_message["content"] = content;
    messages->push_back(JsonValue(std::move(assistant_message)));

    JsonObject result_message;
    result_message["role"] = "user";
    result_message["content"] = JsonValue(std::move(tool_results));
    messages->push_back(JsonValue(std::move(result_message)));
  }
  return true;
}

void HandleChatRequest(ProviderState* state,
                       JsonValue request,
                       std::shared_ptr<ProviderState::ActiveRequest> active_request) {
  const std::string request_id = request["request_id"].AsString();

  if (state == nullptr || state->api_key.empty()) {
    WriteDoneResponse(state, request_id, "failed", {}, "Missing API key");
    return;
  }

  const std::vector<ProviderToolSpec> tools = ParseToolSpecs(request);
  JsonArray openai_messages;
  JsonArray anthropic_messages;
  if (state->options.provider == "anthropic") {
    anthropic_messages = BuildAnthropicMessages(request);
  } else {
    openai_messages = BuildOpenAiMessages(request);
  }

  std::vector<std::string> headers;
  if (state->options.provider == "anthropic") {
    headers.emplace_back("x-api-key: " + state->api_key);
    headers.emplace_back("anthropic-version: 2023-06-01");
    headers.emplace_back("content-type: application/json");
  } else {
    headers.emplace_back("Authorization: Bearer " + state->api_key);
    headers.emplace_back("content-type: application/json");
  }

  while (true) {
    if (RequestCancelled(active_request)) {
      WriteDoneResponse(state, request_id, "cancelled", {}, "Cancelled");
      return;
    }

    JsonObject payload;
    payload["model"] = ResolveModel(*state, request);
    std::string url;
    if (state->options.provider == "anthropic") {
      payload["max_tokens"] = static_cast<std::int64_t>(2048);
      const std::string system_prompt = request["system_prompt"].AsString();
      if (!system_prompt.empty()) {
        payload["system"] = system_prompt;
      }
      payload["messages"] = anthropic_messages;
      if (!tools.empty()) {
        payload["tools"] = BuildAnthropicToolPayload(tools);
      }
      url = JoinUrl(state->options.base_url, "/v1/messages");
    } else {
      payload["messages"] = openai_messages;
      payload["stream"] = false;
      if (!tools.empty()) {
        payload["tools"] = BuildOpenAiToolPayload(tools);
      }
      url = JoinUrl(state->options.base_url, "/v1/chat/completions");
    }

    const HttpResponse http_response =
        PerformRequest("POST", url, headers, util::SerializeJson(JsonValue(std::move(payload))));
    if (!http_response.error.empty()) {
      WriteDoneResponse(state, request_id, "failed", {}, http_response.error);
      return;
    }

    const auto parsed = util::ParseJson(http_response.body);
    if (!parsed.has_value()) {
      WriteDoneResponse(state, request_id, "failed", {}, "Provider returned malformed JSON");
      return;
    }

    if (http_response.status_code < 200 || http_response.status_code >= 300) {
      WriteDoneResponse(state, request_id, "failed", {},
                        ParseDoneError(*parsed, http_response.status_code));
      return;
    }

    const bool handled_tool_call =
        state->options.provider == "anthropic"
            ? HandleAnthropicToolCalls(state, *parsed, tools, &anthropic_messages, active_request,
                                       request_id)
            : HandleOpenAiToolCalls(state, *parsed, tools, &openai_messages, active_request,
                                    request_id);
    if (handled_tool_call) {
      if (RequestCancelled(active_request)) {
        WriteDoneResponse(state, request_id, "cancelled", {}, "Cancelled");
        return;
      }
      if (state->options.provider == "anthropic") {
        const JsonValue& content = (*parsed)["content"];
        bool has_tool_use = false;
        if (content.IsArray()) {
          for (const JsonValue& part : content.AsArray()) {
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

    const std::string content =
        state->options.provider == "anthropic"
            ? ParseAnthropicContent(*parsed)
            : ParseOpenAiContent(*parsed);
    WriteDoneResponse(state, request_id, "succeeded", content, {});
    return;
  }
}

}  // namespace

int RunProviderBridge(const BridgeOptions& options) {
  if (options.provider != "openai" && options.provider != "anthropic") {
    std::cerr << "Unsupported provider: " << options.provider << '\n';
    return 1;
  }

  if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
    std::cerr << "Failed to initialize libcurl\n";
    return 1;
  }

  ProviderState state;
  state.options = BridgeOptions{
      .provider = options.provider,
      .base_url = TrimTrailingSlash(options.base_url),
      .default_model = options.default_model.empty() ? FallbackModel(options.provider)
                                                     : options.default_model,
  };
  state.cached_models = FallbackModels(options.provider);

  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty()) {
      continue;
    }
    const auto request = util::ParseJson(line);
    if (!request.has_value() || !request->IsObject()) {
      continue;
    }

    const std::string type = (*request)["type"].AsString();
    if (type == "initialize") {
      state.api_key = (*request)["api_key"].AsString();
      HandleInitialize(&state, *request);
      continue;
    }
    if (type == "auth_check") {
      HandleAuthCheck(&state);
      continue;
    }
    if (type == "model_list") {
      HandleModelList(&state);
      continue;
    }
    if (type == "chat") {
      const std::string request_id = (*request)["request_id"].AsString();
      if (request_id.empty()) {
        continue;
      }
      auto active_request = std::make_shared<ProviderState::ActiveRequest>();
      {
        std::lock_guard lock(state.requests_mutex);
        state.active_requests[request_id] = active_request;
      }
      state.worker_threads.emplace_back([&state, request = *request, active_request, request_id]() mutable {
        HandleChatRequest(&state, std::move(request), active_request);
        std::lock_guard lock(state.requests_mutex);
        state.active_requests.erase(request_id);
      });
      continue;
    }
    if (type == "tool_result" || type == "tool_denied") {
      const std::string request_id = (*request)["request_id"].AsString();
      const std::string tool_call_id = (*request)["tool_call_id"].AsString();
      if (request_id.empty() || tool_call_id.empty()) {
        continue;
      }
      if (auto active_request = FindActiveRequest(&state, request_id); active_request != nullptr) {
        std::lock_guard lock(active_request->mutex);
        ProviderState::ToolResponse response;
        response.denied = type == "tool_denied";
        response.output_json = (*request)["output"].AsString();
        response.error = (*request)["error"].AsString();
        active_request->tool_responses[tool_call_id] = std::move(response);
        active_request->cv.notify_all();
      }
      continue;
    }
    if (type == "cancel") {
      const std::string request_id = (*request)["request_id"].AsString();
      if (auto active_request = FindActiveRequest(&state, request_id); active_request != nullptr) {
        std::lock_guard lock(active_request->mutex);
        active_request->cancelled = true;
        active_request->cv.notify_all();
      }
      continue;
    }
    if (type == "shutdown") {
      std::lock_guard requests_lock(state.requests_mutex);
      for (const auto& [id, active_request] : state.active_requests) {
        if (active_request == nullptr) {
          continue;
        }
        std::lock_guard active_lock(active_request->mutex);
        active_request->cancelled = true;
        active_request->cv.notify_all();
      }
      break;
    }
  }

  for (auto& worker : state.worker_threads) {
    if (worker.joinable()) {
      worker.join();
    }
  }

  curl_global_cleanup();
  return 0;
}

}  // namespace microide::provider
