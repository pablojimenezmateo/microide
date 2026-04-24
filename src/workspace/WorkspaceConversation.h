#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace microide::workspace {

static constexpr int kConversationSchemaVersion = 1;

enum class MessageRole {
  User,
  Assistant,
  System,
};

// Lifecycle state of a single chat request.
enum class RequestStatus {
  Idle,
  Queued,
  Running,
  Streaming,
  Succeeded,
  Failed,
  Cancelled,
};

bool IsTerminalRequestStatus(RequestStatus status);

enum class ToolMode {
  NoTools,
  Ask,
  Auto,
};

struct ToolEvent {
  std::string call_id;
  std::string tool_id;
  std::string display_name;
  std::string arguments_summary;
  std::string status;
  std::string permission_decision;
  std::string capability_scope;
  std::string started_at;
  std::string finished_at;
  std::int64_t duration_ms = 0;
  std::string error;
  std::string output_summary;
};

struct Message {
  std::string id;
  MessageRole role = MessageRole::User;
  std::string content;
  std::string render_line;  // cached "Role: collapsed-content" for panel rendering
  std::string timestamp;
  std::string provider_id;
  std::string model;
  RequestStatus status = RequestStatus::Idle;
  std::int64_t request_duration_ms = 0;
  std::string error;
  std::vector<ToolEvent> tool_events;
  std::string stream_id;
};

// Conversation thread: a chat session with history.
struct Conversation {
  int schema_version = kConversationSchemaVersion;
  std::string id;
  std::string title;
  std::vector<Message> messages;
  std::string created_at;
  std::string updated_at;
  std::string provider_id;
  std::string model_id;
  RequestStatus status = RequestStatus::Idle;
  ToolMode tool_mode = ToolMode::Ask;
  std::string draft;
  std::string system_prompt;
  std::int64_t last_request_duration_ms = 0;
};

// Conversation registry: manages chat threads and history, scoped to one project.
class ConversationRegistry {
 public:
  ConversationRegistry();
  ~ConversationRegistry();

  // Create a new conversation.
  std::string CreateConversation(const std::string& title, const std::string& provider_id);

  // Get conversation by id.
  Conversation* GetConversation(const std::string& id);
  const Conversation* GetConversation(const std::string& id) const;

  // Add message to conversation.
  void AddMessage(const std::string& conversation_id, const Message& message);

  // Get all conversations (for display or persistence).
  const std::vector<Conversation>& conversations() const { return conversations_; }

  // Replace all conversations (used during session restore).
  void SetConversations(std::vector<Conversation> conversations);

  // Delete conversation.
  void DeleteConversation(const std::string& id);

  // Clear all conversations.
  void Clear();

 private:
  std::vector<Conversation> conversations_;
};

}  // namespace microide::workspace
