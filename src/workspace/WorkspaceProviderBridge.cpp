#include "workspace/WorkspaceProviderBridge.h"

#include <SDL3/SDL.h>

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "platform/AsyncSubprocess.h"

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

std::string JsonEscape(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:   out += c;      break;
    }
  }
  return out;
}

// Extract a string value for a top-level key from a flat JSON object.
std::optional<std::string> JsonGetString(std::string_view json, std::string_view key) {
  const std::string needle = "\"" + std::string(key) + "\"";
  std::size_t pos = 0;
  while (pos < json.size()) {
    const auto found = json.find(needle, pos);
    if (found == std::string_view::npos) return std::nullopt;
    pos = found + needle.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    if (pos >= json.size() || json[pos] != ':') continue;
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    if (pos >= json.size() || json[pos] != '"') return std::nullopt;
    ++pos;
    std::string value;
    while (pos < json.size() && json[pos] != '"') {
      if (json[pos] == '\\' && pos + 1 < json.size()) {
        ++pos;
        switch (json[pos]) {
          case '"':  value += '"';  break;
          case '\\': value += '\\'; break;
          case 'n':  value += '\n'; break;
          case 'r':  value += '\r'; break;
          case 't':  value += '\t'; break;
          default:   value += json[pos]; break;
        }
      } else {
        value += json[pos];
      }
      ++pos;
    }
    return value;
  }
  return std::nullopt;
}

// Extract a boolean value for a top-level key.
std::optional<bool> JsonGetBool(std::string_view json, std::string_view key) {
  const std::string needle = "\"" + std::string(key) + "\"";
  const auto found = json.find(needle);
  if (found == std::string_view::npos) return std::nullopt;
  std::size_t pos = found + needle.size();
  while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
  if (pos >= json.size() || json[pos] != ':') return std::nullopt;
  ++pos;
  while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
  if (pos + 4 <= json.size() && json.substr(pos, 4) == "true")  return true;
  if (pos + 5 <= json.size() && json.substr(pos, 5) == "false") return false;
  return std::nullopt;
}

// Extract an array of strings for a top-level key.
std::vector<std::string> JsonGetStringArray(std::string_view json, std::string_view key) {
  const std::string needle = "\"" + std::string(key) + "\"";
  const auto found = json.find(needle);
  if (found == std::string_view::npos) return {};
  std::size_t pos = found + needle.size();
  while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
  if (pos >= json.size() || json[pos] != ':') return {};
  ++pos;
  while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n')) ++pos;
  if (pos >= json.size() || json[pos] != '[') return {};
  ++pos;
  std::vector<std::string> result;
  while (pos < json.size() && json[pos] != ']') {
    while (pos < json.size() && json[pos] != '"' && json[pos] != ']') ++pos;
    if (pos >= json.size() || json[pos] == ']') break;
    ++pos;
    std::string item;
    while (pos < json.size() && json[pos] != '"') {
      if (json[pos] == '\\' && pos + 1 < json.size()) {
        ++pos;
        item += json[pos];
      } else {
        item += json[pos];
      }
      ++pos;
    }
    if (pos < json.size()) ++pos;
    result.push_back(std::move(item));
  }
  return result;
}

// Extract the raw text of a nested JSON object for the given key (e.g. "capabilities":{...}).
// Returns an empty string if not found.
std::string JsonGetObject(std::string_view json, std::string_view key) {
  const std::string needle = "\"" + std::string(key) + "\"";
  const auto found = json.find(needle);
  if (found == std::string_view::npos) return {};
  std::size_t pos = found + needle.size();
  while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
  if (pos >= json.size() || json[pos] != ':') return {};
  ++pos;
  while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
  if (pos >= json.size() || json[pos] != '{') return {};
  const std::size_t start = pos;
  int depth = 0;
  while (pos < json.size()) {
    if (json[pos] == '{') ++depth;
    else if (json[pos] == '}') { --depth; if (depth == 0) { ++pos; break; } }
    ++pos;
  }
  return std::string(json.substr(start, pos - start));
}

