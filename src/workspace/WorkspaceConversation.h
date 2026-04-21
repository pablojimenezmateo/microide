#pragma once

#include <string>
#include <vector>

namespace microide::workspace {

// Chat message.
enum class MessageRole {
  User,
  Assistant,
  System,
};

struct Message {
  std::string id;
  MessageRole role;
  std::string content;
  std::string render_line;  // cached "Role: collapsed-content" for panel rendering
  std::string timestamp;
  std::string model;  // which model/provider generated this (for assistant messages)
};

// Conversation thread: a chat session with history.
struct Conversation {
  std::string id;
  std::string title;
  std::vector<Message> messages;
  std::string created_at;
  std::string updated_at;
  std::string provider_id;  // default AI provider for this conversation
};

// Conversation registry: manages chat threads and history.
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

  // Get all conversations (for sidebar list).
  std::vector<Conversation> GetAllConversations() const;

  // Delete conversation.
  void DeleteConversation(const std::string& id);

  // Clear all conversations.
  void Clear();

 private:
  std::vector<Conversation> conversations_;
};

}  // namespace microide::workspace
