#include "provider/CloudProviderBridge.h"

#include <curl/curl.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
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
  BridgeOptions options;
  std::string api_key;
  std::vector<std::string> cached_models;
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
  capabilities["tool_call"] = false;
  capabilities["system_prompt"] = true;
  capabilities["model_enumeration"] = true;
  capabilities["structured_output"] = false;
  capabilities["image_attachment"] = false;
  return capabilities;
}

void WriteJsonLine(const JsonValue& value) {
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
  WriteJsonLine(JsonValue(std::move(response)));
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
  WriteJsonLine(JsonValue(std::move(response)));
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
  WriteJsonLine(JsonValue(std::move(response)));
}

void HandleChat(ProviderState* state, const JsonValue& request) {
  JsonObject response;
  response["type"] = "done";
  response["request_id"] = request["request_id"].AsString();

  if (state == nullptr || state->api_key.empty()) {
    response["success"] = false;
    response["error"] = "Missing API key";
    response["content"] = "";
    WriteJsonLine(JsonValue(std::move(response)));
    return;
  }

  JsonObject payload;
  payload["model"] = ResolveModel(*state, request);
  std::vector<std::string> headers;

  std::string url;
  if (state->options.provider == "anthropic") {
    payload["max_tokens"] = static_cast<std::int64_t>(2048);
    const std::string system_prompt = request["system_prompt"].AsString();
    if (!system_prompt.empty()) {
      payload["system"] = system_prompt;
    }
    payload["messages"] = JsonValue(BuildAnthropicMessages(request));
    url = JoinUrl(state->options.base_url, "/v1/messages");
    headers.emplace_back("x-api-key: " + state->api_key);
    headers.emplace_back("anthropic-version: 2023-06-01");
    headers.emplace_back("content-type: application/json");
  } else {
    payload["messages"] = JsonValue(BuildOpenAiMessages(request));
    payload["stream"] = false;
    url = JoinUrl(state->options.base_url, "/v1/chat/completions");
    headers.emplace_back("Authorization: Bearer " + state->api_key);
    headers.emplace_back("content-type: application/json");
  }

  const HttpResponse http_response =
      PerformRequest("POST", url, headers, util::SerializeJson(JsonValue(std::move(payload))));
  if (!http_response.error.empty()) {
    response["success"] = false;
    response["error"] = http_response.error;
    response["content"] = "";
    WriteJsonLine(JsonValue(std::move(response)));
    return;
  }

  const auto parsed = util::ParseJson(http_response.body);
  if (!parsed.has_value()) {
    response["success"] = false;
    response["error"] = "Provider returned malformed JSON";
    response["content"] = "";
    WriteJsonLine(JsonValue(std::move(response)));
    return;
  }

  if (http_response.status_code < 200 || http_response.status_code >= 300) {
    std::string error = ErrorMessageFromJson(*parsed);
    if (error.empty()) {
      error = "Provider request failed with HTTP " + std::to_string(http_response.status_code);
    }
    response["success"] = false;
    response["error"] = error;
    response["content"] = "";
    WriteJsonLine(JsonValue(std::move(response)));
    return;
  }

  std::string content;
  if (state->options.provider == "anthropic") {
    content = ParseAnthropicContent(*parsed);
  } else {
    content = ParseOpenAiContent(*parsed);
  }
  response["success"] = true;
  response["error"] = "";
  response["content"] = content;
  WriteJsonLine(JsonValue(std::move(response)));
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

  ProviderState state{
      .options =
          BridgeOptions{
              .provider = options.provider,
              .base_url = TrimTrailingSlash(options.base_url),
              .default_model = options.default_model.empty() ? FallbackModel(options.provider)
                                                             : options.default_model,
          },
      .api_key = {},
      .cached_models = FallbackModels(options.provider),
  };

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
      HandleChat(&state, *request);
      continue;
    }
    if (type == "cancel") {
      continue;
    }
    if (type == "shutdown") {
      break;
    }
  }

  curl_global_cleanup();
  return 0;
}

}  // namespace microide::provider