// Build the messages JSON array from (role, content) pairs.
std::string BuildMessagesJson(const std::vector<std::pair<std::string, std::string>>& messages) {
  std::string out = "[";
  for (std::size_t i = 0; i < messages.size(); ++i) {
    if (i > 0) out += ',';
    out += "{\"role\":\"";
    out += JsonEscape(messages[i].first);
    out += "\",\"content\":\"";
    out += JsonEscape(messages[i].second);
    out += "\"}";
  }
  out += ']';
  return out;
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
  const std::string init_cmd =
      "{\"type\":\"initialize\",\"api_key\":\"" + JsonEscape(api_key) + "\"}\n";
  WriteCommand(agent_id, init_cmd);
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
    const std::string& tool_mode) {
  const std::string cmd =
      "{\"type\":\"chat\""
      ",\"request_id\":\"" + JsonEscape(request_id) + "\""
      ",\"model\":\"" + JsonEscape(model) + "\""
      ",\"system_prompt\":\"" + JsonEscape(system_prompt) + "\""
      ",\"tool_mode\":\"" + JsonEscape(tool_mode) + "\""
      ",\"messages\":" + BuildMessagesJson(messages) +
      "}\n";
  return WriteCommand(agent_id, cmd);
}

void WorkspaceProviderBridgeManager::CancelRequest(const std::string& agent_id,
                                                   const std::string& request_id) {
  const std::string cmd =
      "{\"type\":\"cancel\",\"request_id\":\"" + JsonEscape(request_id) + "\"}\n";
  WriteCommand(agent_id, cmd);
}

void WorkspaceProviderBridgeManager::RequestModelList(const std::string& agent_id) {
  WriteCommand(agent_id, "{\"type\":\"model_list\"}\n");
}

void WorkspaceProviderBridgeManager::RequestAuthCheck(const std::string& agent_id) {
  WriteCommand(agent_id, "{\"type\":\"auth_check\"}\n");
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
  const auto type = JsonGetString(line, "type");
  if (!type.has_value()) {
    return;
  }

  if (*type == "initialized" || *type == "capabilities") {
    ProviderCapabilities caps;
    const std::string caps_obj = JsonGetObject(line, "capabilities");
    if (!caps_obj.empty()) {
      caps.chat             = JsonGetBool(caps_obj, "chat").value_or(false);
      caps.streaming        = JsonGetBool(caps_obj, "streaming").value_or(false);
      caps.tool_call        = JsonGetBool(caps_obj, "tool_call").value_or(false);
      caps.system_prompt    = JsonGetBool(caps_obj, "system_prompt").value_or(false);
      caps.model_enumeration= JsonGetBool(caps_obj, "model_enumeration").value_or(false);
      caps.structured_output= JsonGetBool(caps_obj, "structured_output").value_or(false);
      caps.image_attachment = JsonGetBool(caps_obj, "image_attachment").value_or(false);
    }
    auto models = JsonGetStringArray(line, "models");
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

  if (*type == "auth_status") {
    const auto status_str = JsonGetString(line, "status");
    ProviderAuthStatus auth = ProviderAuthStatus::Unknown;
    if (status_str) {
      if (*status_str == "valid")   auth = ProviderAuthStatus::KeyValid;
      else if (*status_str == "invalid") auth = ProviderAuthStatus::KeyInvalid;
      else if (*status_str == "missing") auth = ProviderAuthStatus::KeyMissing;
      else auth = ProviderAuthStatus::KeyPresent;
    }
    std::lock_guard lock(mutex_);
    auto it = bridges_.find(agent_id);
    if (it != bridges_.end()) {
      it->second->auth_status = auth;
    }
    return;
  }

  if (*type == "model_list") {
    auto models = JsonGetStringArray(line, "models");
    std::lock_guard lock(mutex_);
    auto it = bridges_.find(agent_id);
    if (it != bridges_.end()) {
      it->second->models = std::move(models);
    }
    return;
  }

  if (*type == "chunk") {
    const auto request_id = JsonGetString(line, "request_id");
    const auto content    = JsonGetString(line, "content");
    if (request_id && content) {
      ChatUpdate update;
      update.agent_id    = agent_id;
      update.request_id  = *request_id;
      update.chunk       = *content;
      update.finished    = false;
      update.succeeded   = false;
      PublishChatUpdate(std::move(update));
    }
    return;
  }

  if (*type == "done") {
    const auto request_id = JsonGetString(line, "request_id");
    if (!request_id) return;
    ChatUpdate update;
    update.agent_id   = agent_id;
    update.request_id = *request_id;
    // Non-streaming bridges may carry the full reply in "content".
    const auto content = JsonGetString(line, "content");
    if (content) {
      update.chunk = *content;
    }
    update.finished    = true;
    update.succeeded   = JsonGetBool(line, "success").value_or(false);
    update.status_text = JsonGetString(line, "error").value_or("");
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
