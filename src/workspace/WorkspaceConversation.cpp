#include "workspace/WorkspaceConversation.h"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace microide::workspace {

namespace {
std::string GetTimestamp() {
  auto now = std::chrono::system_clock::now();
  auto time = std::chrono::system_clock::to_time_t(now);
  std::stringstream ss;
  ss << std::put_time(std::gmtime(&time), "%Y-%m-%dT%H:%M:%SZ");
  return ss.str();
}

std::string GenerateId() {
  static int counter = 0;
  return "conv_" + std::to_string(++counter);
}
}  // namespace

ConversationRegistry::ConversationRegistry() = default;
ConversationRegistry::~ConversationRegistry() = default;

std::string ConversationRegistry::CreateConversation(const std::string& title,
                                                     const std::string& provider_id) {
  Conversation conv;
  conv.id = GenerateId();
  conv.title = title;
  conv.provider_id = provider_id;
  conv.created_at = GetTimestamp();
  conv.updated_at = conv.created_at;
  conversations_.push_back(conv);
  return conv.id;
}

Conversation* ConversationRegistry::GetConversation(const std::string& id) {
  for (auto& conv : conversations_) {
    if (conv.id == id) {
      return &conv;
    }
  }
  return nullptr;
}

const Conversation* ConversationRegistry::GetConversation(const std::string& id) const {
  for (const auto& conv : conversations_) {
    if (conv.id == id) {
      return &conv;
    }
  }
  return nullptr;
}

void ConversationRegistry::AddMessage(const std::string& conversation_id,
                                      const Message& message) {
  auto conv = GetConversation(conversation_id);
  if (conv != nullptr) {
    conv->messages.push_back(message);
    conv->updated_at = GetTimestamp();
  }
}

std::vector<Conversation> ConversationRegistry::GetAllConversations() const {
  return conversations_;
}

void ConversationRegistry::DeleteConversation(const std::string& id) {
  conversations_.erase(
      std::remove_if(conversations_.begin(), conversations_.end(),
                     [&](const Conversation& c) { return c.id == id; }),
      conversations_.end());
}

void ConversationRegistry::Clear() { conversations_.clear(); }

}  // namespace microide::workspace
