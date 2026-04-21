#pragma once

#include <string>
#include <vector>

namespace microide::workspace {

// Auth provider: handles authentication for services (GitHub, GitLab, etc.).
struct AuthProviderSpec {
  std::string id;
  std::string label;
  std::string plugin_id;
};

// Auth session: represents an active authentication session.
struct AuthSession {
  std::string id;
  std::string provider_id;
  std::string account;  // user account identifier
  std::string access_token;
  std::vector<std::string> scopes;
};

// Auth provider registry: manages authentication providers and sessions.
class AuthProviderRegistry {
 public:
  AuthProviderRegistry();
  ~AuthProviderRegistry();

  // Register an auth provider.
  void RegisterProvider(const AuthProviderSpec& spec);
  const std::vector<AuthProviderSpec>& Providers() const { return providers_; }

  // Get provider by id.
  const AuthProviderSpec* GetProvider(const std::string& id) const;

  // Add auth session.
  void AddSession(const AuthSession& session);
  const std::vector<AuthSession>& Sessions() const { return sessions_; }

  // Get sessions for a provider.
  std::vector<AuthSession> GetSessions(const std::string& provider_id) const;

  // Get session by id.
  const AuthSession* GetSession(const std::string& session_id) const;

  // Remove session.
  void RemoveSession(const std::string& session_id);

  // Clear all sessions and providers.
  void Clear();

 private:
  std::vector<AuthProviderSpec> providers_;
  std::vector<AuthSession> sessions_;
};

}  // namespace microide::workspace
