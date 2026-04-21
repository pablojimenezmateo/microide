#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <filesystem>

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
  bool Load();
  bool Save() const;

  // Until we add OS credential manager backends, secrets live in a per-user local store.
  std::filesystem::path storage_path_;
  std::unordered_map<std::string, std::string> storage_;
};

}  // namespace microide::workspace
