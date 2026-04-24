#pragma once

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "workspace/WorkspaceAiProvider.h"

namespace microide::workspace {

// WorkspaceProviderBridgeManager manages long-lived bridge subprocesses for AI providers.
//
// Each bridge is identified by an agent_id and communicates over a newline-delimited
// JSON protocol on stdin/stdout.
//
// Protocol (host → bridge):
//   {"type":"initialize","api_key":"..."}\n
//   {"type":"chat","request_id":"r1","model":"...","system_prompt":"...","tool_mode":"no_tools",
//    "messages":[{"role":"user","content":"..."}],"tools":[...]}\n
//   {"type":"tool_result","request_id":"r1","tool_call_id":"call-1","output":"{}"}\n
//   {"type":"tool_denied","request_id":"r1","tool_call_id":"call-1","error":"..."}\n
//   {"type":"cancel","request_id":"r1"}\n
//   {"type":"model_list"}\n
//   {"type":"auth_check"}\n
//   {"type":"shutdown"}\n
//
// Protocol (bridge → host):
//   {"type":"initialized","capabilities":{"chat":true,...},"models":["..."]}\n
//   {"type":"chunk","request_id":"r1","content":"..."}\n
//   {"type":"tool_call","request_id":"r1","tool_call_id":"call-1","tool_id":"...",
//    "display_name":"...","arguments_json":"{}","arguments_summary":"..."}\n
//   {"type":"done","request_id":"r1","status":"succeeded","content":"...","error":""}\n
//   {"type":"auth_status","status":"valid"}\n   // "valid" | "invalid" | "missing"
//   {"type":"model_list","models":["..."]}\n
//
// "done" may carry a full "content" field (non-streaming), or just close a streaming sequence.
class WorkspaceProviderBridgeManager {
 public:
  struct ToolSpec {
    std::string id;
    std::string display_name;
    std::string description;
    std::string input_schema;
  };

  struct ChatUpdate {
    enum class Kind {
      Chunk,
      ToolCall,
      Done,
    };

    Kind kind = Kind::Chunk;
    std::string agent_id;
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

  WorkspaceProviderBridgeManager();
  ~WorkspaceProviderBridgeManager();
  WorkspaceProviderBridgeManager(const WorkspaceProviderBridgeManager&) = delete;
  WorkspaceProviderBridgeManager& operator=(const WorkspaceProviderBridgeManager&) = delete;

  // Register an SDL event type. Call once after SDL_Init.
  void Initialize();

  // Shut down all running bridge processes. Blocks until all reader threads exit.
  void Shutdown();

  // Shut down one running bridge, if present.
  void StopBridge(const std::string& agent_id);

  // Returns true if this event type belongs to the bridge manager.
  bool HandlesEvent(Uint32 type) const;

  // Start (or restart) a bridge for the given agent. Sends the api_key on stdin after launch.
  // Returns false if the bridge command is empty or launch fails.
  bool StartBridge(const std::string& agent_id,
                   const std::vector<std::string>& command,
                   const std::string& api_key,
                   const std::filesystem::path& cwd);

  // Returns true if the bridge for agent_id is running.
  bool IsBridgeRunning(const std::string& agent_id) const;

  // Send a chat request to the bridge for agent_id.
  // Returns false if the bridge is not running.
  bool SendChat(const std::string& agent_id,
                const std::string& request_id,
                const std::vector<std::pair<std::string, std::string>>& messages,
                const std::string& model,
                const std::string& system_prompt,
                const std::string& tool_mode,
                const std::vector<ToolSpec>& tools);

  // Return a successful tool result to the bridge.
  bool SendToolResult(const std::string& agent_id,
                      const std::string& request_id,
                      const std::string& tool_call_id,
                      const std::string& output_json);

  // Tell the bridge that the host denied or failed a tool invocation.
  bool SendToolDenied(const std::string& agent_id,
                      const std::string& request_id,
                      const std::string& tool_call_id,
                      const std::string& error_message);

  // Ask the bridge to cancel the given request.
  void CancelRequest(const std::string& agent_id, const std::string& request_id);

  // Query the bridge for its current model list (async; result comes via future update).
  void RequestModelList(const std::string& agent_id);

  // Request a credential validation from the bridge.
  void RequestAuthCheck(const std::string& agent_id);

  // Consume one pending chat update (non-blocking). Returns nullopt if none available.
  std::optional<ChatUpdate> ConsumeChatUpdate();

  // Accessors — snapshot of last-known bridge state (main thread only).
  ProviderAuthStatus GetAuthStatus(const std::string& agent_id) const;
  ProviderCapabilities GetCapabilities(const std::string& agent_id) const;
  std::vector<std::string> GetModels(const std::string& agent_id) const;

 private:
  struct BridgeEntry;

  // Called on the reader thread for a bridge.
  void ReaderLoop(const std::string& agent_id, BridgeEntry* entry);

  // Parse and dispatch a single newline-terminated JSON message.
  void HandleMessage(const std::string& agent_id, const std::string& line);

  // Append a ChatUpdate to the pending queue and push an SDL wake event.
  void PublishChatUpdate(ChatUpdate update);
  void PushWakeEvent() const;

  // Write a JSON command line to the bridge's stdin.
  bool WriteCommand(const std::string& agent_id, const std::string& json_line);

  Uint32 event_type_ = 0;
  mutable std::mutex mutex_;
  std::map<std::string, std::unique_ptr<BridgeEntry>> bridges_;
  std::vector<ChatUpdate> pending_updates_;
};

}  // namespace microide::workspace
