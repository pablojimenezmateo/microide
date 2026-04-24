#include "workspace/WorkspaceProviderBridge.h"

#include <SDL3/SDL.h>

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "platform/AsyncSubprocess.h"
#include "util/JsonValue.h"

namespace microide::workspace {

// ---------------------------------------------------------------------------
// BridgeEntry — per-agent process + reader state
// ---------------------------------------------------------------------------

struct WorkspaceProviderBridgeManager::BridgeEntry {
  platform::AsyncSubprocess process;
  std::thread reader_thread;
  std::atomic<bool> stop_requested{false};

  // Protected by the manager's mutex_:
  ProviderAuthStatus auth_status = ProviderAuthStatus::KeyMissing;
  ProviderCapabilities capabilities;
  std::vector<std::string> models;
};

// ---------------------------------------------------------------------------
// Minimal flat-JSON helpers (no external deps)
// ---------------------------------------------------------------------------

namespace {
using microide::util::JsonArray;
using microide::util::JsonObject;
using microide::util::JsonValue;

JsonArray BuildMessagesJson(const std::vector<std::pair<std::string, std::string>>& messages) {
  JsonArray result;
  result.reserve(messages.size());
  for (const auto& [role, content] : messages) {
    JsonObject message;
    message["role"] = role;
    message["content"] = content;
    result.push_back(JsonValue(std::move(message)));
  }
  return result;
}

JsonArray BuildToolsJson(const std::vector<WorkspaceProviderBridgeManager::ToolSpec>& tools) {
  JsonArray result;
  result.reserve(tools.size());
  for (const auto& tool : tools) {
    JsonObject entry;
    entry["id"] = tool.id;
    entry["display_name"] = tool.display_name;
    entry["description"] = tool.description;
    entry["input_schema"] = tool.input_schema;
    result.push_back(JsonValue(std::move(entry)));
  }
  return result;
}

}  // namespace

// ---------------------------------------------------------------------------
// WorkspaceProviderBridgeManager
// ---------------------------------------------------------------------------

WorkspaceProviderBridgeManager::WorkspaceProviderBridgeManager() = default;

WorkspaceProviderBridgeManager::~WorkspaceProviderBridgeManager() {
  Shutdown();
}

void WorkspaceProviderBridgeManager::Initialize() {
  event_type_ = SDL_RegisterEvents(1);
  if (event_type_ == static_cast<Uint32>(-1)) {
    event_type_ = 0;
  }
}

void WorkspaceProviderBridgeManager::Shutdown() {
  std::map<std::string, std::unique_ptr<BridgeEntry>> bridges_to_join;
  {
    std::lock_guard lock(mutex_);
    bridges_to_join = std::move(bridges_);
  }
  for (auto& [id, entry] : bridges_to_join) {
    entry->stop_requested.store(true);
    entry->process.Shutdown(2000);
    if (entry->reader_thread.joinable()) {
      entry->reader_thread.join();
    }
  }
}

void WorkspaceProviderBridgeManager::StopBridge(const std::string& agent_id) {
  std::unique_ptr<BridgeEntry> entry;
  {
    std::lock_guard lock(mutex_);
    auto it = bridges_.find(agent_id);
    if (it == bridges_.end()) {
      return;
    }
    entry = std::move(it->second);
    bridges_.erase(it);
  }
  entry->stop_requested.store(true);
  entry->process.Shutdown(2000);
  if (entry->reader_thread.joinable()) {
    entry->reader_thread.join();
  }
}

bool WorkspaceProviderBridgeManager::HandlesEvent(Uint32 type) const {
  return event_type_ != 0 && type == event_type_;
}

bool WorkspaceProviderBridgeManager::StartBridge(const std::string& agent_id,
                                                 const std::vector<std::string>& command,
                                                 const std::string& api_key,
                                                 const std::filesystem::path& cwd) {
  if (command.empty()) {
    return false;
  }

  // Shut down any existing bridge for this agent.
  {
    std::unique_ptr<BridgeEntry> old_entry;
    {
      std::lock_guard lock(mutex_);
      auto it = bridges_.find(agent_id);
      if (it != bridges_.end()) {
        old_entry = std::move(it->second);
        bridges_.erase(it);
      }
    }
    if (old_entry) {
      old_entry->stop_requested.store(true);
      old_entry->process.Shutdown(2000);
      if (old_entry->reader_thread.joinable()) {
        old_entry->reader_thread.join();
      }
    }
  }

  auto entry = std::make_unique<BridgeEntry>();
  if (!entry->process.Start(command, cwd.string())) {
    return false;
  }

  BridgeEntry* entry_ptr = entry.get();
  entry->reader_thread = std::thread([this, agent_id, entry_ptr]() {
    ReaderLoop(agent_id, entry_ptr);
  });

  {
    std::lock_guard lock(mutex_);
    bridges_[agent_id] = std::move(entry);
  }

  // Send the initialize command with the API key.
  JsonObject init_payload;
  init_payload["type"] = "initialize";
  init_payload["api_key"] = api_key;
  WriteCommand(agent_id, util::SerializeJson(JsonValue(std::move(init_payload))) + "\n");
  return true;
}

bool WorkspaceProviderBridgeManager::IsBridgeRunning(const std::string& agent_id) const {
  std::lock_guard lock(mutex_);
  const auto it = bridges_.find(agent_id);
  return it != bridges_.end() && it->second->process.IsRunning();
}

bool WorkspaceProviderBridgeManager::SendChat(
    const std::string& agent_id,
    const std::string& request_id,
    const std::vector<std::pair<std::string, std::string>>& messages,
    const std::string& model,
    const std::string& system_prompt,
    const std::string& tool_mode,
    const std::vector<ToolSpec>& tools) {
  JsonObject payload;
  payload["type"] = "chat";
  payload["request_id"] = request_id;
  payload["model"] = model;
  payload["system_prompt"] = system_prompt;
  payload["tool_mode"] = tool_mode;
  payload["messages"] = BuildMessagesJson(messages);
  payload["tools"] = BuildToolsJson(tools);
  return WriteCommand(agent_id, util::SerializeJson(JsonValue(std::move(payload))) + "\n");
}

bool WorkspaceProviderBridgeManager::SendToolResult(const std::string& agent_id,
                                                    const std::string& request_id,
                                                    const std::string& tool_call_id,
                                                    const std::string& output_json) {
  JsonObject payload;
  payload["type"] = "tool_result";
  payload["request_id"] = request_id;
  payload["tool_call_id"] = tool_call_id;
  payload["output"] = output_json;
  return WriteCommand(agent_id, util::SerializeJson(JsonValue(std::move(payload))) + "\n");
}

bool WorkspaceProviderBridgeManager::SendToolDenied(const std::string& agent_id,
                                                    const std::string& request_id,
                                                    const std::string& tool_call_id,
                                                    const std::string& error_message) {
  JsonObject payload;
  payload["type"] = "tool_denied";
  payload["request_id"] = request_id;
  payload["tool_call_id"] = tool_call_id;
  payload["error"] = error_message;
  return WriteCommand(agent_id, util::SerializeJson(JsonValue(std::move(payload))) + "\n");
}

void WorkspaceProviderBridgeManager::CancelRequest(const std::string& agent_id,
                                                   const std::string& request_id) {
  JsonObject payload;
  payload["type"] = "cancel";
  payload["request_id"] = request_id;
  WriteCommand(agent_id, util::SerializeJson(JsonValue(std::move(payload))) + "\n");
}

void WorkspaceProviderBridgeManager::RequestModelList(const std::string& agent_id) {
  JsonObject payload;
  payload["type"] = "model_list";
  WriteCommand(agent_id, util::SerializeJson(JsonValue(std::move(payload))) + "\n");
}

void WorkspaceProviderBridgeManager::RequestAuthCheck(const std::string& agent_id) {
  JsonObject payload;
  payload["type"] = "auth_check";
  WriteCommand(agent_id, util::SerializeJson(JsonValue(std::move(payload))) + "\n");
}

std::optional<WorkspaceProviderBridgeManager::ChatUpdate>
WorkspaceProviderBridgeManager::ConsumeChatUpdate() {
  std::lock_guard lock(mutex_);
  if (pending_updates_.empty()) {
    return std::nullopt;
  }
  ChatUpdate update = std::move(pending_updates_.front());
  pending_updates_.erase(pending_updates_.begin());
  return update;
}

ProviderAuthStatus WorkspaceProviderBridgeManager::GetAuthStatus(
    const std::string& agent_id) const {
  std::lock_guard lock(mutex_);
  const auto it = bridges_.find(agent_id);
  if (it == bridges_.end()) return ProviderAuthStatus::Unknown;
  return it->second->auth_status;
}

ProviderCapabilities WorkspaceProviderBridgeManager::GetCapabilities(
    const std::string& agent_id) const {
  std::lock_guard lock(mutex_);
  const auto it = bridges_.find(agent_id);
  if (it == bridges_.end()) return {};
  return it->second->capabilities;
}

std::vector<std::string> WorkspaceProviderBridgeManager::GetModels(
    const std::string& agent_id) const {
  std::lock_guard lock(mutex_);
  const auto it = bridges_.find(agent_id);
  if (it == bridges_.end()) return {};
  return it->second->models;
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

void WorkspaceProviderBridgeManager::ReaderLoop(const std::string& agent_id, BridgeEntry* entry) {
  std::string line_buf;
  while (!entry->stop_requested.load()) {
    const auto data = entry->process.Read(65536, 100);
    if (!data.has_value()) {
      break;  // process exited or fatal error
    }
    if (data->empty()) {
      continue;  // poll timeout — no data yet
    }
    line_buf += *data;
    std::string::size_type pos;
    while ((pos = line_buf.find('\n')) != std::string::npos) {
      const std::string line = line_buf.substr(0, pos);
      line_buf.erase(0, pos + 1);
      if (!line.empty()) {
        HandleMessage(agent_id, line);
      }
    }
  }
}

void WorkspaceProviderBridgeManager::HandleMessage(const std::string& agent_id,
                                                   const std::string& line) {
  const auto parsed = util::ParseJson(line);
  if (!parsed.has_value() || !parsed->IsObject()) {
    return;
  }
  const std::string type = (*parsed)["type"].AsString();
  if (type.empty()) {
    return;
  }

  if (type == "initialized" || type == "capabilities") {
    ProviderCapabilities caps;
    if ((*parsed)["capabilities"].IsObject()) {
      const JsonValue& caps_obj = (*parsed)["capabilities"];
      caps.chat = caps_obj["chat"].AsBool(false);
      caps.streaming = caps_obj["streaming"].AsBool(false);
      caps.tool_call = caps_obj["tool_call"].AsBool(false);
      caps.system_prompt = caps_obj["system_prompt"].AsBool(false);
      caps.model_enumeration = caps_obj["model_enumeration"].AsBool(false);
      caps.structured_output = caps_obj["structured_output"].AsBool(false);
      caps.image_attachment = caps_obj["image_attachment"].AsBool(false);
    }
    std::vector<std::string> models;
    if ((*parsed)["models"].IsArray()) {
      for (const JsonValue& model : (*parsed)["models"].AsArray()) {
        if (model.IsString()) {
          models.push_back(model.AsString());
        }
      }
    }
    std::lock_guard lock(mutex_);
    auto it = bridges_.find(agent_id);
    if (it != bridges_.end()) {
      it->second->capabilities = caps;
      if (!models.empty()) {
        it->second->models = std::move(models);
      }
      // Upgrade auth status from Unknown/Missing to Present when we receive init success.
      if (it->second->auth_status == ProviderAuthStatus::Unknown ||
          it->second->auth_status == ProviderAuthStatus::KeyMissing) {
        it->second->auth_status = ProviderAuthStatus::KeyPresent;
      }
    }
    return;
  }

  if (type == "auth_status") {
    const std::string status_str = (*parsed)["status"].AsString();
    ProviderAuthStatus auth = ProviderAuthStatus::Unknown;
    if (!status_str.empty()) {
      if (status_str == "valid")   auth = ProviderAuthStatus::KeyValid;
      else if (status_str == "invalid") auth = ProviderAuthStatus::KeyInvalid;
      else if (status_str == "missing") auth = ProviderAuthStatus::KeyMissing;
      else auth = ProviderAuthStatus::KeyPresent;
    }
    std::lock_guard lock(mutex_);
    auto it = bridges_.find(agent_id);
    if (it != bridges_.end()) {
      it->second->auth_status = auth;
    }
    return;
  }

  if (type == "model_list") {
    std::vector<std::string> models;
    if ((*parsed)["models"].IsArray()) {
      for (const JsonValue& model : (*parsed)["models"].AsArray()) {
        if (model.IsString()) {
          models.push_back(model.AsString());
        }
      }
    }
    std::lock_guard lock(mutex_);
    auto it = bridges_.find(agent_id);
    if (it != bridges_.end()) {
      it->second->models = std::move(models);
    }
    return;
  }

  if (type == "chunk") {
    const std::string request_id = (*parsed)["request_id"].AsString();
    const std::string content = (*parsed)["content"].AsString();
    if (!request_id.empty()) {
      ChatUpdate update;
      update.kind = ChatUpdate::Kind::Chunk;
      update.agent_id = agent_id;
      update.request_id = request_id;
      update.chunk = content;
      PublishChatUpdate(std::move(update));
    }
    return;
  }

  if (type == "tool_call") {
    const std::string request_id = (*parsed)["request_id"].AsString();
    const std::string tool_call_id = (*parsed)["tool_call_id"].AsString();
    const std::string tool_id = (*parsed)["tool_id"].AsString();
    if (request_id.empty() || tool_call_id.empty() || tool_id.empty()) {
      return;
    }
    ChatUpdate update;
    update.kind = ChatUpdate::Kind::ToolCall;
    update.agent_id = agent_id;
    update.request_id = request_id;
    update.tool_call_id = tool_call_id;
    update.tool_id = tool_id;
    update.display_name = (*parsed)["display_name"].AsString();
    update.arguments_json = (*parsed)["arguments_json"].AsString();
    update.arguments_summary = (*parsed)["arguments_summary"].AsString();
    update.capability_scope = (*parsed)["capability_scope"].AsString();
    PublishChatUpdate(std::move(update));
    return;
  }

  if (type == "done") {
    const std::string request_id = (*parsed)["request_id"].AsString();
    if (request_id.empty()) return;
    ChatUpdate update;
    update.kind = ChatUpdate::Kind::Done;
    update.agent_id = agent_id;
    update.request_id = request_id;
    update.chunk = (*parsed)["content"].AsString();
    update.terminal_status = (*parsed)["status"].AsString();
    if (update.terminal_status.empty()) {
      update.terminal_status = (*parsed)["success"].AsBool(false) ? "succeeded" : "failed";
    }
    update.status_text = (*parsed)["error"].AsString();
    PublishChatUpdate(std::move(update));
    return;
  }

  // Unknown message types are silently ignored.
}

void WorkspaceProviderBridgeManager::PublishChatUpdate(ChatUpdate update) {
  {
    std::lock_guard lock(mutex_);
    pending_updates_.push_back(std::move(update));
  }
  PushWakeEvent();
}

void WorkspaceProviderBridgeManager::PushWakeEvent() const {
  if (event_type_ == 0) {
    return;
  }
  SDL_Event event{};
  event.type = event_type_;
  SDL_PushEvent(&event);
}

bool WorkspaceProviderBridgeManager::WriteCommand(const std::string& agent_id,
                                                  const std::string& json_line) {
  std::lock_guard lock(mutex_);
  auto it = bridges_.find(agent_id);
  if (it == bridges_.end()) {
    return false;
  }
  return it->second->process.Write(json_line);
}

}  // namespace microide::workspace
