#include "workspace/WorkspaceConversation.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <string_view>

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

std::string_view MessageRolePrefix(MessageRole role) {
  switch (role) {
    case MessageRole::User:
      return "You";
    case MessageRole::Assistant:
      return "Assistant";
    case MessageRole::System:
    default:
      return "System";
  }
}

std::string CollapseWhitespaceForRender(std::string_view text) {
  std::string collapsed;
  collapsed.reserve(text.size());
  bool space = false;
  for (unsigned char c : text) {
    if (std::isspace(c)) {
      space = !collapsed.empty();
      continue;
    }
    if (space) {
      collapsed.push_back(' ');
      space = false;
    }
    collapsed.push_back(static_cast<char>(c));
  }
  return collapsed;
}

std::string BuildRenderLine(MessageRole role, std::string_view content) {
  const std::string_view prefix = MessageRolePrefix(role);
  const std::string collapsed = CollapseWhitespaceForRender(content);
  std::string line;
  line.reserve(prefix.size() + 2 + collapsed.size());
  line += prefix;
  line += ": ";
  line += collapsed;
  return line;
}
}  // namespace

bool IsTerminalRequestStatus(RequestStatus status) {
  return status == RequestStatus::Succeeded || status == RequestStatus::Failed ||
         status == RequestStatus::Cancelled || status == RequestStatus::Idle;
}

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
    Message stored = message;
    stored.render_line = BuildRenderLine(stored.role, stored.content);
    conv->messages.push_back(std::move(stored));
    conv->updated_at = GetTimestamp();
  }
}

void ConversationRegistry::SetConversations(std::vector<Conversation> conversations) {
  conversations_ = std::move(conversations);
}

void ConversationRegistry::DeleteConversation(const std::string& id) {
  conversations_.erase(
      std::remove_if(conversations_.begin(), conversations_.end(),
                     [&](const Conversation& c) { return c.id == id; }),
      conversations_.end());
}

void ConversationRegistry::Clear() { conversations_.clear(); }

}  // namespace microide::workspace
