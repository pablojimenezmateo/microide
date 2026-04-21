#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace microide::workspace {

// Secret storage: secure storage for credentials, tokens, etc.
// Backed by OS credential manager or encrypted local store.
class SecretStorage {
 public:
  SecretStorage();
  ~SecretStorage();

  // Store a secret.
  bool Store(const std::string& key, const std::string& value);

  // Retrieve a secret.
  std::optional<std::string> Retrieve(const std::string& key) const;

  // Check if a secret exists.
  bool Contains(const std::string& key) const;

  // Delete a secret.
  bool Delete(const std::string& key);

  // List all secret keys.
  std::vector<std::string> Keys() const;

  // Clear all secrets.
  void Clear();

 private:
  // In production, this would use OS credential stores (keychain on macOS, credential manager
  // on Windows, pass on Linux). For now, we use an in-memory unencrypted map.
  std::unordered_map<std::string, std::string> storage_;
};

}  // namespace microide::workspace
