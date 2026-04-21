#include "workspace/WorkspaceSecretStorage.h"

namespace microide::workspace {

SecretStorage::SecretStorage() = default;
SecretStorage::~SecretStorage() = default;

bool SecretStorage::Store(const std::string& key, const std::string& value) {
  storage_[key] = value;
  return true;
}

std::optional<std::string> SecretStorage::Retrieve(const std::string& key) const {
  auto it = storage_.find(key);
  if (it == storage_.end()) {
    return std::nullopt;
  }
  return it->second;
}

bool SecretStorage::Contains(const std::string& key) const {
  return storage_.find(key) != storage_.end();
}

bool SecretStorage::Delete(const std::string& key) {
  auto it = storage_.find(key);
  if (it == storage_.end()) {
    return false;
  }
  storage_.erase(it);
  return true;
}

std::vector<std::string> SecretStorage::Keys() const {
  std::vector<std::string> result;
  for (const auto& [key, _] : storage_) {
    result.push_back(key);
  }
  return result;
}

void SecretStorage::Clear() { storage_.clear(); }

}  // namespace microide::workspace
