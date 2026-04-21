#include "workspace/WorkspaceAuthProvider.h"

#include <algorithm>

namespace microide::workspace {

AuthProviderRegistry::AuthProviderRegistry() = default;
AuthProviderRegistry::~AuthProviderRegistry() = default;

void AuthProviderRegistry::RegisterProvider(const AuthProviderSpec& spec) {
  providers_.push_back(spec);
}

const AuthProviderSpec* AuthProviderRegistry::GetProvider(const std::string& id) const {
  for (const auto& provider : providers_) {
    if (provider.id == id) {
      return &provider;
    }
  }
  return nullptr;
}

void AuthProviderRegistry::AddSession(const AuthSession& session) { sessions_.push_back(session); }

std::vector<AuthSession> AuthProviderRegistry::GetSessions(const std::string& provider_id) const {
  std::vector<AuthSession> result;
  for (const auto& session : sessions_) {
    if (session.provider_id == provider_id) {
      result.push_back(session);
    }
  }
  return result;
}

const AuthSession* AuthProviderRegistry::GetSession(const std::string& session_id) const {
  for (const auto& session : sessions_) {
    if (session.id == session_id) {
      return &session;
    }
  }
  return nullptr;
}

void AuthProviderRegistry::RemoveSession(const std::string& session_id) {
  sessions_.erase(std::remove_if(sessions_.begin(), sessions_.end(),
                                 [&](const AuthSession& s) { return s.id == session_id; }),
                  sessions_.end());
}

void AuthProviderRegistry::Clear() {
  providers_.clear();
  sessions_.clear();
}

}  // namespace microide::workspace
